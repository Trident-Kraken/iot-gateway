#include "utils/task_queue.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 链式节点，存放一个任务。 */
struct task_node {
    struct task       task;
    struct task_node *next;
};

/* 全局单例队列状态。 */
static pthread_mutex_t g_mutex     = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond      = PTHREAD_COND_INITIALIZER;
static struct task_node *g_head    = NULL;
static struct task_node *g_tail    = NULL;
static int              g_shutdown = 0;
static int              g_initialized = 0;

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
    g_shutdown = 0;
    g_initialized = 1;

    return 0;
}

void task_queue_destroy(void)
{
    struct task_node *node;
    struct task_node *next;

    if (!g_initialized) {
        return;
    }

    node = g_head;
    while (node != NULL) {
        next = node->next;
        free(node);
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

    node = malloc(sizeof(*node));
    if (node == NULL) {
        return -1;
    }

    node->task = *task;
    node->next = NULL;

    /*
     * 锁粒度：仅对链表操作和条件变量信号持有互斥锁。调用方的任务对象
     * 已被复制，因此锁下不执行任何长时间工作。
     */
    pthread_mutex_lock(&g_mutex);

    if (g_shutdown) {
        pthread_mutex_unlock(&g_mutex);
        free(node);
        errno = ESHUTDOWN;
        return -1;
    }

    if (g_tail == NULL) {
        g_head = node;
        g_tail = node;
    } else {
        g_tail->next = node;
        g_tail = node;
    }

    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mutex);

    return 0;
}

struct task *task_queue_pop(void)
{
    struct task_node *node;
    struct task *task;

    if (!g_initialized) {
        errno = EINVAL;
        return NULL;
    }

    pthread_mutex_lock(&g_mutex);

    /*
     * 为什么用 while 循环而不是 if？
     *
     * POSIX 允许 pthread_cond_wait() 因显式 signal 或 broadcast 以外的
     * 原因返回（虚假唤醒）。它也可能因为多个线程被唤醒，而另一个线程在
     * 本线程重新获取互斥锁之前消费了唯一可用的任务。
     *
     * 使用 'if' 会让线程在队列可能仍然为空时继续执行，导致 NULL 指针解
     * 引用。使用 'while' 在每次唤醒后在互斥锁下重新检查条件。
     */
    while (g_head == NULL && !g_shutdown) {
        pthread_cond_wait(&g_cond, &g_mutex);
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

    pthread_mutex_unlock(&g_mutex);

    task = malloc(sizeof(*task));
    if (task == NULL) {
        free(node);
        return NULL;
    }

    *task = node->task;
    free(node);

    return task;
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
