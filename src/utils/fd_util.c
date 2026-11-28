#define _POSIX_C_SOURCE 200809L /* FD_CLOEXEC */

#include "utils/fd_util.h"

#include <errno.h>
#include <fcntl.h>

int fd_set_nonblocking(int fd)
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

int fd_set_cloexec(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFD, 0);
    if (flags == -1) {
        return -1;
    }

    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
        return -1;
    }

    return 0;
}
