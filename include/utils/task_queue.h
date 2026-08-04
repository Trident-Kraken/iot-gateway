#ifndef IOT_GATEWAY_TASK_QUEUE_H
#define IOT_GATEWAY_TASK_QUEUE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief I/O 线程与工作线程之间传递的任务单元。 *
 * frame 字段最多携带256 字节的原始帧数据（取决于调用方，可以是Payload
 * 或完整的编码帧）。cmd 是从帧头中提取的协议命令字节。 */
struct task {
    int      fd;        /* 来源连接的文件描述符。*/
    uint8_t  frame[PROTOCOL_MAX_FRAME_SIZE];/* 待处理的原始帧字节。*/
    size_t   frame_len; /* frame[] 中的有效字节数。*/
    uint8_t  cmd;       /* 协议命令码。*/
};

/**
 * @brief 初始化全局任务队列。 *
 * 在任何push/pop 操作之前必须调用一次。本函数设计为非线程安全的；
 * 调用方应在程序启动期间从单一线程初始化队列。 *
 * @return 成功返回 0，失败返回-1。 */
int task_queue_init(void);

/**
 * @brief 销毁全局任务队列并释放所有排队的任务。 *
 * 仅在所有生产者和消费者停止后才可调用。 */
void task_queue_destroy(void);

/**
 * @brief 向全局队列推送一个任务。 *
 * 任务会被复制到堆分配的节点中，因此调用方在函数返回后可以安全地重用 * @p task 对象。 *
 * @param task 待入队的任务。 *
 * @return 成功返回 0，失败返回-1。 */
int task_queue_push(const struct task *task);

/**
 * @brief 从全局队列弹出一个任务，必要时阻塞。 *
 * 如果队列为空，调用线程阻塞在条件变量上，直到有任务被推送或队列被关闭。 *
 * @return 成功返回指向已分配任务的指针，队列已关闭且为空时返回 NULL。 *         调用方负责使用free() 释放返回的指针。 */
struct task *task_queue_pop(void);

/**
 * @brief 关闭队列并唤醒所有阻塞的 pop() 调用者。 *
 * 关闭后，push() 拒绝新任务，pop() 在队列清空后返回 NULL。 */
void task_queue_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* IOT_GATEWAY_TASK_QUEUE_H */
