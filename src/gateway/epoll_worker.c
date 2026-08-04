#include "gateway/epoll_worker.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "gateway/conn_manager.h"
#include "utils/logger.h"
#include "protocol/packet_parser.h"
#include "gateway/server.h"

/* 每次 epoll_wait() 调用处理的最大事件数。*/
#define EPOLL_MAX_EVENTS 1024

/* Self-pipe for cross-thread close requests: heartbeat thread writes fds,
 * epoll thread reads them and performs full connection cleanup. */
static int g_close_pipe[2] = { -1, -1 };

/* Singly-linked list of active client connections, for fd->conn lookup.
 * Only accessed by the epoll thread, no lock needed. */
static struct connection *g_conn_list = NULL;

/**
 * @brief 存储在epoll_event.data.ptr 中的每连接上下文。 *
 * packet_parser 内部拥有每连接RingBuffer，因此epoll_worker 只需
 * 跟踪 fd 以及该上下文是否属于监听 socket。 */
struct connection {
    int       fd;
    uint64_t  generation; /* 连接代际号：accept 时递增分配，fd 复用后用于区分。 */
    int       is_listen;  /* 1 = listen socket, 0 = client, 2 = close-pipe sentinel */
    struct connection *next; /* singly-linked list of client connections */
}; /* 1 表示监听 socket，0 表示客户端socket。*/

/* 连接代际计数器（仅 epoll 线程访问，无需加锁）。 */
static uint64_t g_generation = 0;

/* self-pipe 载荷：fd + generation 双重匹配，防止 fd 复用后误关新连接。 */
struct close_request {
    int      fd;
    uint64_t generation;
};

/**
 * @brief 将文件描述符设置为非阻塞模式。 *
 * server.c 中已有重复的辅助函数；保留在此处以保持模块自包含。 * 后续可移到共享工具模块。 *
 * @param fd 待修改的文件描述符。 *
 * @return 成功返回 0，失败返回-1 并设置errno。 */
static int set_nonblocking(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return -1;
    }

    return 0;
}
/* =========================================================================
 * Connection list helpers (epoll thread only, no lock needed)
 * ========================================================================= */

static void conn_list_add(struct connection *conn)
{
    conn->next = g_conn_list;
    g_conn_list = conn;
}

