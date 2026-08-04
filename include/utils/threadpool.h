#ifndef IOT_GATEWAY_THREADPOOL_H
#define IOT_GATEWAY_THREADPOOL_H

#include "utils/task_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 当调用方传入 0 或负数时使用的默认工作线程数。 */
#define THREADPOOL_DEFAULT_THREADS 4

/**
 * @brief 初始化全局线程池。
 *
 * 创建配置数量的工作线程。每个线程等待全局任务队列，弹出任务并处理。
 * 本函数内部也会初始化全局任务队列。
 *
 * @param num_threads 工作线程数量。<= 0 的值选择默认值。
 *
 * @return 成功返回 0，失败返回 -1。
 */
int threadpool_init(int num_threads);

/**
 * @brief 向全局线程池提交一个任务。
 *
 * 任务被复制到全局任务队列中；调用方在函数返回后可以安全地重用 @p task 对象。
 *
 * @param task 待提交的任务。
 *
 * @return 成功返回 0，失败返回 -1（例如线程池未初始化或正在关闭）。
 */
int threadpool_submit(const struct task *task);

/**
 * @brief 优雅关闭全局线程池。
 *
 * 设置全局运行标志为 0，广播任务队列条件变量以唤醒所有阻塞的工作线程，
 * 然后 join 每个线程。所有工作线程退出后，销毁任务队列。
 */
void threadpool_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* IOT_GATEWAY_THREADPOOL_H */
