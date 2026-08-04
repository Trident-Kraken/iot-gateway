/*
 * fd_util 单元测试。
 *
 * 覆盖：pipe fd 设置为非阻塞（F_GETFL 含 O_NONBLOCK）、非法 fd 返回 -1。
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "utils/fd_util.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

static void test_pipe_nonblocking(void)
{
    int fds[2];
    int flags;

    CHECK(pipe(fds) == 0);

    CHECK(fd_set_nonblocking(fds[0]) == 0);
    flags = fcntl(fds[0], F_GETFL, 0);
    CHECK(flags != -1);
    CHECK((flags & O_NONBLOCK) != 0);

    CHECK(fd_set_nonblocking(fds[1]) == 0);
    flags = fcntl(fds[1], F_GETFL, 0);
    CHECK(flags != -1);
    CHECK((flags & O_NONBLOCK) != 0);

    close(fds[0]);
    close(fds[1]);
}

static void test_invalid_fd(void)
{
    errno = 0;
    CHECK(fd_set_nonblocking(-1) == -1);
    CHECK(errno != 0);
}

int main(void)
{
    test_pipe_nonblocking();
    test_invalid_fd();

    printf("unit_fd_util: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
