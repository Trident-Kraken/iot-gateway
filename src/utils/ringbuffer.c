#include "utils/ringbuffer.h"

#include <errno.h>
#include <string.h>

int rb_init(ringbuffer_t *rb)
{
    if (rb == NULL) {
        errno = EINVAL;
        return -1;
    }

    rb->head = 0;
    rb->tail = 0;
    rb->len = 0;

    return 0;
}

void rb_destroy(ringbuffer_t *rb)
{
    if (rb == NULL) {
        return;
    }

    rb->head = 0;
    rb->tail = 0;
    rb->len = 0;
}

size_t rb_write(ringbuffer_t *rb, const void *data, size_t len)
{
    const uint8_t *src;
    size_t i;

    if (rb == NULL || data == NULL || len == 0) {
        return 0;
    }

    /*
     * 满则拒绝：只写入当前可用空间，绝不覆盖尚未被解析器消费的旧数据。
     * 空间不足时返回实际写入字节数（< len），由上层（packet_parser）决定
     * 重置状态机并关闭连接，避免静默丢帧（AGENTS.md §5.3）。
     */
    if (len > rb_free(rb)) {
        len = rb_free(rb);
    }

    if (len == 0) {
        return 0;
    }

    src = data;

    for (i = 0; i < len; i++) {
        rb->buf[rb->head] = src[i];
        rb->head = (rb->head + 1) % RB_CAPACITY;
    }

    rb->len += len;

    return len;
}

size_t rb_read(ringbuffer_t *rb, void *buf, size_t len)
{
    uint8_t *dst;
    size_t to_read;
    size_t i;

    if (rb == NULL || buf == NULL) {
        errno = EINVAL;
        return 0;
    }

    to_read = len < rb->len ? len : rb->len;

    dst = buf;
    for (i = 0; i < to_read; i++) {
        dst[i] = rb->buf[rb->tail];
        rb->tail = (rb->tail + 1) % RB_CAPACITY;
    }

    rb->len -= to_read;

    return to_read;
}

size_t rb_peek(const ringbuffer_t *rb, void *buf, size_t len)
{
    uint8_t *dst;
    size_t to_peek;
    size_t i;
    size_t pos;

    if (rb == NULL || buf == NULL) {
        errno = EINVAL;
        return 0;
    }

    to_peek = len < rb->len ? len : rb->len;

    dst = buf;
    pos = rb->tail;
    for (i = 0; i < to_peek; i++) {
        dst[i] = rb->buf[pos];
        pos = (pos + 1) % RB_CAPACITY;
    }

    return to_peek;
}

size_t rb_used(const ringbuffer_t *rb)
{
    if (rb == NULL) {
        errno = EINVAL;
        return 0;
    }

    return rb->len;
}

size_t rb_free(const ringbuffer_t *rb)
{
    if (rb == NULL) {
        errno = EINVAL;
        return 0;
    }

    return RB_CAPACITY - rb->len;
}
