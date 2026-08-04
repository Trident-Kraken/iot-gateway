#include "utils/threadpool.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 全局运行标志。
 *
 * 由 threadpool_init() 设为 1，由 threadpool_destroy() 设为 0。工作线程
 * 在 task_queue_pop() 返回 NULL 后读取此标志以决定是否退出。使用原子类型
 * 是因为该标志由多个工作线程读取，由 destroy 路径写入，而此时不持有任务
 * 队列锁。
 */
static atomic_int g_running = 0;

/* 单例线程池状态。 */
static struct {
    pthread_t    *threads;
    int           num_threads;
    task_handler_t task_handler; /* 任务处理回调（init 时注入）。 */
    int           initialized;
} g_pool;

/**
 * @brief 工作线程主循环。
 *
 * 每个工作线程永远循环，阻塞在 task_queue_pop() 上直到有任务到达或收到
 * 关闭信号。当弹出任务后，工作线程委托给注册的 task_handler
 *（threadpool_init 注入，如 business_handler 的 worker_process_task），
 * 由上层展开帧并通过函数指针表分发到相应的命令处理器。
 */
static void *worker_thread(void *arg)
{
    (void)arg;

    for (;;) {
        struct task *task;

        task = task_queue_pop();
        if (task == NULL) {
            /*
             * task_queue_pop() 只在队列为空且已请求关闭时返回 NULL。
             * 检查 g_running 以确认确实到了退出时间。
             */
            if (!atomic_load(&g_running)) {
                break;
            }

            /* 虚假唤醒：无任务也未关闭，继续循环。 */
            continue;
        }

        /*
         * 锁粒度：任务处理在任务队列互斥锁外执行。只有队列操作（push/pop）
         * 在锁下进行。这使临界区极小，并允许多个工作线程并行处理任务。
         */
        g_pool.task_handler(task);

        /* 任务节点属对象池，归还而非 free()（零分配热路径）。 */
        task_queue_release(task);
    }

    return NULL;
}

int threadpool_init(int num_threads, task_handler_t task_handler)
{
    int i;

    if (g_pool.initialized) {
        return 0;
    }

    if (task_handler == NULL) {
        errno = EINVAL;
        return -1;
    }

    g_pool.task_handler = task_handler;

    if (num_threads <= 0) {
        num_threads = THREADPOOL_DEFAULT_THREADS;
    }

    if (task_queue_init() != 0) {
        return -1;
    }

    g_pool.threads = calloc((size_t)num_threads, sizeof(pthread_t));
    if (g_pool.threads == NULL) {
        task_queue_destroy();
        return -1;
    }

    g_pool.num_threads = num_threads;

    /* 在创建线程前设置运行标志，使它们不会立即退出。 */
    atomic_store(&g_running, 1);

    for (i = 0; i < num_threads; i++) {
        if (pthread_create(&g_pool.threads[i], NULL, worker_thread, NULL) != 0) {
            /*
             * 创建失败。信号关闭，唤醒可能已启动的任何线程，join 它们，
             * 然后清理。
             */
            atomic_store(&g_running, 0);
            task_queue_shutdown();

            while (--i >= 0) {
                pthread_join(g_pool.threads[i], NULL);
            }

            task_queue_destroy();
            free(g_pool.threads);
            g_pool.threads = NULL;
            g_pool.num_threads = 0;
            return -1;
        }
    }

    g_pool.initialized = 1;
    return 0;
}

int threadpool_submit(const struct task *task)
{
    if (!g_pool.initialized || task == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (!atomic_load(&g_running)) {
        errno = ESHUTDOWN;
        return -1;
    }

    return task_queue_push(task);
}

void threadpool_destroy(void)
{
    int i;

    if (!g_pool.initialized) {
        return;
    }

    /* 第 1 步：告诉工作线程在队列排空后停止。 */
    atomic_store(&g_running, 0);

    /*
     * 第 2 步：唤醒每个阻塞在 task_queue_pop() 中的工作线程。
     * 唤醒后，每个工作线程将看到空队列和 g_running == 0，然后退出。
     */
    task_queue_shutdown();

    /* 第 3 步：等待所有工作线程结束。 */
    for (i = 0; i < g_pool.num_threads; i++) {
        if (g_pool.threads[i] != 0) {
            pthread_join(g_pool.threads[i], NULL);
        }
    }

    /* 第 4 步：释放队列和线程池资源。 */
    task_queue_destroy();
    free(g_pool.threads);

    g_pool.threads = NULL;
    g_pool.num_threads = 0;
    g_pool.initialized = 0;
}
