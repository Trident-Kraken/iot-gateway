/*
 * business_handler 单元测试。
 *
 * 直接编译 src/business/business_handler.c + protocol.c + logger.c，
 * 以本地桩函数替代 mqtt_publish / conn_manager_update_active_time /
 * epoll_worker_request_send，覆盖：
 *   - 同名 value 碰撞（如 {"device_id":"temp","temp":25.5}）
 *   - 非法数值字段安全回退（防拼出无效 JSON）
 *   - device_id 特殊字符/控制字符清洗
 *   - 精确长度、无 NUL 结尾堆缓冲区（ASan 越界读验证）
 *   - CMD_HEARTBEAT / CMD_CONFIG 下行委托 / 未知命令丢弃
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "business/business_handler.h"
#include "business/mqtt_client.h"
#include "gateway/conn_manager.h"
#include "gateway/epoll_worker.h"
#include "protocol/protocol.h"
#include "utils/logger.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* Mock 桩函数                                                          */
/* ------------------------------------------------------------------ */
static int g_mqtt_calls = 0;
static char g_last_topic[64];
static char g_last_mqtt_payload[512];

static int g_hb_calls = 0;
static int g_last_hb_fd = -1;

static int g_downlink_calls = 0;
static int g_last_dl_fd = -1;
static uint64_t g_last_dl_gen = 0;
static uint8_t g_last_dl_frame[PROTOCOL_MAX_FRAME_SIZE];
static size_t g_last_dl_len = 0;

int mqtt_publish(const char *topic, const char *payload)
{
    g_mqtt_calls++;
    snprintf(g_last_topic, sizeof(g_last_topic), "%s", topic ? topic : "");
    snprintf(g_last_mqtt_payload, sizeof(g_last_mqtt_payload),
             "%s", payload ? payload : "");
    return 0;
}

void conn_manager_update_active_time(int fd)
{
    g_hb_calls++;
    g_last_hb_fd = fd;
}

int epoll_worker_request_send(int fd, uint64_t generation,
                              const uint8_t *frame, size_t len)
{
    g_downlink_calls++;
    g_last_dl_fd = fd;
    g_last_dl_gen = generation;
    g_last_dl_len = len;
    if (frame != NULL && len <= sizeof(g_last_dl_frame)) {
        memcpy(g_last_dl_frame, frame, len);
    }
    return 0;
}

static void reset_mocks(void)
{
    g_mqtt_calls = 0;
    g_last_topic[0] = '\0';
    g_last_mqtt_payload[0] = '\0';
    g_hb_calls = 0;
    g_last_hb_fd = -1;
    g_downlink_calls = 0;
    g_last_dl_fd = -1;
    g_last_dl_gen = 0;
    g_last_dl_len = 0;
    memset(g_last_dl_frame, 0, sizeof(g_last_dl_frame));
}

static void make_frame_task(struct task *t, int fd, uint64_t gen,
                            uint8_t cmd, const uint8_t *payload, size_t plen)
{
    struct packet *pkt = malloc(sizeof(*pkt) + plen);

    memset(t, 0, sizeof(*t));
    t->fd = fd;
    t->generation = gen;
    t->cmd = cmd;

    pkt->cmd = cmd;
    pkt->payload_len = (uint16_t)plen;
    if (plen > 0 && payload != NULL) {
        memcpy(pkt->payload, payload, plen);
    }
    CHECK(packet_encode(pkt, t->frame, sizeof(t->frame)) == 0);
    t->frame_len = PACKET_MIN_SIZE + plen;
    free(pkt);
}

/* ------------------------------------------------------------------ */
/* 测试用例                                                            */
/* ------------------------------------------------------------------ */
static void test_same_name_value_collision(void)
{
    const char *json =
        "{\"device_id\":\"temp\",\"temp\":25.5,\"hum\":60,\"timestamp\":1700000000}";

    reset_mocks();
    handle_sensor_data(1, 10, (const uint8_t *)json, strlen(json));

    CHECK(g_mqtt_calls == 1);
    CHECK(strcmp(g_last_topic, "sensor/data") == 0);
    CHECK(strstr(g_last_mqtt_payload, "\"device_id\":\"temp\"") != NULL);
    CHECK(strstr(g_last_mqtt_payload, "\"temp\":25.5,") != NULL);
    CHECK(strstr(g_last_mqtt_payload, "\"hum\":60,") != NULL);
    CHECK(strstr(g_last_mqtt_payload, "\"timestamp\":1700000000,") != NULL);
}

