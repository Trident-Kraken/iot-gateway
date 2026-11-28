#define _POSIX_C_SOURCE 200809L /* PIPE_BUF */

#include "gateway/epoll_worker.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "gateway/conn_manager.h"
#include "utils/fd_util.h"
#include "utils/logger.h"
#include "utils/threadpool.h"
#include "protocol/packet_parser.h"
#include "protocol/protocol.h"
#include "gateway/server.h"

/* 每次 epoll_wait() 调用处理的最大事件数。*/
#define EPOLL_MAX_EVENTS 1024

/* epoll_wait 超时（毫秒）：不用 -1 永久阻塞，周期性醒来轮询
 * g_stop_requested，保证信号处理器仅置标志（不做任何其他系统调用）
 * 也能在半个周期内触发优雅退出。 */
#define EPOLL_POLL_TIMEOUT_MS 500

/* Self-pipe for cross-thread close requests: heartbeat thread writes fds,
 * epoll thread reads them and performs full connection cleanup. */
static int g_close_pipe[2] = { -1, -1 };

/* 优雅停机请求标志：由 SIGTERM/SIGINT 处理器经
 * epoll_worker_request_stop() 置位。volatile sig_atomic_t 是信号处理器
 * 与普通代码共享标志的标准类型（单字读写原子，async-signal-safe）。 */
static volatile sig_atomic_t g_stop_requested = 0;

/* 运行时统计快照请求标志（SIGUSR1 async-signal-safe）与回调钩子。 */
static volatile sig_atomic_t g_stats_requested = 0;
static void (*g_stats_hook)(void) = NULL;

/* epoll I/O 线程统计计数器（仅 epoll 线程写入，relaxed 原子读取）。 */
static atomic_uint_fast64_t g_active_conns    = 0;
static atomic_uint_fast64_t g_accepted_conns  = 0;
static atomic_uint_fast64_t g_pool_rejected   = 0;
static atomic_uint_fast64_t g_downlink_sent   = 0;
static atomic_uint_fast64_t g_downlink_failed = 0;

/* Doubly-linked list of active client connections (O(1) removal).
 * Only accessed by the epoll thread, no lock needed. */
static struct connection *g_conn_list = NULL;

/* Client fd hash table (2^k buckets, O(1) lookup by fd, epoll thread only). */
#define CONN_HASH_BUCKETS 1024u
static struct connection *g_conn_buckets[CONN_HASH_BUCKETS];

static size_t conn_hash_bucket(int fd)
{
    return (size_t)(((unsigned int)fd) & (CONN_HASH_BUCKETS - 1u));
}

/**
 * @brief 存储在 epoll_event.data.ptr 中的每连接上下文。
 *
 * packet_parser 内部拥有每连接 RingBuffer，因此 epoll_worker 只需
 * 跟踪 fd 及其在事件循环中的角色（监听/客户端/self-pipe）。
 */
/* 连接角色：区分 epoll 事件循环中三类 fd 来源。 */
enum conn_role {
    CONN_ROLE_CLIENT = 0, /* 客户端连接。 */
    CONN_ROLE_LISTEN = 1, /* 监听 socket。 */
    CONN_ROLE_PIPE   = 2  /* close-request self-pipe（sentinel）。 */
};

struct connection {
    int       fd;
    uint64_t  generation; /* 连接代际号：accept 时递增分配，fd 复用后用于区分。 */
    enum conn_role role;  /* 该 fd 在事件循环中的角色。 */
    /* 本事件批内是否已被 connection_close() 关闭（陈旧事件重入防护，
     * 见 connection_close 与事件分发处的设计说明）。对象池 acquire
     * 时的 memset 会把它清零，新连接天然为“未关闭”。 */
    int       closed;
    struct parse_ctx  *parser;    /* 直连每连接解析器上下文（O(1) 零查表） */
    struct connection *next;      /* 活跃双向链 next / 空闲链与 trash 链指针 */
    struct connection *prev;      /* 活跃双向链 prev（O(1) 删除） */
    struct connection *hash_next; /* fd 哈希桶链表指针（O(1) 查找） */
};

/* 连接代际计数器（仅 epoll 线程访问，无需加锁）。 */
static uint64_t g_generation = 0;

