#include "utils/task_queue.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 任务节点对象池容量：也是任务队列的有界水位线（满则拒绝入队，防止无界增长）。 */
#define TASK_POOL_CAPACITY 2048

/* 链式节点，存放一个任务。 */
struct task_node {
    struct task       task;
    struct task_node *next;
};

/* 任务节点对象池：固定数组 + LIFO 空闲链（g_mutex 保护）。
 * epoll 线程 push / 工作线程 pop 均零动态分配，控制热路径延迟。 */
static struct task_node   g_task_pool[TASK_POOL_CAPACITY];
static struct task_node  *g_free_nodes = NULL;

/* 全局单例队列状态。 */
static pthread_mutex_t g_mutex     = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond      = PTHREAD_COND_INITIALIZER;
static struct task_node *g_head    = NULL;
static struct task_node *g_tail    = NULL;
static size_t            g_waiters = 0;
static size_t            g_depth   = 0;
static uint64_t          g_pushed  = 0;
static uint64_t          g_popped  = 0;
static uint64_t          g_dropped_full = 0;
static int              g_shutdown = 0;
static int              g_initialized = 0;

/* 构建空闲链（启动时单线程调用，无需加锁）。 */
static void task_pool_init(void)
{
    size_t i;

    /* 每次 init 均重建完整 2048 节点空闲链，防止 destroy 前若有未归还节点
     * 导致二次 init 时对象池容量缩水。 */
    g_free_nodes = NULL;
    for (i = 0; i < TASK_POOL_CAPACITY; i++) {
        g_task_pool[i].next = g_free_nodes;
        g_free_nodes = &g_task_pool[i];
    }
}

int task_queue_init(void)
{
    if (g_initialized) {
        return 0;
    }

    /*
     * 上面的静态初始化器对 PTHREAD_MUTEX_INITIALIZER 和
     * PTHREAD_COND_INITIALIZER 已足够，但保留显式的 init/destroy
     * 路径以保持对称性和在非 glibc 环境下的可移植性。
     */
    if (pthread_mutex_init(&g_mutex, NULL) != 0) {
        return -1;
    }

    if (pthread_cond_init(&g_cond, NULL) != 0) {
        pthread_mutex_destroy(&g_mutex);
        return -1;
    }

    g_head = NULL;
    g_tail = NULL;
    g_waiters = 0;
    g_depth = 0;
    g_pushed = 0;
    g_popped = 0;
    g_dropped_full = 0;
    g_shutdown = 0;
    g_initialized = 1;

    task_pool_init();

    return 0;
}

void task_queue_destroy(void)
{
    struct task_node *node;
    struct task_node *next;

    if (!g_initialized) {
        return;
    }

    /* 将队列中残留节点归还对象池（销毁时队列应已排空，防御性处理）。 */
    node = g_head;
    while (node != NULL) {
        next = node->next;
        node->next = g_free_nodes;
        g_free_nodes = node;
        node = next;
    }

    g_head = NULL;
    g_tail = NULL;
    g_shutdown = 1;

    pthread_mutex_destroy(&g_mutex);
    pthread_cond_destroy(&g_cond);

    g_initialized = 0;
}

int task_queue_push(const struct task *task)
{
    struct task_node *node;

    if (!g_initialized || task == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (task->frame_len > sizeof(task->frame)) {
        errno = EINVAL;
        return -1;
    }

    /*
     * 锁粒度：仅对链表操作、对象池取节点与条件变量信号持有互斥锁。调用方
     * 的任务对象已被复制，因此锁下不执行任何长时间工作。
     */
    pthread_mutex_lock(&g_mutex);

    if (g_shutdown) {
        pthread_mutex_unlock(&g_mutex);
        errno = ESHUTDOWN;
        return -1;
    }

    /* 从对象池取节点（零分配）。池耗尽 = 队列已满（有界水位线）。 */
    node = g_free_nodes;
    if (node == NULL) {
        g_dropped_full++;
        pthread_mutex_unlock(&g_mutex);
        errno = ENOMEM;
        return -1;
    }
    g_free_nodes = node->next;

    node->task.fd = task->fd;
    node->task.generation = task->generation;
    node->task.frame_len = task->frame_len;
    node->task.cmd = task->cmd;
    if (task->frame_len > 0) {
        memcpy(node->task.frame, task->frame, task->frame_len);
    }
    node->next = NULL;

    if (g_tail == NULL) {
        g_head = node;
        g_tail = node;
    } else {
        g_tail->next = node;
        g_tail = node;
    }

    g_depth++;
    g_pushed++;

    /* 仅当存在阻塞等待的工作线程时唤醒，避免高吞吐繁忙期无效 syscall。 */
    if (g_waiters > 0) {
        pthread_cond_signal(&g_cond);
    }
    pthread_mutex_unlock(&g_mutex);

    return 0;
}

struct task *task_queue_pop(void)
{
    struct task_node *node;

    if (!g_initialized) {
        errno = EINVAL;
        return NULL;
    }

    pthread_mutex_lock(&g_mutex);

    /*
     * 必须在 g_mutex 下用 while 循环重检条件：POSIX 允许 pthread_cond_wait()
     * 发生虚假唤醒，且多工作线程竞争下先醒来的线程可能已消费唯一任务，若用 if
     * 会在空队列上继续执行导致 NULL 解引用。
     */
    while (g_head == NULL && !g_shutdown) {
        g_waiters++;
        pthread_cond_wait(&g_cond, &g_mutex);
        g_waiters--;
    }

    if (g_head == NULL) {
        /* 请求关闭且队列已空。 */
        pthread_mutex_unlock(&g_mutex);
        return NULL;
    }

    node = g_head;
    g_head = node->next;
    if (g_head == NULL) {
        g_tail = NULL;
    }

    g_depth--;
    g_popped++;

    pthread_mutex_unlock(&g_mutex);

    /*
     * 返回对象池节点内嵌的 task（节点已出队，仍属对象池占用）。
     * 调用方处理完毕后必须调用 task_queue_release() 归还节点，零分配。
     */
    return &node->task;
}

void task_queue_release(struct task *task)
{
    struct task_node *node;
    uintptr_t addr;
    uintptr_t base;
    uintptr_t end;

    if (!g_initialized || task == NULL) {
        return;
    }

    /* container_of：task 是 struct task_node 的首成员。 */
    node = (struct task_node *)((char *)task -
                                offsetof(struct task_node, task));

    addr = (uintptr_t)node;
    base = (uintptr_t)&g_task_pool[0];
    end  = (uintptr_t)&g_task_pool[TASK_POOL_CAPACITY];
    if (addr < base || addr >= end ||
        (addr - base) % sizeof(struct task_node) != 0) {
        return;
    }

    pthread_mutex_lock(&g_mutex);
    node->next = g_free_nodes;
    g_free_nodes = node;
    pthread_mutex_unlock(&g_mutex);
}

void task_queue_get_stats(struct task_queue_stats *out)
{
    if (out == NULL) {
        return;
    }

    if (!g_initialized) {
        memset(out, 0, sizeof(*out));
        return;
    }

    pthread_mutex_lock(&g_mutex);
    out->depth        = g_depth;
    out->pushed       = g_pushed;
    out->popped       = g_popped;
    out->dropped_full = g_dropped_full;
    pthread_mutex_unlock(&g_mutex);
}

void task_queue_shutdown(void)
{
    if (!g_initialized) {
        return;
    }

    pthread_mutex_lock(&g_mutex);
    g_shutdown = 1;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_mutex);
}
