/*
 * task_queue 单元测试。
 *
 * 覆盖：FIFO 顺序、frame_len 校验、对象池耗尽（有界水位线）、
 * release 归还后节点复用、shutdown 唤醒阻塞 pop、关闭后 push 拒绝。
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/task_queue.h"

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

static struct task make_task(uint8_t tag)
{
    struct task t;

    memset(&t, 0, sizeof(t));
    t.cmd = tag;
    t.frame[0] = tag;   /* 用于 FIFO 顺序校验。 */
    t.frame_len = 1;
    return t;
}

static void test_fifo_order(void)
{
    struct task *t;
    int i;

    CHECK(task_queue_init() == 0);

    for (i = 0; i < 3; i++) {
        struct task tk = make_task((uint8_t)(0x10 + i));
        CHECK(task_queue_push(&tk) == 0);
    }

    for (i = 0; i < 3; i++) {
        t = task_queue_pop();
        CHECK(t != NULL);
        CHECK(t->frame[0] == (uint8_t)(0x10 + i));  /* 先进先出。 */
        task_queue_release(t);
    }
}

static void test_invalid_frame_len(void)
{
    struct task tk = make_task(0x20);

    tk.frame_len = sizeof(tk.frame) + 1;
    errno = 0;
    CHECK(task_queue_push(&tk) == -1);
    CHECK(errno == EINVAL);
}

static void test_pool_exhaustion_and_reuse(void)
{
    int i;
    struct task *t;

    /* 填满 2048 节点对象池。 */
    for (i = 0; i < 2048; i++) {
        struct task tk = make_task((uint8_t)(i & 0xFF));
        CHECK(task_queue_push(&tk) == 0);
    }

    /* 池满 → 拒绝入队（有界水位线）。 */
    {
        struct task tk = make_task(0xFF);
        errno = 0;
        CHECK(task_queue_push(&tk) == -1);
        CHECK(errno == ENOMEM);
    }

    /* 全部弹出并归还后，池恢复可用。 */
    for (i = 0; i < 2048; i++) {
        t = task_queue_pop();
        CHECK(t != NULL);
        task_queue_release(t);
    }

    {
        struct task tk = make_task(0x77);
        CHECK(task_queue_push(&tk) == 0);
        t = task_queue_pop();
        CHECK(t != NULL && t->frame[0] == 0x77);
        task_queue_release(t);
    }
}

static void test_shutdown(void)
{
    struct task tk = make_task(0x30);

    task_queue_shutdown();

    /* 队列已空且关闭 → pop 返回 NULL（阻塞被唤醒）。 */
    CHECK(task_queue_pop() == NULL);

    /* 关闭后 push 拒绝。 */
    errno = 0;
    CHECK(task_queue_push(&tk) == -1);
    CHECK(errno == ESHUTDOWN);

    task_queue_destroy();
}

int main(void)
{
    test_fifo_order();
    test_invalid_frame_len();
    test_pool_exhaustion_and_reuse();
    test_shutdown();

    printf("unit_task_queue: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