/* 客户端连接上下文对象池容量（固定上限：超出后 accept 被拒绝并记日志）。 */
#define CONN_POOL_CAPACITY 1024

/* 客户端连接上下文对象池：固定数组 + LIFO 空闲链。
 * 仅 epoll 线程访问（accept/connection_close 均在其上下文中），无需加锁，
 * 在高并发连接建立/关闭关键路径上避免动态分配。 */
static struct connection g_conn_pool[CONN_POOL_CAPACITY];
static struct connection *g_conn_free = NULL;
static int g_conn_pool_initialized = 0;

/* 本 epoll_wait 批次内已关闭、待批末归还的连接槽（LIFO，仅 epoll 线程访问）。
 * 为什么不立即归还空闲链：events[] 是 epoll_wait 返回时刻的快照，同批内
 * 可能残留“已被 pipe 消息关闭的连接”的陈旧事件；若立即归还，槽位会被
 * 同批 accept 复用，陈旧事件将误伤新连接。批末统一归还保证批内槽位
 * 绝不复用，配合 closed 标志可安全跳过陈旧事件。 */
static struct connection *g_conn_trash = NULL;

/*
 * 设计说明（对象池初始化标志与耗尽区分）：
 *   绝不能用 `g_conn_free == NULL` 判断对象池是否已初始化！当 1024 个
 *   活跃连接同时存在时，`g_conn_free` 也会自然变为 NULL（池已耗尽）。
 *   若误以 `g_conn_free == NULL` 再次触发 `conn_pool_init()`，会把正在
 *   使用的 1024 个连接槽位全部重置串回空闲链，直接破坏 `g_conn_list`
 *   活跃链表并清零活跃槽位。必须用独立的 `g_conn_pool_initialized` 标志。
 */
static void conn_pool_init(void)
{
    size_t i;

    if (g_conn_pool_initialized) {
        return;
    }

    g_conn_free = NULL;
    for (i = 0; i < CONN_POOL_CAPACITY; i++) {
        g_conn_pool[i].next = g_conn_free;
        g_conn_free = &g_conn_pool[i];
    }
    g_conn_pool_initialized = 1;
}

static struct connection *conn_pool_acquire(void)
{
    struct connection *conn;

    if (!g_conn_pool_initialized) {
        conn_pool_init();
    }

    conn = g_conn_free;
    if (conn != NULL) {
        g_conn_free = conn->next;
        memset(conn, 0, sizeof(*conn)); /* 与 calloc 行为一致。 */
    }
    return conn;
}

static void conn_pool_release(struct connection *conn)
{
    if (conn == NULL) {
        return;
    }

    conn->next = g_conn_free;
    g_conn_free = conn;
}

/*
 * 批末回收：把本事件批内关闭的连接槽从 trash 链统一归还空闲链。
 * 由事件循环在处理完一批 events[] 的全部事件后调用（仅 epoll 线程），
 * 保证关闭槽位在批内绝不被 accept 复用（见 g_conn_trash 注释）。
 */
static void conn_pool_flush_trash(void)
{
    while (g_conn_trash != NULL) {
        struct connection *conn = g_conn_trash;
        g_conn_trash = conn->next;
        conn_pool_release(conn);
    }
}

/* self-pipe 消息类型。 */
enum {
    PIPE_MSG_CLOSE = 1, /* 关闭连接（心跳线程委托）。 */
    PIPE_MSG_SEND  = 2  /* 下行发送（工作线程委托，如 CMD_CONFIG 响应）。 */
};

/*
 * self-pipe 载荷：fd + generation 双重匹配，防止 fd 复用后误关/误发新连接。
 * data 内嵌完整编码帧（<= PROTOCOL_MAX_FRAME_SIZE），整条消息 < PIPE_BUF，
 * write 原子且不会半写。
 */
struct pipe_msg {
    int      fd;
    uint64_t generation;
    uint8_t  type;    /* PIPE_MSG_CLOSE / PIPE_MSG_SEND。 */
    uint16_t len;     /* PIPE_MSG_SEND 时 data 有效长度。 */
    uint8_t  data[PROTOCOL_MAX_FRAME_SIZE];
};

