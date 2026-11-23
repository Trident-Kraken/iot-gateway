#define _POSIX_C_SOURCE 200809L /* O_CLOEXEC */

#include "gateway/server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "utils/fd_util.h"
#include "utils/logger.h"

#define SERVER_LISTEN_BACKLOG 128

/*
 * reserve fd：进程 fd 耗尽（EMFILE/ENFILE）时的"救急"描述符。
 * accept() 失败 EMFILE 时积压连接仍留在 backlog，ET 模式只在队列
 * 空→非空跳变时通知，backlog 不清空监听 fd 就永远等不到新事件
 * （新设备全部无声挂起）。临时关闭 reserve fd 换取一个可用 fd，
 * 把 backlog 全部 accept 出来并立即 close（明确拒绝），排空后
 * 再补开 reserve fd，恢复 ET 事件边沿。
 */
static int g_reserve_fd = -1;

int server_init(uint16_t port)
{
    int listen_fd;
    int opt;
    struct sockaddr_in addr;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        LOG_ERROR("server_init: socket failed: %s", strerror(errno));
        return -1;
    }

    /* 启用地址重用，使服务器能快速重启。 */
    opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   &opt, sizeof(opt)) == -1) {
        LOG_ERROR("server_init: setsockopt SO_REUSEADDR failed: %s",
              strerror(errno));
        close(listen_fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        LOG_ERROR("server_init: bind port %u failed: %s",
              (unsigned int)port, strerror(errno));
        close(listen_fd);
        return -1;
    }

    if (fd_set_nonblocking(listen_fd) == -1) {
        LOG_ERROR("server_init: fd_set_nonblocking failed: %s",
              strerror(errno));
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, SERVER_LISTEN_BACKLOG) == -1) {
        LOG_ERROR("server_init: listen failed: %s", strerror(errno));
        close(listen_fd);
        return -1;
    }

    /* 打开 reserve fd：失败仅告警——极少数环境无 /dev/null
     * 时退化为"EMFILE 即停止 accept"行为。先防重入泄漏旧描述符。 */
    if (g_reserve_fd >= 0) {
        close(g_reserve_fd);
        g_reserve_fd = -1;
    }
    g_reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (g_reserve_fd == -1) {
        LOG_ERROR("server_init: reserve fd open failed: %s",
                  strerror(errno));
    }

    return listen_fd;
}

void server_close(int listen_fd)
{
    if (listen_fd >= 0) {
        close(listen_fd);
    }

    if (g_reserve_fd >= 0) {
        close(g_reserve_fd);
        g_reserve_fd = -1;
    }
}

/*
 * EMFILE/ENFILE 时的 backlog 排空拒绝（见 g_reserve_fd 注释）。
 * 释放 reserve fd → 循环 accept 并立即 close（明确拒绝，客户端马上感知
 * 连接被关闭而非无声挂起）→ 排空后补开 reserve fd。
 */
static void server_drain_backlog_reject(int listen_fd)
{
    int fd;
    int drained = 0;

    close(g_reserve_fd);
    g_reserve_fd = -1;

    for (;;) {
        fd = accept(listen_fd, NULL, NULL);
        if (fd == -1) {
            if (errno == EINTR || errno == ECONNABORTED || errno == EPROTO) {
                /* 瞬时中断或对端已放弃，必须继续排空剩余 backlog。 */
                continue;
            }
            break; /* EAGAIN 等：backlog 已排空 */
        }
        close(fd); /* 无 fd 可分配：立即拒绝该连接。 */
        drained++;
    }

    if (drained > 0) {
        LOG_ERROR("server_accept: fd exhausted, rejected %d pending "
                  "connection(s) to unblock the listen backlog", drained);
    }

    g_reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (g_reserve_fd == -1) {
        LOG_ERROR("server_accept: reserve fd reopen failed: %s",
                  strerror(errno));
    }
}

int server_accept(int listen_fd)
{
    int client_fd;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len;

    if (listen_fd < 0) {
        errno = EBADF;
        return -1;
    }

    /* 若此前因暂时 fd 紧张导致 reserve fd 重开失败，趁机静默补开。 */
    if (g_reserve_fd < 0) {
        g_reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    }

    for (;;) {
        client_addr_len = sizeof(client_addr);
        client_fd = accept(listen_fd,
                           (struct sockaddr *)&client_addr,
                           &client_addr_len);
        if (client_fd == -1) {
            if (errno == EINTR) {
                /* 被信号中断，重试 accept 调用。 */
                continue;
            }

            if (errno == ECONNABORTED || errno == EPROTO) {
                /*
                 * 瞬时错误：该连接在 accept 前已被对端放弃，
                 * 内核已把它移出队列，队列后方可能还有待处理连接。
                 * 必须继续 accept 而不是跳出：ET 模式下若 backlog
                 * 未排空，监听 fd 不会再触发新事件，剩余连接会被饿死。
                 */
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 非阻塞模式下没有待处理的连接。 */
                return -1;
            }

            if (errno == EMFILE || errno == ENFILE) {
                /*
                 * fd 耗尽：排空并拒绝积压连接，防止监听 fd
                 * 在 ET 模式下被永久饿死。排空后恢复 errno，由调用方
                 * 结束本轮 accept 循环。
                 */
                int cause = errno;
                if (g_reserve_fd >= 0) {
                    server_drain_backlog_reject(listen_fd);
                }
                errno = cause;
                LOG_ERROR("server_accept: accept failed: %s",
                          strerror(errno));
                return -1;
            }

            LOG_ERROR("server_accept: accept failed: %s", strerror(errno));
            return -1;
        }

        break;
    }

    if (fd_set_nonblocking(client_fd) == -1) {
        LOG_ERROR("server_accept: fd_set_nonblocking failed: %s",
              strerror(errno));
        close(client_fd);
        return -1;
    }

    return client_fd;
}