static void test_invalid_numbers_fallback(void)
{
    const char *bad1 =
        "{\"device_id\":\"dev1\",\"temp\":\"abc\",\"hum\":+25,\"timestamp\":---}";
    const char *bad2 =
        "{\"device_id\":\"dev2\",\"temp\":12abc,\"hum\":01,\"timestamp\":1.}";
    const char *good_sci =
        "{\"device_id\":\"dev3\",\"temp\":-12.5,\"hum\":1e2,\"timestamp\":1700000000}";

    reset_mocks();
    handle_sensor_data(2, 20, (const uint8_t *)bad1, strlen(bad1));
    CHECK(g_mqtt_calls == 1);
    CHECK(strstr(g_last_mqtt_payload, "\"temp\":0.0,") != NULL);
    CHECK(strstr(g_last_mqtt_payload, "\"hum\":0.0,") != NULL);
    CHECK(strstr(g_last_mqtt_payload, "\"timestamp\":0,") != NULL);

    reset_mocks();
    handle_sensor_data(2, 21, (const uint8_t *)bad2, strlen(bad2));
    CHECK(g_mqtt_calls == 1);
    CHECK(strstr(g_last_mqtt_payload, "\"temp\":0.0,") != NULL);
    CHECK(strstr(g_last_mqtt_payload, "\"hum\":0.0,") != NULL);
    CHECK(strstr(g_last_mqtt_payload, "\"timestamp\":0,") != NULL);

    reset_mocks();
    handle_sensor_data(2, 22, (const uint8_t *)good_sci, strlen(good_sci));
    CHECK(g_mqtt_calls == 1);
    CHECK(strstr(g_last_mqtt_payload, "\"temp\":-12.5,") != NULL);
    CHECK(strstr(g_last_mqtt_payload, "\"hum\":1e2,") != NULL);
}

static void test_device_id_sanitization(void)
{
    const char *json =
        "{\"device_id\":\"bad\\\"id\\\\x\x01\",\"temp\":21,\"hum\":50,\"timestamp\":1}";

    reset_mocks();
    handle_sensor_data(3, 30, (const uint8_t *)json, strlen(json));

    CHECK(g_mqtt_calls == 1);
    CHECK(strstr(g_last_mqtt_payload, "\"device_id\":\"bad__id__x_\"") != NULL);
}

static void test_non_nul_heap_payload(void)
{
    const char *raw =
        "{\"device_id\":\"exact_heap\",\"temp\":23.4,\"hum\":55,\"timestamp\":1700000001}";
    size_t len = strlen(raw);
    uint8_t *buf = malloc(len);

    CHECK(buf != NULL);
    memcpy(buf, raw, len); /* 故意不加 '\0'，供 ASan 检查严格限界 */

    reset_mocks();
    handle_sensor_data(4, 40, buf, len);
    CHECK(g_mqtt_calls == 1);
    CHECK(strstr(g_last_mqtt_payload, "\"device_id\":\"exact_heap\"") != NULL);
    CHECK(strstr(g_last_mqtt_payload, "\"temp\":23.4,") != NULL);
    free(buf);

    /* 截断且无引号收尾的无 NUL 缓冲区 */
    {
        const char *trunc = "{\"device_id\":\"nonul";
        size_t tlen = strlen(trunc);
        uint8_t *tbuf = malloc(tlen);

        CHECK(tbuf != NULL);
        memcpy(tbuf, trunc, tlen);
        reset_mocks();
        handle_sensor_data(4, 41, tbuf, tlen);
        CHECK(g_mqtt_calls == 1);
        CHECK(strstr(g_last_mqtt_payload, "\"device_id\":\"unknown\"") != NULL);
        free(tbuf);
    }
}

static void test_dispatch_config_heartbeat_unknown(void)
{
    struct task t;

    cmd_handlers_init();

    /* CMD_CONFIG (0x03) */
    reset_mocks();
    make_frame_task(&t, 42, 999ULL, CMD_CONFIG, NULL, 0);
    worker_process_task(&t);
    CHECK(g_downlink_calls == 1);
    CHECK(g_last_dl_fd == 42);
    CHECK(g_last_dl_gen == 999ULL);
    CHECK(g_last_dl_len > PACKET_MIN_SIZE);
    CHECK(packet_checksum_verify(g_last_dl_frame, g_last_dl_len) == 1);
    CHECK(g_last_dl_frame[4] == CMD_CONFIG);

    /* CMD_HEARTBEAT (0x02) */
    reset_mocks();
    make_frame_task(&t, 42, 999ULL, CMD_HEARTBEAT, NULL, 0);
    worker_process_task(&t);
    CHECK(g_hb_calls == 1);
    CHECK(g_last_hb_fd == 42);

    /* 未知命令 0xFE */
    reset_mocks();
    make_frame_task(&t, 42, 999ULL, 0xFE, NULL, 0);
    worker_process_task(&t);
    CHECK(g_mqtt_calls == 0);
    CHECK(g_downlink_calls == 0);
    CHECK(g_hb_calls == 0);
}

int main(void)
{
    logger_init(NULL, LOG_LEVEL_ERROR);

    test_same_name_value_collision();
    test_invalid_numbers_fallback();
    test_device_id_sanitization();
    test_non_nul_heap_payload();
    test_dispatch_config_heartbeat_unknown();

    logger_destroy();

    printf("unit_business: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