static void conn_list_remove(struct connection *conn)
{
    struct connection **pp = &g_conn_list;
    while (*pp != NULL) {
        if (*pp == conn) {
            *pp = conn->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static struct connection *conn_list_find(int fd, uint64_t generation)
{
    struct connection *conn;
    for (conn = g_conn_list; conn != NULL; conn = conn->next) {
        if (conn->fd == fd && conn->generation == generation) {
            return conn;
        }
    }
    return NULL;
}

/* =========================================================================
 * Public API: init / destroy / request_close
 * ========================================================================= */

int epoll_worker_init(void)
{
    if (g_close_pipe[0] != -1) {
        return 0; /* already initialized */
    }

    if (pipe(g_close_pipe) != 0) {
        LOG_ERROR("epoll_worker_init: pipe failed: %s", strerror(errno));
        return -1;
    }

    if (set_nonblocking(g_close_pipe[0]) == -1 ||
        set_nonblocking(g_close_pipe[1]) == -1) {
        LOG_ERROR("epoll_worker_init: set_nonblocking failed: %s",
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

void epoll_worker_request_close(int fd, uint64_t generation)
{
    struct close_request req;

    if (g_close_pipe[1] < 0) {
        LOG_ERROR("epoll_worker_request_close: pipe not initialized, fd=%d", fd);
        return;
    }

    /*
     * Write {fd, generation} into the self-pipe. The epoll thread will read it
     * and perform full cleanup via connection_close(). If the pipe is full
     * (extremely unlikely with 64KB capacity), the connection stays open
     * and will be retried on the next heartbeat scan. We do NOT fall back
     * to a direct close() here, as that would race with the epoll thread
     * and leak the connection struct.
     */
    req.fd = fd;
    req.generation = generation;

    ssize_t n = write(g_close_pipe[1], &req, sizeof(req));
    if (n != (ssize_t)sizeof(req) && errno != EAGAIN) {
        LOG_ERROR("epoll_worker_request_close: pipe write failed fd=%d: %s",
                  fd, strerror(errno));
    }
}


/**
 * @brief 向epoll 实例注册一个文件描述符。 *
 * 使用 ET 模式，因此调用方负责在每次通知后排空fd。 *
 * @param epoll_fd epoll 实例。 * @param fd       待注册的文件描述符。 * @param conn     存储在event.data.ptr 中的连接上下文指针。 *
 * @return 成功返回 0，失败返回-1。 */
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
 * @brief 从epoll 实例移除一个文件描述符并关闭它。 *
 * @param epoll_fd epoll 实例。 * @param conn     待清理的连接上下文。 */
static void connection_close(int epoll_fd, struct connection *conn)
{
    if (conn == NULL) {
        return;
    }

    /* Remove from the client connection list. */
    conn_list_remove(conn);

    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL) == -1) {
        /*
         * 忽略 EBADF：如果对端重置了连接，该 fd 可能已被内核关闭。         * 记录其他错误以用于诊断。         */
        if (errno != EBADF) {
            LOG_ERROR("connection_close: epoll_ctl DEL fd=%d failed: %s",
                      conn->fd, strerror(errno));
        }
    }

    if (!conn->is_listen) {
        /* 释放解析器上下文及其每连接RingBuffer。*/
        packet_parser_unregister(conn->fd);
        /* 从连接管理器移除（心跳检测）。*/
        conn_manager_remove(conn->fd);
    }

    close(conn->fd);
    free(conn);
}

/**
 * @brief 接受监听 socket 上的所有待处理连接。 *
 * 在ET 模式下，epoll_wait() 仅在监听 backlog 从空变为非空时返回一次。 * 我们必须循环 accept() 直到返回 -1 且errno == EAGAIN/EWOULDBLOCK， * 表示就绪队列已排空。 *
 * @param epoll_fd  epoll 实例。 * @param listen_fd 监听 socket。 */
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

            LOG_ERROR("handle_listen: accept failed: %s",
                      strerror(errno));
            break;
        }

        /* 为该连接注册专用的解析器上下文（含RingBuffer）。*/
        if (packet_parser_register(client_fd, NULL) != 0) {
            LOG_ERROR("handle_listen: packet_parser_register failed, "
                      "closing fd=%d", client_fd);
            close(client_fd);
            continue;
        }

        /* 分配连接代际号：fd 复用后据此区分新旧连接。 */
        uint64_t gen = ++g_generation;

        if (conn_manager_add(client_fd, gen) != 0) {
            LOG_ERROR("handle_listen: conn_manager_add failed, closing fd=%d",
                      client_fd);
            packet_parser_unregister(client_fd);
            close(client_fd);
            continue;
        }

        conn = calloc(1, sizeof(*conn));
        if (conn == NULL) {
            LOG_ERROR("handle_listen: calloc failed, closing fd=%d",
                      client_fd);
            conn_manager_remove(client_fd);
            packet_parser_unregister(client_fd);
            close(client_fd);
            continue;
        }

        conn->fd = client_fd;
        conn->generation = gen;
        conn->is_listen = 0;
        conn_list_add(conn);

        /*
         * server_accept() 已设置socket 为非阻塞，但防御性地再检查一次         * 以应对实现可能变化的情况。         */
        if (set_nonblocking(client_fd) == -1) {
            LOG_ERROR("handle_listen: set_nonblocking failed, closing fd=%d",
                      client_fd);
            connection_close(epoll_fd, conn);
            continue;
        }

        if (epoll_add(epoll_fd, client_fd, conn) == -1) {
            connection_close(epoll_fd, conn);
            continue;
        }
    }
}

/**
 * @brief 在ET 模式下从客户端socket 读取所有可用数据。 *
 * 使用 EPOLLET 时，epoll_wait() 仅当 socket 从不可读变为可读时通知我们。 * 我们必须循环 recv() 直到返回 -1 且errno == EAGAIN/EWOULDBLOCK，以确保
 * 内核接收缓冲区中的每个字节都被移入连接RingBuffer。如果字节遗留在接收
 * 缓冲区中，它们将一直处于内核缓冲区，且在收到新数据之前不会有epoll 通知。 *
 * @param epoll_fd epoll 实例。 * @param conn     客户端连接上下文。 */
static void handle_client(int epoll_fd, struct connection *conn)
{
    char recv_buf[4096];

    for (;;) {
        ssize_t n;

        n = recv(conn->fd, recv_buf, sizeof(recv_buf), 0);
        if (n > 0) {
            /*
             * 将原始字节送入每连接解析器。解析器的状态机从RingBuffer 中             * 消费字节，并通过 on_frame_complete() 递送完整帧。             */
            if (packet_parser_feed(conn->fd,
                                   (const uint8_t *)recv_buf,
                                   (size_t)n) != 0) {
                LOG_ERROR("handle_client: packet_parser_feed fd=%d failed",
                          conn->fd);
                connection_close(epoll_fd, conn);
                return;
            }

            /* 更新该连接的心跳活动时间戳。*/
            conn_manager_update_active_time(conn->fd);

            continue;
        }

        if (n == 0) {
            /* 对端执行了正常关闭。*/
            connection_close(epoll_fd, conn);
            return;
        }

        /* n == -1 */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* 内核接收缓冲区已完全排空。*/
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
 * Read fds from the close-request self-pipe and close the corresponding
 * connections. Called from the epoll event loop when the pipe is readable.
 *
 * ET mode: loop read() until EAGAIN to drain all pending close requests.
 */
static void handle_close_requests(int epoll_fd)
{
    struct close_request reqs[32];
    ssize_t n;
    size_t i;

    for (;;) {
        n = read(g_close_pipe[0], reqs, sizeof(reqs));
        if (n <= 0) {
            if (n == -1 && errno == EINTR) {
                continue;
            }
            break; /* EAGAIN or EOF */
        }

        for (i = 0; i < (size_t)n / sizeof(struct close_request); i++) {
            struct connection *conn =
                conn_list_find(reqs[i].fd, reqs[i].generation);
            if (conn != NULL) {
                LOG_INFO("epoll: closing fd=%d (gen=%llu) by heartbeat request",
                         reqs[i].fd,
                         (unsigned long long)reqs[i].generation);
                connection_close(epoll_fd, conn);
            }
            /* If conn is NULL, either the connection was already closed by
             * the epoll thread (e.g., peer sent FIN), or the fd has been
             * reused by a newer connection (generation mismatch). Either
             * way, no action is needed — this is the fd-reuse guard. */
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
    listen_conn->is_listen = 1;

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
        } else {
            pipe_conn->fd = g_close_pipe[0];
            pipe_conn->is_listen = 2; /* sentinel for close pipe */
            if (epoll_add(epoll_fd, g_close_pipe[0], pipe_conn) == -1) {
                free(pipe_conn);
                pipe_conn = NULL;
            }
        }
    }

    for (;;) {
        int nfds;

        nfds = epoll_wait(epoll_fd, events, EPOLL_MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) {
                /* 被信号中断，直接重试。*/
                continue;
            }

            LOG_ERROR("epoll_worker_run: epoll_wait failed: %s",
                      strerror(errno));
            break;
        }

        for (i = 0; i < nfds; i++) {
            struct connection *conn;
            uint32_t ev;

            conn = events[i].data.ptr;
            if (conn == NULL) {
                continue;
            }

            ev = events[i].events;

            /* 错误/挂起事件也意味着应该关闭连接。*/
            if (ev & (EPOLLERR | EPOLLHUP)) {
                if (!conn->is_listen) {
                    connection_close(epoll_fd, conn);
                }
                continue;
            }

            if (ev & EPOLLIN) {
                if (conn->is_listen == 1) {
                    handle_listen(epoll_fd, conn->fd);
                } else if (conn->is_listen == 2) {
                    handle_close_requests(epoll_fd);
                } else {
                    handle_client(epoll_fd, conn);
                }
            }
        }
    }

    /*
     * 在实践中上述循环永远运行。到达此处意味着发生了致命的 epoll_wait 错误。     * 清理监听器上下文，但将客户端清理留给进程退出处理。     */
    free(listen_conn);
    free(pipe_conn); /* NULL-safe */
    close(epoll_fd);

    return -1;
}
