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
#include "utils/threadpool.h"
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
    struct connection *next; /* singly-linked list of client connections */
};

/* 连接代际计数器（仅 epoll 线程访问，无需加锁）。 */
static uint64_t g_generation = 0;

/* 客户端连接上下文对象池容量（固定上限：超出后 accept 被拒绝并记日志）。 */
#define CONN_POOL_CAPACITY 1024

/* 客户端连接上下文对象池：固定数组 + LIFO 空闲链。
 * 仅 epoll 线程访问（accept/connection_close 均在其上下文中），无需加锁，
 * 符合 AGENTS §2.3「只实现项目够用的连接池」。 */
static struct connection g_conn_pool[CONN_POOL_CAPACITY];
static struct connection *g_conn_free = NULL;

static void conn_pool_init(void)
{
    size_t i;

    if (g_conn_free != NULL) {
        return;
    }

    for (i = 0; i < CONN_POOL_CAPACITY; i++) {
        g_conn_pool[i].next = g_conn_free;
        g_conn_free = &g_conn_pool[i];
    }
}

static struct connection *conn_pool_acquire(void)
{
    struct connection *conn;

    if (g_conn_free == NULL) {
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

/**
 * @brief 将文件描述符设置为非阻塞模式。
 *
 * server.c 中已有重复的辅助函数；保留在此处以保持模块自包含，
 * 后续可移到共享工具模块。
 *
 * @param fd 待修改的文件描述符。
 * @return 成功返回 0，失败返回 -1 并设置 errno。
 */
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

    ssize_t n = write(g_close_pipe[1], &msg, sizeof(msg));
    if (n != (ssize_t)sizeof(msg) && errno != EAGAIN) {
        LOG_ERROR("epoll_worker_request_close: pipe write failed fd=%d: %s",
                  fd, strerror(errno));
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

    ssize_t n = write(g_close_pipe[1], &msg, sizeof(msg));
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
 * @param epoll_fd epoll 实例。
 * @param conn     待清理的连接上下文。
 */
static void connection_close(int epoll_fd, struct connection *conn)
{
    if (conn == NULL) {
        return;
    }

    /* Remove from the client connection list. */
    conn_list_remove(conn);

    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL) == -1) {
        /*
         * 忽略 EBADF：如果对端重置了连接，该 fd 可能已被内核关闭。
         * 记录其他错误以用于诊断。
         */
        if (errno != EBADF) {
            LOG_ERROR("connection_close: epoll_ctl DEL fd=%d failed: %s",
                      conn->fd, strerror(errno));
        }
    }

    if (conn->role != CONN_ROLE_LISTEN) {
        /* 释放解析器上下文及其每连接RingBuffer。*/
        packet_parser_unregister(conn->fd);
        /* 从连接管理器移除（心跳检测）。*/
        conn_manager_remove(conn->fd);
    }

    close(conn->fd);
    conn_pool_release(conn);
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

        conn = conn_pool_acquire();
        if (conn == NULL) {
            LOG_ERROR("handle_listen: connection pool exhausted (%d), "
                      "closing fd=%d", CONN_POOL_CAPACITY, client_fd);
            conn_manager_remove(client_fd);
            packet_parser_unregister(client_fd);
            close(client_fd);
            continue;
        }

        conn->fd = client_fd;
        conn->generation = gen;
        conn->role = CONN_ROLE_CLIENT;
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
                ssize_t sent = send(conn->fd, msg->data, msg->len,
                                    MSG_NOSIGNAL);
                if (sent != (ssize_t)msg->len) {
                    LOG_ERROR("epoll: send downlink fd=%d failed: %s",
                              msg->fd, strerror(errno));
                } else {
                    LOG_INFO("epoll: sent downlink frame to fd=%d (%u bytes)",
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
        } else {
            pipe_conn->fd = g_close_pipe[0];
            pipe_conn->role = CONN_ROLE_PIPE;
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

            /* 错误/挂起事件也意味着应该关闭连接（仅客户端连接）。 */
            if (ev & (EPOLLERR | EPOLLHUP)) {
                if (conn->role == CONN_ROLE_CLIENT) {
                    connection_close(epoll_fd, conn);
                }
                continue;
            }

            if (ev & EPOLLIN) {
                if (conn->role == CONN_ROLE_LISTEN) {
                    handle_listen(epoll_fd, conn->fd);
                } else if (conn->role == CONN_ROLE_PIPE) {
                    handle_pipe_messages(epoll_fd);
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

/*
 * packet_parser 在完整帧组装完毕后调用的回调函数（本文件为 epoll 线程
 * 上下文）：把完整帧连同连接代际号打包成任务，交给工作线程池处理。
 */
void on_frame_complete(int fd, const uint8_t *frame, size_t len)
{
    struct task t;

    if (frame == NULL || len == 0 || len > sizeof(t.frame)) {
        LOG_ERROR("on_frame_complete: invalid frame from fd=%d", fd);
        return;
    }

    memset(&t, 0, sizeof(t));
    t.fd = fd;

    /* 记录连接代际号：下行响应（如 CMD_CONFIG）发送前据此防 fd 复用误发。 */
    if (conn_manager_get_generation(fd, &t.generation) != 0) {
        /* 理论上不会发生（本回调运行在 epoll 线程、连接尚存活）；
         * 防御性置 0，使后续下行发送被 generation 匹配拒绝。 */
        t.generation = 0;
    }

    t.frame_len = len;
    memcpy(t.frame, frame, len);

    /* 从协议帧中提取命令字（偏移量 4，见 protocol.h 帧布局）。 */
    t.cmd = t.frame[4];

    if (threadpool_submit(&t) != 0) {
        LOG_ERROR("on_frame_complete: threadpool_submit failed for fd=%d", fd);
    }
}
