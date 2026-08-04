#include "business/business_handler.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gateway/conn_manager.h"
#include "gateway/epoll_worker.h"
#include "utils/logger.h"
#include "business/mqtt_client.h"
#include "protocol/protocol.h"

/* MQTT 上报 Topic。 */
#define MQTT_TOPIC_SENSOR   "sensor/data"

/*
 * CMD_CONFIG 下行响应的默认设备配置（JSON）。
 * 当前网关没有设备配置数据源，先以固定默认值回复；后续可扩展为
 * 按 device_id 查询真实配置后下发。
 */
#define DEVICE_CONFIG_JSON  "{\"sampling_interval\":5,\"upload_freq\":2,\"heartbeat_interval\":5}"

/* 简易 JSON 提取缓冲区大小。 */
#define JSON_KEY_MAX   32
#define JSON_VALUE_MAX 64

static cmd_handler_t g_cmd_handlers[256];

/* =========================================================================
 * 内部辅助函数
 * ========================================================================= */

/*
 * 从 JSON 字符串中提取指定 key 的值，写入 val_buf（最多 val_size 字节）。
 *
 * 使用 strstr（轻量字符串查找），仅处理 "key":value 的简单场景：
 *   - 字符串值带引号：  "temp":25.5  → val_buf = "25.5"
 *   - 整数值不带引号： 同理
 *   - 仅取第一个匹配项
 *
 * 返回 0 成功，-1 未找到或格式不合法。
 */
static int json_extract(const char *json, size_t json_len,
                        const char *key, char *val_buf, size_t val_size)
{
    const char *pos;
    const char *colon;
    const char *start;
    const char *end;
    char search_key[JSON_KEY_MAX];

    if (json == NULL || key == NULL || val_buf == NULL || val_size == 0) {
        return -1;
    }

    /* 构造带引号的搜索串: "key" */
    (void)snprintf(search_key, sizeof(search_key), "\"%s\"", key);

    /* 1. 定位 key。 */
    pos = strstr(json, search_key);
    if (pos == NULL) {
        return -1;
    }

    /* 2. 跳过 key，找 :。 */
    pos += strlen(search_key);

    /* 跳过空白 */
    while (pos < json + json_len && (*pos == ' ' || *pos == '\t')) {
        pos++;
    }

    colon = pos;
    if (colon >= json + json_len || *colon != ':') {
        return -1;
    }

    colon++;

    /* 3. 跳过空白，定位 value 起始。 */
    while (colon < json + json_len && (*colon == ' ' || *colon == '\t')) {
        colon++;
    }

    if (colon >= json + json_len) {
        return -1;
    }

    if (*colon == '"') {
        /* 字符串值: "xxx" */
        colon++;
        start = colon;
        end = strchr(start, '"');
        if (end == NULL) {
            return -1;
        }
    } else {
        /* 数字或布尔值: 25.5, 60, true, false */
        start = colon;
        end = start;
        while (end < json + json_len
               && (*end == '-' || *end == '+' || *end == '.'
                   || (*end >= '0' && *end <= '9'))) {
            end++;
        }
    }

    if (end == start) {
        return -1;
    }

    {
        size_t copy_len = (size_t)(end - start);
        if (copy_len >= val_size) {
            copy_len = val_size - 1;
        }
        memcpy(val_buf, start, copy_len);
        val_buf[copy_len] = '\0';
    }

    return 0;
}

/* =========================================================================
 * 命令处理函数
 * ========================================================================= */

/**
 * @brief 处理 CMD_SENSOR_DATA (0x01) —— 传感器数据上报。
 *
 * Payload 为 JSON 字符串，形如：
 *   {"device_id":"dev_001","temp":25.5,"hum":60,"timestamp":1718188800}
 *
 * 处理流程：
 *   1. 将 Payload 转为 C 字符串（加 '\0' 结尾）。
 *   2. 用轻量 strstr/sscanf 提取 device_id、temp、hum、timestamp。
 *   3. 重新打包为标准 JSON，通过 mqtt_publish() 上报。
 *
 * 注：
 *   - 当前不引入 cJSON 等第三方库，以 strstr + sscanf 完成简易解析。
 *   - Payload 中可能不含 '\0'，需手动构造临时副本保证字符串安全。
 */
