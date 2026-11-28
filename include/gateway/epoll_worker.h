#ifndef IOT_GATEWAY_EPOLL_WORKER_H
#define IOT_GATEWAY_EPOLL_WORKER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
 * @brief Request the epoll event loop to exit gracefully.
 *
 * Async-signal-safe：仅做一次标志写入，可在 SIGTERM/SIGINT
 * 信号处理器中直接调用（不加锁、不分配、不记日志）。事件循环通过
 * epoll_wait 超时轮询读到标志后，处理完当前事件批次、关闭全部存活
 * 客户端连接（统一 connection_close 清理路径），epoll_worker_run()
 * 随后返回 0，main() 进入既有逆序清理序列。
 */
void epoll_worker_request_stop(void);

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

/**
 * @brief epoll I/O 线程统计快照。
 */
struct epoll_stats {
    uint64_t active_conns;    /* 当前存活客户端连接数 */
    uint64_t accepted_conns;  /* 累计成功接入连接数 */
    uint64_t pool_rejected;   /* 连接池满（1024）拒绝次数 */
    uint64_t downlink_sent;   /* 下行发送成功帧数 */
    uint64_t downlink_failed; /* 下行发送失败或丢弃帧数 */
};

/**
 * @brief 请求在事件循环安全上下文中输出统计快照（async-signal-safe，可供 SIGUSR1 调用）。
 */
void epoll_worker_request_stats(void);

/**
 * @brief 注册统计快照回调钩子（由 epoll 事件循环在安全线程上下文调用）。
 *
 * @param hook 回调函数指针（可为 NULL 表示取消）。
 */
void epoll_worker_set_stats_hook(void (*hook)(void));

/**
 * @brief 读取 epoll I/O 层的原子统计计数快照。
 *
 * @param out 输出结构体指针。
 */
void epoll_worker_get_stats(struct epoll_stats *out);

/**
 * @brief 运行边缘触发（ET）模式的 epoll 事件循环。
 *
 * 本函数阻塞运行直到：收到优雅停机请求（epoll_worker_request_stop()，
 * 返回 0）或发生致命错误（返回 -1）。它使用 epoll ET（边缘触发）
 * 模式处理监听 socket 和所有已接受的客户端 socket。
 *
 * ET 模式行为：
 *   - epoll_wait() 仅在每次状态变迁时返回一次（例如不可读→可读）。
 *   - 通知后，应用程序必须循环调用 accept()/recv() 排空 fd，直到
 *     调用返回 -1 且 errno == EAGAIN。
 *   - 如果 fd 未被完全排空，epoll_wait() 在发生新的状态变迁之前不会
 *     再次触发通知。
 *
 * LT 模式（本项目不使用）会在数据可用时反复通知，因此每次通知只需一次
 * accept()/recv() 即可，但需要更多的内核/用户上下文切换。
 *
 * @param listen_fd 由 server_init() 返回的非阻塞监听 socket。
 * @return 循环正常退出返回 0，致命错误返回 -1 并设置 errno。
 */
int epoll_worker_run(int listen_fd);

#ifdef __cplusplus
}
#endif

#endif /* IOT_GATEWAY_EPOLL_WORKER_H */
