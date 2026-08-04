#ifndef IOT_GATEWAY_EPOLL_WORKER_H
#define IOT_GATEWAY_EPOLL_WORKER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 运行边缘触发（ET）模式的 epoll 事件循环。 *
 * 本函数永久阻塞（直到发生致命错误）。它使用 epoll ET（边缘触发）模式
 * 处理监听 socket 和所有已接受的客户端 socket。 *
 * ET 模式行为： *   - epoll_wait() 仅在每次状态变迁时返回一次（例如不可读→可读）。 *   - 通知后，应用程序必须循环调用 accept()/recv() 排空 fd，直到 *     调用返回 -1 且errno == EAGAIN。 *   - 如果 fd 未被完全排空，epoll_wait() 在发生新的状态变迁之前不会 *     再次触发通知。 *
 * LT 模式（本项目不使用）会在数据可用时反复通知，因此每次通知只需一次 * accept()/recv() 即可，但需要更多的内核/用户上下文切换。 *
 * @param listen_fd 由server_init() 返回的非阻塞监听 socket。 *
 * @return 循环正常退出返回0，致命错误返回-1 并设置errno。 */
/**
 * @brief Create the close-request self-pipe.
 *
 * Must be called before heartbeat_start() and before epoll_worker_run().
 * Creates a non-blocking pipe that the heartbeat thread uses to request
 * the epoll thread to close stale connections.
 *
 * @return 0 on success, -1 on error.
 */
int epoll_worker_init(void);

/**
 * @brief Close the close-request pipe and release resources.
 *
 * Call after heartbeat_stop() and after epoll_worker_run() returns.
 */
void epoll_worker_destroy(void);

/**
 * @brief Request the epoll thread to close a connection by fd.
 *
 * Thread-safe. Writes the fd into a self-pipe; the epoll thread reads
 * it and performs full cleanup (epoll_ctl DEL, parser unregister,
 * conn_manager remove, close, free) via connection_close().
 *
 * @param fd The stale client fd to close.
 */
void epoll_worker_request_close(int fd);

int epoll_worker_run(int listen_fd);

#ifdef __cplusplus
}
#endif

#endif /* IOT_GATEWAY_EPOLL_WORKER_H */