void handle_sensor_data(int fd, uint64_t generation,
                         const uint8_t *payload, size_t len)
{
    (void)generation;
    char *json_str;
    char device_id[JSON_VALUE_MAX]   = "unknown";
    char temp_str[JSON_VALUE_MAX]    = "0.0";
    char hum_str[JSON_VALUE_MAX]     = "0.0";
    char ts_str[JSON_VALUE_MAX]      = "0";

    /*
     * MQTT 上报的 JSON 缓冲区。
     * 最大约 256 字节（device_id + temp + hum + ts + 固定开销）。
     */
    char mqtt_json[384];
    int mqtt_json_len;

    if (payload == NULL || len == 0) {
        LOG_ERROR("handle_sensor_data: fd=%d empty payload", fd);
        return;
    }

    /* 1. 转为 C 字符串（payload 中不含 '\0'）。 */
    json_str = malloc(len + 1);
    if (json_str == NULL) {
        LOG_ERROR("handle_sensor_data: malloc failed for fd=%d", fd);
        return;
    }
    memcpy(json_str, payload, len);
    json_str[len] = '\0';

    /* 2. 轻量级 JSON 字段提取（不使用 cJSON）。 */
    (void)json_extract(json_str, len, "device_id",
                       device_id, sizeof(device_id));
    (void)json_extract(json_str, len, "temp",
                       temp_str, sizeof(temp_str));
    (void)json_extract(json_str, len, "hum",
                       hum_str, sizeof(hum_str));
    (void)json_extract(json_str, len, "timestamp",
                       ts_str, sizeof(ts_str));

    LOG_INFO("handle_sensor_data: fd=%d device=%s temp=%s hum=%s ts=%s",
             fd, device_id, temp_str, hum_str, ts_str);

    /* 3. 重新打包为标准 JSON 并上报 MQTT。 */
    mqtt_json_len = snprintf(mqtt_json, sizeof(mqtt_json),
                             "{"
                             "\"device_id\":\"%s\","
                             "\"temp\":%s,"
                             "\"hum\":%s,"
                             "\"timestamp\":%s,"
                             "\"gateway_time\":%ld"
                             "}",
                             device_id, temp_str, hum_str, ts_str,
                             (long)time(NULL));

    if (mqtt_json_len < 0 || (size_t)mqtt_json_len >= sizeof(mqtt_json)) {
        LOG_ERROR("handle_sensor_data: snprintf overflow for fd=%d", fd);
        free(json_str);
        return;
    }

    (void)mqtt_publish(MQTT_TOPIC_SENSOR, mqtt_json);

    free(json_str);
}

/**
 * @brief 处理 CMD_HEARTBEAT (0x02) —— 心跳包。
 *
 * 业务含义：
 *   - 更新连接的 last_active_time（设备主动心跳说明链路存活）。
 *   - 实际部署中可选发送心跳响应（ACK）。
 *
 * 当前实现：
 *   - 调用 conn_manager_update_active_time() 刷新时间戳。
 *   - 打印 DEBUG 日志。
 *
 * 面试点注释（task_queue / threadpool 深水区）：
 *   该函数运行在工作线程上下文。工作线程从 task_queue_pop() 取出任务后，
 *   通过函数指针表 g_cmd_handlers[0x02] 调用到此函数。函数执行期间不持
 *   有任务队列锁（task_queue_pop 的 mutex 仅在 pop 时持有），因此可以在
 *   这里安全地访问 conn_manager（conn_manager_update_active_time 内部自
 *   行加锁）。
 */
void handle_heartbeat(int fd, uint64_t generation,
                      const uint8_t *payload, size_t len)
{
    (void)generation;
    (void)payload;
    (void)len;

    if (fd < 0) {
        return;
    }

    /* 刷新该连接的活跃时间戳（内部已加锁）。 */
    conn_manager_update_active_time(fd);

    LOG_DEBUG("handle_heartbeat: fd=%d activity refreshed", fd);
}

/**
 * @brief 处理 CMD_CONFIG (0x03) —— 配置请求 / 下行响应。
 *
 * 设备发送配置请求帧（Payload 可为空或 JSON），网关回发一帧 CMD_CONFIG：
 *   - 用 packet_encode() 将默认设备配置 JSON 编码为完整帧；
 *   - 通过 epoll_worker_request_send() 委托 epoll 线程发送（best-effort）；
 *   - fd + generation 双重匹配，连接已关闭或 fd 被复用时静默丢弃。
 *
 * 面试点注释（业务下行链路）：
 *   工作线程不直接写客户端 fd —— 所有 fd I/O 收敛在 epoll 线程，工作线程
 *   只通过 self-pipe 委托，避免多线程并发读写同一 socket 与 fd 复用竞态。
 */
void handle_config(int fd, uint64_t generation,
                   const uint8_t *payload, size_t len)
{
    const size_t config_len = sizeof(DEVICE_CONFIG_JSON) - 1;
    struct packet *pkt;
    uint8_t frame[PROTOCOL_MAX_FRAME_SIZE];
    size_t frame_len;

    if (payload != NULL && len > 0) {
        LOG_INFO("handle_config: fd=%d gen=%llu config request (%zu bytes)",
                 fd, (unsigned long long)generation, len);
    } else {
        LOG_INFO("handle_config: fd=%d gen=%llu config request (empty)",
                 fd, (unsigned long long)generation);
    }

    /* 组装 CMD_CONFIG 响应帧（柔性数组包）。 */
    pkt = malloc(sizeof(*pkt) + config_len);
    if (pkt == NULL) {
        LOG_ERROR("handle_config: malloc failed for fd=%d", fd);
        return;
    }
    pkt->cmd = CMD_CONFIG;
    pkt->payload_len = (uint16_t)config_len;
    memcpy(pkt->payload, DEVICE_CONFIG_JSON, config_len);

    if (packet_encode(pkt, frame, sizeof(frame)) != 0) {
        LOG_ERROR("handle_config: packet_encode failed for fd=%d", fd);
        free(pkt);
        return;
    }
    free(pkt);

    frame_len = PACKET_MIN_SIZE + config_len;

    /* 委托 epoll 线程发送（非阻塞、MSG_NOSIGNAL、generation 匹配）。 */
    if (epoll_worker_request_send(fd, generation, frame, frame_len) != 0) {
        LOG_ERROR("handle_config: enqueue downlink failed for fd=%d", fd);
        return;
    }

    LOG_INFO("handle_config: fd=%d config downlink queued (%zu bytes)",
             fd, frame_len);
}

