#ifndef IOT_GATEWAY_CONN_MANAGER_H
#define IOT_GATEWAY_CONN_MANAGER_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 为每个活跃客户端 socket 维护的连接条目。
 *
 * epoll 工作线程负责添加/删除条目，心跳读取线程遍历它们，
 * 心跳写入线程可能关闭过期的 fd。所有链表修改操作由内部互斥锁保护。
 */
struct conn_entry {
    int        fd;            /* 客户端 socket 文件描述符。 */
    uint64_t   generation;    /* 连接代际号：fd 复用后用于区分新旧连接。 */
    time_t     last_active;   /* 最近一次 recv() 活动的时间戳。 */
    struct conn_entry *next;  /* 链表指针。 */
};

/**
 * @brief 初始化连接管理器。
 *
 * @return 成功返回 0，失败返回 -1。
 */
int conn_manager_init(void);

/**
 * @brief 销毁连接管理器并释放所有条目。
 *
 * 注意：本函数不关闭文件描述符；调用方必须先自行关闭。
 */
void conn_manager_destroy(void);

/**
 * @brief 添加一个新的连接条目。
 *
 * @param fd         客户端文件描述符。
 * @param generation 连接代际号（epoll_worker 分配，用于 fd 复用场景）。
 *
 * @return 成功返回 0，失败返回 -1。
 */
int conn_manager_add(int fd, uint64_t generation);

/**
 * @brief 按文件描述符删除一个连接条目。
 *
 * @param fd 客户端文件描述符。
 */
void conn_manager_remove(int fd);

/**
 * @brief 更新连接的最后活跃时间戳。
 *
 * @param fd 客户端文件描述符。
 */
void conn_manager_update_active_time(int fd);

/**
 * @brief 按文件描述符查询连接代际号。
 *
 * 用于工作线程在发起下行响应前核对连接是否仍是同一代（fd 复用防护），
 * 与 epoll_worker 的 generation 双重匹配配套。
 *
 * @param fd  客户端文件描述符。
 * @param gen 输出参数：成功时写入代际号。
 *
 * @return 0 成功，-1 未找到（该 fd 无对应连接）。
 */
int conn_manager_get_generation(int fd, uint64_t *gen);

/**
 * @brief 带回调遍历所有连接条目。
 *
 * 回调在连接管理器锁内执行。调用方不得在回调中执行长时间工作，
 * 也不能直接从回调中调用 conn_manager_remove() 释放当前条目
 *（应使用两段式模式）。
 *
 * @param callback 对每个条目调用的函数。
 * @param arg      传递给回调的不透明参数。
 */
void conn_manager_foreach(void (*callback)(struct conn_entry *, void *), void *arg);

#ifdef __cplusplus
}
#endif

#endif /* IOT_GATEWAY_CONN_MANAGER_H */
