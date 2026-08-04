#include "gateway/heartbeat.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gateway/conn_manager.h"
#include "gateway/epoll_worker.h"
#include "utils/logger.h"

/* 单次扫描中关闭的最大过期连接数。*/
#define HEARTBEAT_BATCH_SIZE 256

/* 传递给 conn_manager_foreach() 的收集关闭上下文。*/
struct stale_collect_ctx {
    int   fds[HEARTBEAT_BATCH_SIZE];
    size_t count;
    time_t now;
    int    timeout_sec;
};

static pthread_t g_thread;
static atomic_int g_running = 0;
static int g_interval_sec = 5;
static int g_timeout_sec = 15;
static int g_started = 0;

/**
 * @brief conn_manager_foreach() 的回调：收集过期文件描述符。 *
 * 本回调在 conn_manager 锁内执行，因此只记录过期的fd，不执行任何 I/O
 * 或长时间工作。 */
static void collect_stale(struct conn_entry *entry, void *arg)
{
    struct stale_collect_ctx *ctx = arg;

    if (ctx->count >= HEARTBEAT_BATCH_SIZE) {
        return;
    }

    if ((time_t)ctx->timeout_sec > ctx->now) {
        /* 防御性：如果时钟跳跃会下溢。*/
        return;
    }

    if (ctx->now - entry->last_active > (time_t)ctx->timeout_sec) {
        ctx->fds[ctx->count++] = entry->fd;
    }
}

/**
 * @brief 心跳检测线程的主循环。 */
static void *heartbeat_thread(void *arg)
{
    (void)arg;

    while (atomic_load(&g_running)) {
        struct stale_collect_ctx ctx;
        size_t i;

        sleep((unsigned int)g_interval_sec);

        if (!atomic_load(&g_running)) {
            break;
        }

        ctx.count = 0;
        ctx.now = time(NULL);
        ctx.timeout_sec = g_timeout_sec;

        /*
         * 锁粒度：仅在链表遍历和fd 收集期间持有 conn_manager 锁。         * 实际的close() 调用在锁外执行，以避免释放内核资源时阻塞
         * epoll 线程。         */
        conn_manager_foreach(collect_stale, &ctx);

        for (i = 0; i < ctx.count; i++) {
            int fd = ctx.fds[i];

            LOG_INFO("heartbeat: closing inactive fd=%d (timeout %ds)",
                     fd, g_timeout_sec);

            /*
             * 关闭 socket。这也会将其从内核的 epoll 集中移除。             * conn_manager 条目被移除后，epoll 工作线程将不会再次             * 报告它。             */
            /* Delegate to the epoll thread via self-pipe: it will perform
             * full cleanup (parser unregister, conn_manager remove,
             * connection struct free, close) without racing with I/O. */
            epoll_worker_request_close(fd);
        }
    }

    return NULL;
}

int heartbeat_init(int interval_sec, int timeout_sec)
{
    if (interval_sec <= 0 || timeout_sec <= 0) {
        errno = EINVAL;
        return -1;
    }

    g_interval_sec = interval_sec;
    g_timeout_sec = timeout_sec;

    if (conn_manager_init() != 0) {
        return -1;
    }

    return 0;
}

int heartbeat_start(void)
{
    if (g_started) {
        return 0;
    }

    atomic_store(&g_running, 1);

    if (pthread_create(&g_thread, NULL, heartbeat_thread, NULL) != 0) {
        atomic_store(&g_running, 0);
        return -1;
    }

    g_started = 1;
    return 0;
}

void heartbeat_stop(void)
{
    if (!g_started) {
        return;
    }

    atomic_store(&g_running, 0);
    pthread_join(g_thread, NULL);
    g_started = 0;
}
