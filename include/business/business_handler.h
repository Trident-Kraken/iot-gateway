#ifndef IOT_GATEWAY_BUSINESS_HANDLER_H
#define IOT_GATEWAY_BUSINESS_HANDLER_H

#include <stddef.h>
#include <stdint.h>

#include "utils/task_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ---------------------------------------------------------------------------
 * 为什么用函数指针数组实现命令分发，而不是 switch-case？
 * ---------------------------------------------------------------------------
 *
 * 1. O(1) 跳转 — 函数指针数组通过 cmd 直接索引，无需逐条比较。switch-case
 *    编译器通常会优化成跳转表，但当 case 稀疏（如 0x01, 0x7F, 0xFE）时可能退
 *    化为 if-else 链，造成 O(n) 逐条匹配。
 *
 * 2. 可扩展性 — 新增命令处理函数只需定义函数实现 + 在初始化中将
 *    g_cmd_handlers[CMD_XXX] = handle_xxx 注册即可，无需修改分发逻辑本身。
 *    这遵循开放-封闭原则（对扩展开放，对修改封闭）。
 *
 * 3. 解耦 — 每个 handler 独立编译、独立测试，命令分发骨架与具体业务逻辑分离。
 *    工作线程无需感知 0x01 是传感器还是报警，只需按 cmd 查表调用。
 *
 * 4. 可读性 — 256 项的数组初始化一目了然，比 256 个 case 的 switch 更紧凑。
 */

/** 命令处理函数指针类型。 */
typedef void (*cmd_handler_t)(int fd, uint64_t generation,
                              const uint8_t *payload, size_t len);

/* =========================================================================
 * 业务处理函数（每个对应一种命令字）
 * ========================================================================= */

/**
 * @brief 处理传感器数据（CMD_SENSOR_DATA = 0x01）。
 *
 * Payload 为 JSON 字符串，形如：
 *   {"device_id":"dev_001","temp":25.5,"hum":60,"timestamp":1718188800}
 *
 * 使用轻量字符串解析（strstr + sscanf），无需 cJSON，适合两周项目。
 * 解析后重新打包 JSON，通过 mqtt_publish() 上报至云端。
 *
 * @param fd         来源连接的文件描述符。
 * @param generation 连接代际号（任务入队时记录，供下行响应防 fd 复用）。
 * @param payload    Payload 字节首地址（不含帧头/长度/命令/CRC）。
 * @param len        Payload 长度（字节数）。
 */
void handle_sensor_data(int fd, uint64_t generation,
                        const uint8_t *payload, size_t len);

/**
 * @brief 处理心跳包（CMD_HEARTBEAT = 0x02）。
 *
 * 更新连接的 last_active_time，并可选发送心跳响应。
 *
 * @param fd         来源连接的文件描述符。
 * @param generation 连接代际号（本 handler 不使用，保留下行扩展能力）。
 * @param payload    Payload 字节首地址（可为 NULL，心跳包通常无 payload）。
 * @param len        Payload 长度（心跳包通常为 0）。
 */
void handle_heartbeat(int fd, uint64_t generation,
                      const uint8_t *payload, size_t len);

/**
 * @brief 处理配置请求（CMD_CONFIG = 0x03，下行）。
 *
 * 设备发送 CMD_CONFIG 帧（Payload 可为空或 JSON 请求）时，网关回复一帧
 * 携带默认设备配置 JSON 的 CMD_CONFIG 帧（网关 → 设备下行）。下行帧通过
 * epoll_worker_request_send() 委托 epoll 线程发送，fd + generation 双重
 * 匹配确保不会误发给 fd 复用后的新连接。
 *
 * @param fd         来源连接的文件描述符。
 * @param generation 连接代际号（防 fd 复用误发）。
 * @param payload    请求 Payload 字节首地址（可为 NULL）。
 * @param len        Payload 长度。
 */
void handle_config(int fd, uint64_t generation,
                   const uint8_t *payload, size_t len);

/* =========================================================================
 * 初始化与分发入口
 * ========================================================================= */

/**
 * @brief 初始化命令分发表。
 *
 * 将 g_cmd_handlers 数组填充为对应的业务处理函数指针。
 * 必须在工作线程启动前调用一次（调用方负责保证线程安全）。
 */
void cmd_handlers_init(void);

/**
 * @brief 工作线程入口：消费一个任务并分发到业务处理函数。
 *
 * 调用流程：
 *   1. 从 task->frame 中提取命令字（offset 4）。
 *   2. 以 cmd 索引 g_cmd_handlers。
 *   3. 若 handler 非 NULL，调用 handler(fd, payload, payload_len)。
 *   4. 若 handler 为 NULL，记录 LOG_ERROR 并丢弃。
 *
 * 本函数在工作线程上下文中调用，不应执行阻塞 I/O。
 *
 * @param task 从任务队列取出的完整帧任务。调用方拥有所有权并负责释放。
 */
void worker_process_task(struct task *task);

#ifdef __cplusplus
}
#endif

#endif /* IOT_GATEWAY_BUSINESS_HANDLER_H */
