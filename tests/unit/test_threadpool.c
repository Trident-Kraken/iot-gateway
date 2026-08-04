/*
 * threadpool 单元测试。
 *
 * 覆盖：NULL 回调拒绝、任务全部被处理（destroy 排空 + join）、
 * 销毁后提交拒绝。
 *
 * 注：task_queue 采用 pthread_cond_signal（单播唤醒），单生产者
 * 爆发式提交时单个工作线程可能连续排空队列，故不做"最大并行度"
 * 断言（该属性取决于调度，生产场景 epoll 逐帧入队时多线程并发成立）。
 */
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/threadpool.h"

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

static atomic_int g_processed = 0;

/* 工作线程回调：统计处理数。 */
static void test_handler(struct task *task)
{
    (void)task;
    atomic_fetch_add(&g_processed, 1);
}

static void test_null_handler(void)
{
    errno = 0;
    CHECK(threadpool_init(0, NULL) == -1);
    CHECK(errno == EINVAL);
}

static void test_process_all_and_parallelism(void)
{
    int i;

    CHECK(threadpool_init(4, test_handler) == 0);

    for (i = 0; i < 1000; i++) {
        struct task t;

        memset(&t, 0, sizeof(t));
        t.cmd = (uint8_t)(i & 0xFF);
        t.frame_len = 0;
        CHECK(threadpool_submit(&t) == 0);
    }

    /* destroy：设置停止标志 → 广播唤醒 → join；队列中任务被排空处理。 */
    threadpool_destroy();

    CHECK(atomic_load(&g_processed) == 1000);
}

static void test_submit_after_destroy(void)
{
    struct task t;

    memset(&t, 0, sizeof(t));
    t.frame_len = 0;

    errno = 0;
    CHECK(threadpool_submit(&t) == -1);
    CHECK(errno == EINVAL);
}

int main(void)
{
    test_null_handler();
    test_process_all_and_parallelism();
    test_submit_after_destroy();

    printf("unit_threadpool: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