_Static_assert(sizeof(struct pipe_msg) <= PIPE_BUF,
               "struct pipe_msg must fit in PIPE_BUF for atomic pipe writes");

/* =========================================================================
 * Connection list & hash helpers (epoll thread only, no lock needed)
 * ========================================================================= */

static void conn_list_add(struct connection *conn)
{
    size_t bucket = conn_hash_bucket(conn->fd);

    conn->prev = NULL;
    conn->next = g_conn_list;
    if (g_conn_list != NULL) {
        g_conn_list->prev = conn;
    }
    g_conn_list = conn;

    conn->hash_next = g_conn_buckets[bucket];
    g_conn_buckets[bucket] = conn;
}

static void conn_list_remove(struct connection *conn)
{
    struct connection **pp;

    if (conn->prev != NULL) {
        conn->prev->next = conn->next;
    } else if (g_conn_list == conn) {
        g_conn_list = conn->next;
    }
    if (conn->next != NULL) {
        conn->next->prev = conn->prev;
    }
    conn->prev = NULL;
    conn->next = NULL;

    pp = &g_conn_buckets[conn_hash_bucket(conn->fd)];
    while (*pp != NULL) {
        if (*pp == conn) {
            *pp = conn->hash_next;
            conn->hash_next = NULL;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

static struct connection *conn_find_by_fd(int fd)
{
    struct connection *conn;

    if (fd < 0) {
        return NULL;
    }

    for (conn = g_conn_buckets[conn_hash_bucket(fd)];
         conn != NULL;
         conn = conn->hash_next) {
        if (conn->fd == fd && !conn->closed) {
            return conn;
        }
    }
    return NULL;
}

static struct connection *conn_list_find(int fd, uint64_t generation)
{
    struct connection *conn = conn_find_by_fd(fd);

    if (conn != NULL && conn->generation == generation) {
        return conn;
    }
    return NULL;
}

/* =========================================================================
 * Public API: init / destroy / request_close
 * ========================================================================= */

int epoll_worker_init(void)
{
    conn_pool_init();
    g_stop_requested = 0;

    if (g_close_pipe[0] != -1) {
        return 0; /* already initialized */
    }

    if (pipe(g_close_pipe) != 0) {
        LOG_ERROR("epoll_worker_init: pipe failed: %s", strerror(errno));
        return -1;
    }

    if (fd_set_nonblocking(g_close_pipe[0]) == -1 ||
        fd_set_nonblocking(g_close_pipe[1]) == -1 ||
        fd_set_cloexec(g_close_pipe[0]) == -1 ||
        fd_set_cloexec(g_close_pipe[1]) == -1) {
        LOG_ERROR("epoll_worker_init: fd_set_nonblocking/cloexec failed: %s",
                  strerror(errno));
        close(g_close_pipe[0]);
        close(g_close_pipe[1]);
        g_close_pipe[0] = -1;
        g_close_pipe[1] = -1;
        return -1;
    }

    return 0;
}

void epoll_worker_destroy(void)
{
    if (g_close_pipe[0] != -1) {
        close(g_close_pipe[0]);
        g_close_pipe[0] = -1;
    }
    if (g_close_pipe[1] != -1) {
        close(g_close_pipe[1]);
        g_close_pipe[1] = -1;
    }
}

void epoll_worker_request_stop(void)
{
    /*
     * 优雅停机请求：可由信号处理器调用，因此只做一次标志
     * 写入，不加锁、不分配、不记日志（async-signal-safe）。事件循环经
     * epoll_wait 超时轮询读到该标志后走统一清理路径退出。
     */
    g_stop_requested = 1;
}

void epoll_worker_request_stats(void)
{
    g_stats_requested = 1;
}

void epoll_worker_set_stats_hook(void (*hook)(void))
{
    g_stats_hook = hook;
}

void epoll_worker_get_stats(struct epoll_stats *out)
{
    if (out == NULL) {
        return;
    }

    out->active_conns    = (uint64_t)atomic_load_explicit(&g_active_conns,
                                                          memory_order_relaxed);
    out->accepted_conns  = (uint64_t)atomic_load_explicit(&g_accepted_conns,
                                                          memory_order_relaxed);
    out->pool_rejected   = (uint64_t)atomic_load_explicit(&g_pool_rejected,
                                                          memory_order_relaxed);
    out->downlink_sent   = (uint64_t)atomic_load_explicit(&g_downlink_sent,
                                                          memory_order_relaxed);
    out->downlink_failed = (uint64_t)atomic_load_explicit(&g_downlink_failed,
                                                          memory_order_relaxed);
}

void epoll_worker_request_close(int fd, uint64_t generation)
{
    struct pipe_msg msg;

    if (g_close_pipe[1] < 0) {
        LOG_ERROR("epoll_worker_request_close: pipe not initialized, fd=%d", fd);
        return;
    }

    /*
     * Write a CLOSE message into the self-pipe. The epoll thread will read it
     * and perform full cleanup via connection_close(). If the pipe is full
     * (extremely unlikely with 64KB capacity), the connection stays open
     * and will be retried on the next heartbeat scan. We do NOT fall back
     * to a direct close() here, as that would race with the epoll thread
     * and leak the connection struct.
     */
    memset(&msg, 0, sizeof(msg)); /* data[] 清零，避免向 pipe 写入未初始化字节。 */
    msg.type = PIPE_MSG_CLOSE;
    msg.fd = fd;
    msg.generation = generation;
    msg.len = 0;

    ssize_t n;
    do {
        n = write(g_close_pipe[1], &msg, sizeof(msg));
    } while (n == -1 && errno == EINTR);
    if (n == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR("epoll_worker_request_close: pipe write failed fd=%d: %s",
                      fd, strerror(errno));
        }
    } else if (n != (ssize_t)sizeof(msg)) {
        LOG_ERROR("epoll_worker_request_close: pipe short write fd=%d "
                  "(%zd/%zu bytes)", fd, n, sizeof(msg));
    }
}

int epoll_worker_request_send(int fd, uint64_t generation,
                              const uint8_t *frame, size_t len)
{
    struct pipe_msg msg;

    if (g_close_pipe[1] < 0) {
        LOG_ERROR("epoll_worker_request_send: pipe not initialized, fd=%d", fd);
        return -1;
    }

    if (frame == NULL || len == 0 || len > PROTOCOL_MAX_FRAME_SIZE) {
        LOG_ERROR("epoll_worker_request_send: invalid frame fd=%d len=%zu",
                  fd, len);
        errno = EINVAL;
        return -1;
    }

    memset(&msg, 0, sizeof(msg)); /* data[] 清零，避免向 pipe 写入未初始化字节。 */
    msg.type = PIPE_MSG_SEND;
    msg.fd = fd;
    msg.generation = generation;
    msg.len = (uint16_t)len;
    memcpy(msg.data, frame, len);

    ssize_t n;
    do {
        n = write(g_close_pipe[1], &msg, sizeof(msg));
    } while (n == -1 && errno == EINTR);
    if (n != (ssize_t)sizeof(msg)) {
        LOG_ERROR("epoll_worker_request_send: pipe write failed fd=%d: %s",
                  fd, strerror(errno));
        return -1;
    }
    return 0;
}


/**
 * @brief 向 epoll 实例注册一个文件描述符。
 *
 * 使用 ET 模式，因此调用方负责在每次通知后排空 fd。
 *
 * @param epoll_fd epoll 实例。
 * @param fd       待注册的文件描述符。
 * @param conn     存储在 event.data.ptr 中的连接上下文指针。
 * @return 成功返回 0，失败返回 -1。
 */
static int epoll_add(int epoll_fd, int fd, struct connection *conn)
{
    struct epoll_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = conn;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        LOG_ERROR("epoll_add: epoll_ctl ADD fd=%d failed: %s",
                  fd, strerror(errno));
        return -1;
    }

    return 0;
}

/**
 * @brief 从 epoll 实例移除一个文件描述符并关闭它。
 *
 * 设计说明（陈旧事件与双重释放防护）：
 *   epoll_wait 返回的 events[] 是调用时刻的快照。同一批次内，先处理的
 *   pipe 消息（如心跳超时关闭）可能已把某连接关闭，而该连接的
 *   EPOLLIN/EPOLLHUP 事件仍留在批内后续位置。若对此类“陈旧事件”再次
 *   执行本函数，旧实现会把同一对象池节点归还空闲链两次，空闲链自环，
 *   之后所有新连接共享同一连接上下文（池整体腐坏）。防护分两层：
 *     1. closed 标志：首次进入即置位，重入直接返回；
 *     2. 批末延迟归还：节点挂入 trash 链而非空闲链，批内槽位不可能被
 *        accept 复用，陈旧事件即使到达也只能看到 closed=1 被跳过，
 *        不会误伤复用该槽位的新连接。
 *
 * @param epoll_fd epoll 实例。
 * @param conn     待清理的连接上下文。
 */
static void connection_close(int epoll_fd, struct connection *conn)
{
    if (conn == NULL || conn->closed) {
        /* conn == NULL 防御；closed 表示同批内已关闭（陈旧事件重入）。 */
        return;
    }

    conn->closed = 1;

    /* Remove from the client connection list & hash table. */
    if (conn->role == CONN_ROLE_CLIENT) {
        conn_list_remove(conn);
        atomic_fetch_sub_explicit(&g_active_conns, 1, memory_order_relaxed);
    }

    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL) == -1) {
        /*
         * 忽略 EBADF/ENOENT：如果对端重置或 fd 尚未加入 epoll（如注册失败回滚），
         * 该 fd 可能已不在 epoll 实例中。记录其他错误以用于诊断。
         */
        if (errno != EBADF && errno != ENOENT) {
            LOG_ERROR("connection_close: epoll_ctl DEL fd=%d failed: %s",
                      conn->fd, strerror(errno));
        }
    }

    if (conn->role == CONN_ROLE_CLIENT) {
        /* 释放解析器上下文及其每连接RingBuffer。*/
        packet_parser_unregister(conn->fd);
        /* 从连接管理器移除（心跳检测）。*/
        conn_manager_remove(conn->fd);
    }

    close(conn->fd);

    /* 挂入 trash 链，待本批事件处理完毕后再归还空闲链（见函数头注释）。
     * 仅客户端连接来自对象池；listen/pipe 上下文为独立 calloc，
     * 由 epoll_worker_run 退出路径释放，不得进入池。 */
    if (conn->role == CONN_ROLE_CLIENT) {
        conn->next = g_conn_trash;
        g_conn_trash = conn;
    }
}