/* =========================================================================
 * 命令分发表初始化
 * ========================================================================= */

/**
 * @brief 初始化全局命令分发表。
 *
 * 将 g_cmd_handlers[cmd] 赋值为对应的处理函数指针。
 * 未注册的命令字保持 NULL，dispatch 时检查并跳过。
 *
 * 面试点注释：
 *   - 函数指针数组 = 最快的命令分发方式之一（编译器可内联间接调用）。
 *   - 维护时只需加一行赋值即可注册新的命令处理器，无需改动 switch-case。
 *   - 数组大小为 256，索引范围与 uint8_t cmd 天然对齐，无需越界检查。
 */
void cmd_handlers_init(void)
{
    size_t i;

    /* 先全部置 NULL（防御性）。 */
    for (i = 0; i < 256; i++) {
        g_cmd_handlers[i] = NULL;
    }

    /* 注册已知命令处理器。 */
    g_cmd_handlers[CMD_SENSOR_DATA] = handle_sensor_data; /* 0x01 */
    g_cmd_handlers[CMD_HEARTBEAT]   = handle_heartbeat;   /* 0x02 */
    g_cmd_handlers[CMD_CONFIG]      = handle_config;      /* 0x03 下行 */

    LOG_INFO("cmd_handlers: dispatch table initialized (3/256 handlers registered)");
}

/* =========================================================================
 * 工作线程入口（从任务到业务的分发枢纽）
 * ========================================================================= */

/**
 * @brief 工作线程的任务入口函数。
 *
 * 由 threadpool 的工作线程在取到任务后调用，负责：
 *   1. 校验任务有效性。
 *   2. 从完整帧中提取命令字。
 *   3. 通过函数指针数组 g_cmd_handlers[cmd] 分发给具体处理函数。
 *   4. 未知命令字记录错误并丢弃。
 *
 * 面试点注释（threadpool 深水区）：
 *   该函数在工作线程上下文中执行，**不持有任务队列锁**。业务处理（如 JSON
 *   解析、MQTT 发布）与 I/O 线程 epoll 事件循环完全解耦。这保证了：
 *     - I/O 线程即使在高负载下也能快速 recv + 入队，不会被业务逻辑阻塞。
 *     - 工作线程不会被 epoll_wait 阻塞，专注处理 CPU/网络密集型业务。
 *     - 两个线程池通过任务队列解耦，锁粒度仅限于 push/pop 操作。
 */
void worker_process_task(struct task *task)
{
    cmd_handler_t handler;
    const uint8_t *payload;
    size_t payload_len;

    if (task == NULL) {
        LOG_ERROR("worker_process_task: task is NULL");
        return;
    }

    /*
     * 帧格式回顾（protocol.h）：
     *   Offset: 0  1  2  3  4  5 .. 5+N-1  5+N..5+N+1
     *   Field:  AA 55 LEN_HI LEN_LO CMD PAYLOAD  CRC16
     *
     * 命令字位于偏移量 4，Payload 从偏移量 5 开始，长度 = frame_len - 7。
     */
    if (task->frame_len < PACKET_MIN_SIZE) {
        LOG_ERROR("worker_process_task: fd=%d frame too short (%zu bytes, min %d)",
                  task->fd, task->frame_len, PACKET_MIN_SIZE);
        return;
    }

    /* 提取命令字（偏移量 4）。 */
    task->cmd = task->frame[4];

    /* 提取 Payload 首地址与长度。 */
    payload = &task->frame[5];
    payload_len = task->frame_len - PACKET_MIN_SIZE;

    /*
     * 面试点注释：
     *   g_cmd_handlers[task->cmd] 是 O(1) 的数组索引操作。
     *   无论注册了 1 个还是 200 个处理函数，dispatch 耗时恒定。
     *   相比 switch-case，函数指针表在稀疏 case 下性能更稳定。
     */
    handler = g_cmd_handlers[task->cmd];
    if (handler == NULL) {
        LOG_ERROR("worker_process_task: fd=%d unknown cmd=0x%02x, "
                  "frame_len=%zu",
                  task->fd, task->cmd, task->frame_len);
        return;
    }

    /*
     * 调用对应的业务处理函数。
     *
     * 注意：handler 运行在工作线程上下文，可以安全地：
     *   - 调用 mqtt_publish()（网络发送）。
     *   - 调用 conn_manager_update_active_time()（内部自行加锁）。
     *   - 进行 CPU 密集的 JSON 解析。
     */
    handler(task->fd, task->generation, payload, payload_len);
}
