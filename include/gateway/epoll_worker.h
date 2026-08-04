#ifndef IOT_GATEWAY_EPOLL_WORKER_H
#define IOT_GATEWAY_EPOLL_WORKER_H

#include <stddef.h>
#include <stdint.h>

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
 * @brief Request the epoll thread to close a connection.
 *
 * Thread-safe. Writes {fd, generation} into a self-pipe; the epoll thread
 * reads it and performs full cleanup (epoll_ctl DEL, parser unregister,
 * conn_manager remove, close, free) via connection_close().
 *
 * generation 用于避免 fd 复用竞态：若旧连接已被清理且 fd 被新连接复用，
 * epoll 线程按 fd+generation 双重匹配，不会误关新连接。
 *
 * @param fd         The stale client fd to close.
 * @param generation The connection generation captured at collect time.
 */
void epoll_worker_request_close(int fd, uint64_t generation);

/**
 * @brief Request the epoll thread to send a downlink frame to a connection.
 *
 * Thread-safe. Writes the encoded frame into the self-pipe; the epoll thread
 * validates the connection by fd + generation, then performs a best-effort
 * non-blocking send() (MSG_NOSIGNAL). If the connection is gone or the fd was
 * reused by a newer connection, the frame is dropped.
 *
 * All client-fd I/O stays in the epoll thread; worker threads only enqueue
 * requests through the self-pipe.
 *
 * @param fd         The target client fd.
 * @param generation The connection generation captured when the task was queued.
 * @param frame      The fully encoded frame (header + CRC included).
 * @param len        Length of @p frame.
 *
 * @return 0 on success, -1 on invalid args or pipe write failure.
 */
int epoll_worker_request_send(int fd, uint64_t generation,
                              const uint8_t *frame, size_t len);

int epoll_worker_run(int listen_fd);

#ifdef __cplusplus
}
#endif

#endif /* IOT_GATEWAY_EPOLL_WORKER_H */