/**
 * @brief 接受监听 socket 上的所有待处理连接。
 *
 * 在 ET 模式下，epoll_wait() 仅在监听 backlog 从空变为非空时返回一次，
 * 我们必须循环 accept() 直到返回 -1 且 errno == EAGAIN/EWOULDBLOCK，
 * 表示就绪队列已排空。
 *
 * @param epoll_fd  epoll 实例。
 * @param listen_fd 监听 socket。
 */
static void handle_listen(int epoll_fd, int listen_fd)
{
    for (;;) {
        int client_fd;
        struct connection *conn;

        client_fd = server_accept(listen_fd);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 所有待处理的连接均已接受。*/
                break;
            }

            /*
             * 其余错误：ECONNABORTED/EPROTO 已在 server_accept
             * 内部重试排空；EMFILE/ENFILE 已通过 reserve fd 排空拒绝。
             * 到达这里即本轮 accept 结束，等待下一次 epoll 事件。
             */
            LOG_ERROR("handle_listen: accept failed: %s",
                      strerror(errno));
            break;
        }

        conn = conn_pool_acquire();
        if (conn == NULL) {
            atomic_fetch_add_explicit(&g_pool_rejected, 1,
                                      memory_order_relaxed);
            LOG_ERROR("handle_listen: connection pool exhausted (%d), "
                      "closing fd=%d", CONN_POOL_CAPACITY, client_fd);
            close(client_fd);
            continue;
        }

        /* 为该连接注册专用的解析器上下文（含 RingBuffer）并缓存直连指针。*/
        if (packet_parser_register(client_fd, &conn->parser) != 0) {
            LOG_ERROR("handle_listen: packet_parser_register failed, "
                      "closing fd=%d", client_fd);
            conn_pool_release(conn);
            close(client_fd);
            continue;
        }

        /* 分配连接代际号：fd 复用后据此区分新旧连接。 */
        uint64_t gen = ++g_generation;

        if (conn_manager_add(client_fd, gen) != 0) {
            LOG_ERROR("handle_listen: conn_manager_add failed, closing fd=%d",
                      client_fd);
            packet_parser_unregister(client_fd);
            conn_pool_release(conn);
            close(client_fd);
            continue;
        }

        conn->fd = client_fd;
        conn->generation = gen;
        conn->role = CONN_ROLE_CLIENT;
        conn_list_add(conn);
        atomic_fetch_add_explicit(&g_active_conns, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_accepted_conns, 1, memory_order_relaxed);

        if (epoll_add(epoll_fd, client_fd, conn) == -1) {
            connection_close(epoll_fd, conn);
            continue;
        }
    }
}

