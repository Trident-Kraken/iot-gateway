#ifndef IOT_GATEWAY_MQTT_CLIENT_H
#define IOT_GATEWAY_MQTT_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 MQTT 客户端，连接到 Mosquitto Broker。
 *
 * 初始化 libmosquitto 全局状态，创建客户端实例，建立 TCP 连接。
 * 必须在工作线程启动前调用（单线程初始化，无需线程安全）。
 *
 * @param broker    Broker 地址，如 "localhost"。
 * @param port      Broker 端口，如 1883。
 * @param client_id MQTT 客户端 ID（用于 CONNECT 报文）。传 NULL 则由
 *                  libmosquitto 自动生成。
 *
 * @return 0 成功，-1 失败（Broker 不可达或内存不足）。
 */
int mqtt_client_init(const char *broker, int port, const char *client_id);

/**
 * @brief 以 QoS 0 发布一条消息到指定 Topic。
 *
 * 工业传感器高频上报场景下使用 QoS 0 的原因：
 *   1. 低延迟 — QoS 0 是 fire-and-forget，无需等待 PUBACK，发送即完成。
 *      QoS 1/2 需要至少 2/4 次握手，在 --> 100 msg/s 场景下累积延迟不可忽略。
 *   2. 允许少量丢失 — 传感器每 1~10 秒上报一次，单条丢失仅损失一个采样点，
 *      不影响趋势分析。容忍度远高于控制指令或告警。
 *   3. Broker 压力小 — QoS 0 消息不持久化（除非 Broker 配置 bridge），内存
 *      占用低，适合大规模部署。
 *
 * 线程安全：内部持锁保护 mosquitto 句柄，多个工作线程可并发调用。
 * 发布失败时自动重连（最多 3 次），全部失败后丢弃消息，不影响网关继续运行。
 *
 * @param topic   MQTT Topic，如 "sensor/data"。
 * @param payload 消息体（C 字符串，JSON 格式）。
 *
 * @return 0 成功（消息已提交到 Broker），-1 失败（3 次重连后仍未成功）。
*/
/*
 * Updated behavior (v1.4): publish is non-blocking. If the broker is
 * unreachable, the message is dropped immediately (fast-fail) and an
 * async reconnect thread is signalled. The thread retries connection
 * in the background; worker threads are never blocked on reconnect.
 */
int mqtt_publish(const char *topic, const char *payload);

/**
 * @brief 断开 MQTT 连接并释放所有资源。
 *
 * 先停止异步重连线程，再发送 DISCONNECT 报文（优雅断开），销毁
 * mosquitto 实例，最后清理 libmosquitto 全局状态。应在所有工作线程
 * 停止后调用。
 */
void mqtt_client_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* IOT_GATEWAY_MQTT_CLIENT_H */
