#ifndef IOT_GATEWAY_RINGBUFFER_H
#define IOT_GATEWAY_RINGBUFFER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 每个连接独立 RingBuffer 的固定容量。 */
#define RB_CAPACITY 4096u

/**
 * @brief 简单的循环缓冲区。
 *
 * 设计前提：每个 TCP 连接拥有一个独立的 RingBuffer。epoll I/O 线程是唯一
 * 的写入者（单生产者），packet_parser 是唯一的读取者（单消费者）。因为两者
 * 在本项目中运行于同一个 epoll 线程，不需要互斥锁。如果将来设计发生变化
 *（例如解析器迁移到工作线程），需要添加锁保护。
 */
typedef struct ringbuffer {
    uint8_t buf[RB_CAPACITY]; /* 原始字节存储 */
    size_t  head;             /* 下一个写入位置 */
    size_t  tail;             /* 下一个读取位置 */
    size_t  len;              /* 当前存储的有效字节数 */
} ringbuffer_t;

/**
 * @brief 初始化/重置 RingBuffer。
 *
 * @param rb 待初始化的 RingBuffer。
 *
 * @return 成功返回 0，失败返回 -1 并设置 errno。
 */
int rb_init(ringbuffer_t *rb);

/**
 * @brief 销毁/释放 RingBuffer。
 *
 * 对于纯缓冲区实现，重置 RingBuffer 状态。如果 ringbuffer_t 对象本身是
 * 动态分配的，调用方仍需自行释放。
 *
 * @param rb 待销毁的 RingBuffer。
 */
void rb_destroy(ringbuffer_t *rb);

/**
 * @brief 向 RingBuffer 写入数据。
 *
 * 如果传入数据无法完全放入，旧数据会被覆盖（循环覆盖策略）。调用方必须
 * 确保解析器足够快地排空数据，以避免丢帧。
 *
 * @param rb   RingBuffer 句柄。
 * @param data 待写入的数据。
 * @param len  待写入的字节数。
 *
 * @return 实际写入的字节数（对于超大输入，上限为 RB_CAPACITY）。
 */
size_t rb_write(ringbuffer_t *rb, const void *data, size_t len);

/**
 * @brief 从 RingBuffer 读取数据并推进读取指针。
 *
 * @param rb  RingBuffer 句柄。
 * @param buf 目标缓冲区。
 * @param len 最大读取字节数。
 *
 * @return 实际读取的字节数。
 */
size_t rb_read(ringbuffer_t *rb, void *buf, size_t len);

/**
 * @brief 查看数据但不推进读取指针。
 *
 * @param rb  RingBuffer 句柄。
 * @param buf 目标缓冲区。
 * @param len 最大查看字节数。
 *
 * @return 实际查看的字节数。
 */
size_t rb_peek(const ringbuffer_t *rb, void *buf, size_t len);

/**
 * @brief 返回当前存储的有效字节数。
 *
 * @param rb RingBuffer 句柄。
 *
 * @return 已使用的字节数。
 */
size_t rb_used(const ringbuffer_t *rb);

/**
 * @brief 返回剩余空闲字节数。
 *
 * @param rb RingBuffer 句柄。
 *
 * @return 空闲字节数（可写入的剩余空间）。
 */
size_t rb_free(const ringbuffer_t *rb);

#ifdef __cplusplus
}
#endif

#endif /* IOT_GATEWAY_RINGBUFFER_H */