/**
 * @brief 在 ET 模式下从客户端 socket 读取所有可用数据。
 *
 * 使用 EPOLLET 时，epoll_wait() 仅当 socket 从不可读变为可读时通知我们，
 * 我们必须循环 recv() 直到返回 -1 且 errno == EAGAIN/EWOULDBLOCK，以
 * 确保内核接收缓冲区中的每个字节都被移入连接 RingBuffer。如果字节遗留
 * 在接收缓冲区中，它们将一直处于内核缓冲区，且在收到新数据之前不会有
 * epoll 通知。
 *
 * @param epoll_fd epoll 实例。
 * @param conn     客户端连接上下文。
 */
static void handle_client(int epoll_fd, struct connection *conn)
{
    char recv_buf[4096];
    int received_any = 0;

    for (;;) {
        ssize_t n;

        n = recv(conn->fd, recv_buf, sizeof(recv_buf), 0);
        if (n > 0) {
            /*
             * 将原始字节直连送入每连接解析器（零查表）。状态机从 RingBuffer
             * 消费字节，并通过 on_frame_complete() 递送完整帧。
             */
            if (packet_parser_feed_ctx(conn->parser,
                                       (const uint8_t *)recv_buf,
                                       (size_t)n) != 0) {
                LOG_ERROR("handle_client: packet_parser_feed fd=%d failed",
                          conn->fd);
                connection_close(epoll_fd, conn);
                return;
            }

            received_any = 1;
            continue;
        }

        if (n == 0) {
            /* 对端执行了正常关闭。*/
            connection_close(epoll_fd, conn);
            return;
        }

        /* n == -1 */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* 内核接收缓冲区已完全排空；合并刷新一次心跳活动时间戳。*/
            if (received_any) {
                conn_manager_update_active_time(conn->fd);
            }
            return;
        }

        if (errno == EINTR) {
            /* 被信号中断，立即重试。*/
            continue;
        }

        /* 致命 recv 错误，关闭连接。*/
        LOG_ERROR("handle_client: recv fd=%d failed: %s",
                  conn->fd, strerror(errno));
        connection_close(epoll_fd, conn);
        return;
    }
}
/*
 * Read messages from the self-pipe and act on them. Called from the epoll
 * event loop when the pipe is readable.
 *
 * 消息类型：
 *   - PIPE_MSG_CLOSE：心跳线程委托的关闭请求 → connection_close()。
 *   - PIPE_MSG_SEND：工作线程委托的下行发送（如 CMD_CONFIG 响应）→ send()。
 * 两者都携带 fd + generation 双重匹配，fd 复用后不会误关/误发新连接。
 *
 * ET mode: loop read() until EAGAIN to drain all pending messages.
 */
