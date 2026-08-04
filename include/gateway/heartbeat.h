#ifndef IOT_GATEWAY_HEARTBEAT_H
#define IOT_GATEWAY_HEARTBEAT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化心跳检测器。
 *
 * @param interval_sec 扫描间隔（秒）。
 * @param timeout_sec  无活动超时阈值（秒），超过此值则关闭连接。
 *
 * @return 成功返回 0，失败返回 -1。
 */
int heartbeat_init(int interval_sec, int timeout_sec);

/**
 * @brief 启动心跳线程。
 *
 * @return 成功返回 0，失败返回 -1。
 */
int heartbeat_start(void);

/**
 * @brief 停止心跳线程并等待其退出。
 */
void heartbeat_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* IOT_GATEWAY_HEARTBEAT_H */
