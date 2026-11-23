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
 * 在长度受限的内存区域 haystack[0..haystack_len) 内查找子串 needle，
 * 不依赖 NUL 结尾，防止非 NUL 终止的原始协议 payload 越界读取。
 */
static const char *find_substr_len(const char *haystack, size_t haystack_len,
                                   const char *needle, size_t needle_len)
{
    size_t i;

    if (haystack == NULL || needle == NULL || needle_len == 0 ||
        haystack_len < needle_len) {
        return NULL;
    }

    for (i = 0; i <= haystack_len - needle_len; i++) {
        if (haystack[i] == needle[0] &&
            memcmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }

    return NULL;
}

/*
 * 校验字符串是否符合 RFC 8259 JSON 数字语法：
 *   -? (0 | [1-9][0-9]*) (\.[0-9]+)? ([eE][+-]?[0-9]+)?
 * 拒绝 "+25"、"---"、"abc"、".5"、"01" 等非法格式，避免拼出无效 JSON。
 */
static int is_valid_json_number(const char *s)
{
    if (s == NULL || *s == '\0') {
        return 0;
    }

    if (*s == '-') {
        s++;
    }

    if (*s == '0') {
        s++;
        if (*s >= '0' && *s <= '9') {
            return 0;
        }
    } else if (*s >= '1' && *s <= '9') {
        while (*s >= '0' && *s <= '9') {
            s++;
        }
    } else {
        return 0;
    }

    if (*s == '.') {
        s++;
        if (*s < '0' || *s > '9') {
            return 0;
        }
        while (*s >= '0' && *s <= '9') {
            s++;
        }
    }

    if (*s == 'e' || *s == 'E') {
        s++;
        if (*s == '+' || *s == '-') {
            s++;
        }
        if (*s < '0' || *s > '9') {
            return 0;
        }
        while (*s >= '0' && *s <= '9') {
            s++;
        }
    }

    return *s == '\0';
}

/* 清洗将放入 JSON 字符串字面量的文本：替换控制字符、引号与反斜杠，防 JSON 破坏。 */
static void sanitize_json_string(char *s)
{
    unsigned char *p;

    if (s == NULL) {
        return;
    }

    for (p = (unsigned char *)s; *p != '\0'; p++) {
        if (*p < 0x20u || *p == '"' || *p == '\\') {
            *p = '_';
        }
    }
}

/*
 * 从长度为 json_len 的 JSON 字节流中提取指定 key 的值，写入 val_buf（最多 val_size 字节）。
 *
 * 严格按 json_len 限界扫描，不依赖 NUL 结尾；若遇到与 key 同名的字符串 value
 * （例如 {"device_id":"temp","temp":25.5} 中先命中 "temp" 后紧跟 ',' 而非 ':'），
 * 会继续向后搜索直到匹配真正的 "key":value。
 *
 * 返回 0 成功，-1 未找到或格式不合法。
 */
static int json_extract(const char *json, size_t json_len,
                        const char *key, char *val_buf, size_t val_size)
{
    const char *json_end;
    const char *cursor;
    const char *colon = NULL;
    const char *start;
    const char *end;
    char search_key[JSON_KEY_MAX];
    int sk_len;

    if (json == NULL || key == NULL || val_buf == NULL || val_size == 0) {
        return -1;
    }

    /* 构造带引号的搜索串: "key" */
    sk_len = snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    if (sk_len < 0 || (size_t)sk_len >= sizeof(search_key)) {
        return -1;
    }

    json_end = json + json_len;
    cursor = json;

    /* 1. 迭代定位真正的 key（后面紧跟空白 + ':' 且非转义引号）。 */
    while (cursor < json_end) {
        const char *pos = find_substr_len(cursor, (size_t)(json_end - cursor),
                                          search_key, (size_t)sk_len);
        const char *p;

        if (pos == NULL) {
            return -1;
        }

        if (pos == json || pos[-1] != '\\') {
            p = pos + sk_len;
            while (p < json_end &&
                   (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
                p++;
            }
            if (p < json_end && *p == ':') {
                colon = p;
                break;
            }
        }

        cursor = pos + 1;
    }

    if (colon == NULL) {
        return -1;
    }

    colon++;

    /* 2. 跳过空白，定位 value 起始。 */
    while (colon < json_end &&
           (*colon == ' ' || *colon == '\t' || *colon == '\r' || *colon == '\n')) {
        colon++;
    }

    if (colon >= json_end) {
        return -1;
    }

    if (*colon == '"') {
        /* 字符串值: "xxx" —— 按 json_len 限界并跳过转义序列查找收尾引号。 */
        colon++;
        start = colon;
        end = start;
        while (end < json_end) {
            if (*end == '\\') {
                if (end + 1 >= json_end) {
                    return -1;
                }
                end += 2;
                continue;
            }
            if (*end == '"') {
                break;
            }
            end++;
        }
        if (end >= json_end) {
            return -1;
        }
    } else {
        /* 数字、布尔或原始 token：读到分隔符/空白为止，由调用方校验合法性。 */
        start = colon;
        end = start;
        while (end < json_end && *end != ',' && *end != '}' && *end != ']' &&
               *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n') {
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
 *   1. 直接在长度受限的 payload 视图上调用 json_extract() 提取字段（零动态分配）。
 *   2. 清洗 device_id 字符串并校验 temp/hum/timestamp 为合法 JSON 数字。
 *   3. 重新打包为标准 JSON，通过 mqtt_publish() 上报。
 */
void handle_sensor_data(int fd, uint64_t generation,
                         const uint8_t *payload, size_t len)
{
    (void)generation;
    const char *json = (const char *)payload;
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

    /* 1. 轻量级 JSON 字段提取（按 len 限界，零 malloc）。 */
    if (json_extract(json, len, "device_id",
                     device_id, sizeof(device_id)) != 0) {
        snprintf(device_id, sizeof(device_id), "unknown");
    } else {
        sanitize_json_string(device_id);
    }

    if (json_extract(json, len, "temp", temp_str, sizeof(temp_str)) != 0 ||
        !is_valid_json_number(temp_str)) {
        snprintf(temp_str, sizeof(temp_str), "0.0");
    }

    if (json_extract(json, len, "hum", hum_str, sizeof(hum_str)) != 0 ||
        !is_valid_json_number(hum_str)) {
        snprintf(hum_str, sizeof(hum_str), "0.0");
    }

    if (json_extract(json, len, "timestamp", ts_str, sizeof(ts_str)) != 0 ||
        !is_valid_json_number(ts_str)) {
        snprintf(ts_str, sizeof(ts_str), "0");
    }

    /* 高频路径（每帧一条）：用 DEBUG 避免刷屏与日志 I/O 瓶颈。 */
    LOG_DEBUG("handle_sensor_data: fd=%d device=%s temp=%s hum=%s ts=%s",
              fd, device_id, temp_str, hum_str, ts_str);

    /* 2. 重新打包为标准 JSON 并上报 MQTT。 */
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
        return;
    }

    (void)mqtt_publish(MQTT_TOPIC_SENSOR, mqtt_json);
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
 * 设计说明（task_queue / threadpool 锁粒度）：
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
 * 设计说明（业务下行链路）：
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
 * 设计说明：
 *   - 函数指针数组提供确定的 O(1) 命令分发（不依赖编译器 switch 跳转表优化）。
 *   - 维护时只需加一行赋值即可注册新的命令处理器，无需改动分发骨架。
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
 * 设计说明（I/O 与业务逻辑解耦）：
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
     * 设计说明（O(1) 命令分发）：
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