static void handle_pipe_messages(int epoll_fd)
{
    struct pipe_msg msgs[32];
    ssize_t n;
    size_t i;

    for (;;) {
        n = read(g_close_pipe[0], msgs, sizeof(msgs));
        if (n <= 0) {
            if (n == -1 && errno == EINTR) {
                continue;
            }
            break; /* EAGAIN or EOF */
        }

        for (i = 0; i < (size_t)n / sizeof(struct pipe_msg); i++) {
            struct pipe_msg *msg = &msgs[i];
            struct connection *conn =
                conn_list_find(msg->fd, msg->generation);

            if (conn == NULL) {
                /* 连接已关闭或 fd 被新连接复用（generation 不匹配）：
                 * 忽略过期请求，这是 fd 复用防护。 */
                if (msg->type == PIPE_MSG_SEND) {
                    atomic_fetch_add_explicit(&g_downlink_failed, 1,
                                              memory_order_relaxed);
                }
                continue;
            }

            if (msg->type == PIPE_MSG_CLOSE) {
                LOG_INFO("epoll: closing fd=%d (gen=%llu) by heartbeat request",
                         msg->fd,
                         (unsigned long long)msg->generation);
                connection_close(epoll_fd, conn);
            } else if (msg->type == PIPE_MSG_SEND) {
                /* 下行发送：best-effort。socket 非阻塞，缓冲区满（EAGAIN）
                 * 或连接已断开时直接丢弃并记日志，配置响应可重发。 */
                ssize_t sent;
                do {
                    sent = send(conn->fd, msg->data, msg->len, MSG_NOSIGNAL);
                } while (sent == -1 && errno == EINTR);
                if (sent < 0) {
                    atomic_fetch_add_explicit(&g_downlink_failed, 1,
                                              memory_order_relaxed);
                    LOG_ERROR("epoll: send downlink fd=%d failed: %s",
                              msg->fd, strerror(errno));
                } else if (sent != (ssize_t)msg->len) {
                    atomic_fetch_add_explicit(&g_downlink_failed, 1,
                                              memory_order_relaxed);
                    LOG_ERROR("epoll: send downlink fd=%d partial (%zd/%u bytes)",
                              msg->fd, sent, (unsigned int)msg->len);
                } else {
                    atomic_fetch_add_explicit(&g_downlink_sent, 1,
                                              memory_order_relaxed);
                    LOG_DEBUG("epoll: sent downlink frame to fd=%d (%u bytes)",
                              msg->fd, (unsigned int)msg->len);
                }
            }
        }
    }
}



