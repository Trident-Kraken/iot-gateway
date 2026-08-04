#include "gateway/server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define SERVER_LISTEN_BACKLOG 128

/**
 * @brief 将文件描述符设置为非阻塞模式。
 *
 * @param fd 待修改的文件描述符。
 *
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

int server_init(uint16_t port)
{
    int listen_fd;
    int opt;
    struct sockaddr_in addr;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        fprintf(stderr, "server_init: socket failed: %s\n", strerror(errno));
        return -1;
    }

    /* 启用地址重用，使服务器能快速重启。 */
    opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   &opt, sizeof(opt)) == -1) {
        fprintf(stderr, "server_init: setsockopt SO_REUSEADDR failed: %s\n",
                strerror(errno));
        close(listen_fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        fprintf(stderr, "server_init: bind port %u failed: %s\n",
                (unsigned int)port, strerror(errno));
        close(listen_fd);
        return -1;
    }

    if (set_nonblocking(listen_fd) == -1) {
        fprintf(stderr, "server_init: set_nonblocking failed: %s\n",
                strerror(errno));
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, SERVER_LISTEN_BACKLOG) == -1) {
        fprintf(stderr, "server_init: listen failed: %s\n", strerror(errno));
        close(listen_fd);
        return -1;
    }

    return listen_fd;
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

    client_addr_len = sizeof(client_addr);

    for (;;) {
        client_fd = accept(listen_fd,
                           (struct sockaddr *)&client_addr,
                           &client_addr_len);
        if (client_fd == -1) {
            if (errno == EINTR) {
                /* 被信号中断，重试 accept 调用。 */
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 非阻塞模式下没有待处理的连接。 */
                return -1;
            }

            fprintf(stderr, "server_accept: accept failed: %s\n",
                    strerror(errno));
            return -1;
        }

        break;
    }

    if (set_nonblocking(client_fd) == -1) {
        fprintf(stderr, "server_accept: set_nonblocking failed: %s\n",
                strerror(errno));
        close(client_fd);
        return -1;
    }

    return client_fd;
}