int epoll_worker_run(int listen_fd)
{
    int epoll_fd;
    struct connection *listen_conn;
    struct connection *pipe_conn = NULL;
    struct epoll_event events[EPOLL_MAX_EVENTS];
    int i;

    if (listen_fd < 0) {
        errno = EBADF;
        return -1;
    }

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1) {
        LOG_ERROR("epoll_worker_run: epoll_create1 failed: %s",
                  strerror(errno));
        return -1;
    }

    listen_conn = calloc(1, sizeof(*listen_conn));
    if (listen_conn == NULL) {
        close(epoll_fd);
        return -1;
    }

    listen_conn->fd = listen_fd;
    listen_conn->role = CONN_ROLE_LISTEN;

    if (epoll_add(epoll_fd, listen_fd, listen_conn) == -1) {
        free(listen_conn);
        close(epoll_fd);
        return -1;
    }

    /* Register the close-request pipe read end (if initialized). */
    if (g_close_pipe[0] >= 0) {
        pipe_conn = calloc(1, sizeof(*pipe_conn));
        if (pipe_conn == NULL) {
            LOG_ERROR("epoll_worker_run: calloc pipe_conn failed");
            free(listen_conn);
            close(epoll_fd);
            return -1;
        }
        pipe_conn->fd = g_close_pipe[0];
        pipe_conn->role = CONN_ROLE_PIPE;
        if (epoll_add(epoll_fd, g_close_pipe[0], pipe_conn) == -1) {
            free(pipe_conn);
            free(listen_conn);
            close(epoll_fd);
            return -1;
        }
    }

    for (;;) {
        int nfds;

        /*
         * 优雅停机检测：epoll_wait 带超时（而非 -1 永久阻塞），
         * 每半个秒醒来轮询一次停机标志；信号处理器只置标志、不做其他
         * 系统调用，事件循环在此处统一响应。
         */
        if (g_stop_requested) {
            break;
        }

        if (g_stats_requested) {
            g_stats_requested = 0;
            if (g_stats_hook != NULL) {
                g_stats_hook();
            }
        }

        nfds = epoll_wait(epoll_fd, events, EPOLL_MAX_EVENTS,
                          EPOLL_POLL_TIMEOUT_MS);
        if (nfds == -1) {
            if (errno == EINTR) {
                /* 被信号中断，直接重试。*/
                continue;
            }

            LOG_ERROR("epoll_worker_run: epoll_wait failed: %s",
                      strerror(errno));
            break;
        }

        if (nfds == 0) {
            /* 超时无事件：回到循环顶轮询停机标志。 */
            continue;
        }

        for (i = 0; i < nfds; i++) {
            struct connection *conn;
            uint32_t ev;

            conn = events[i].data.ptr;
            if (conn == NULL) {
                continue;
            }

            /*
             * 设计说明（陈旧事件防护）：
             * events[] 是 epoll_wait 时刻的快照，本批更早处理的 pipe
             * 消息可能已关闭该连接（closed=1，槽位在 trash 链待批末回收）。
             * 直接跳过，防止对已关闭连接二次 recv/close；由于槽位批内
             * 不会被 accept 复用，closed=1 一定属于“旧连接”而非新连接。
             */
            if (conn->role == CONN_ROLE_CLIENT && conn->closed) {
                continue;
            }

            ev = events[i].events;

            /*
             * 设计说明（EPOLLIN | EPOLLHUP 同批处理顺序）：
             * 短连接客户端发完最后一帧后立刻 close，内核可能在同一事件中
             * 同时上报 EPOLLIN | EPOLLHUP。必须先处理 EPOLLIN 排空内核接收
             * 缓冲区，再检查 EPOLLERR | EPOLLHUP 执行关闭；若先关闭并 continue，
             * 最后到达的有效帧会被静默丢弃。
             */
            if (ev & EPOLLIN) {
                if (conn->role == CONN_ROLE_LISTEN) {
                    handle_listen(epoll_fd, conn->fd);
                } else if (conn->role == CONN_ROLE_PIPE) {
                    handle_pipe_messages(epoll_fd);
                } else {
                    handle_client(epoll_fd, conn);
                }
            }

            /* 错误/挂起事件意味着应该关闭连接（仅尚未关闭的客户端连接）。 */
            if (!conn->closed && (ev & (EPOLLERR | EPOLLHUP))) {
                if (conn->role == CONN_ROLE_CLIENT) {
                    connection_close(epoll_fd, conn);
                }
            }
        }

        /* 本批事件全部处理完毕，统一归还批内关闭的连接槽（见
         * connection_close 处设计说明）。此后槽位才允许被 accept 复用。 */
        conn_pool_flush_trash();
    }

    /*
     * 退出清理（无论是 SIGTERM/SIGINT 优雅停机还是致命 epoll 错误）：
     * 统一关闭全部存活客户端连接，避免 fd 与 parse_ctx 泄漏。
     */
    {
        int stopped = (g_stop_requested != 0);

        while (g_conn_list != NULL) {
            connection_close(epoll_fd, g_conn_list);
        }
        conn_pool_flush_trash();
        g_stop_requested = 0;

        if (stopped) {
            LOG_INFO("epoll_worker_run: graceful shutdown, all client "
                     "connections closed");
        }

        free(listen_conn);
        free(pipe_conn); /* NULL-safe */
        close(epoll_fd);

        return stopped ? 0 : -1;
    }
}

/*
 * packet_parser 在完整帧组装完毕后调用的回调函数（本文件为 epoll 线程
 * 上下文）：把完整帧连同连接代际号打包成任务，交给工作线程池处理。
 */
void on_frame_complete(int fd, const uint8_t *frame, size_t len)
{
    struct task t;
    struct connection *conn;

    if (frame == NULL || len < PACKET_MIN_SIZE || len > sizeof(t.frame)) {
        LOG_ERROR("on_frame_complete: invalid frame from fd=%d len=%zu",
                  fd, len);
        return;
    }

    t.fd = fd;

    /* 直接在 epoll 线程 O(1) 无锁获取代际号，消除每帧互斥锁竞争。 */
    conn = conn_find_by_fd(fd);
    t.generation = (conn != NULL) ? conn->generation : 0;

    t.frame_len = len;
    memcpy(t.frame, frame, len);

    /* 从协议帧中提取命令字（偏移量 4，见 protocol.h 帧布局）。 */
    t.cmd = t.frame[4];

    if (threadpool_submit(&t) != 0) {
        LOG_ERROR("on_frame_complete: threadpool_submit failed for fd=%d", fd);
    }
}
