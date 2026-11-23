/*
 * RingBuffer 绕回行为单元测试。
 *
 * 覆盖：
 *   1. head 写满绕回到 0；
 *   2. 满则拒写不覆盖未消费数据；
 *   3. tail 读空绕回到 0；
 *   4. 空缓冲区拒读；
 *   5. 跨缓冲区数组末尾→开头边界写入与顺序读出的数据完整性。
 */
#include <stdio.h>
#include <string.h>

#include "utils/ringbuffer.h"

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

int main(void)
{
    ringbuffer_t rb;
    uint8_t write_buf[10];
    uint8_t read_buf[10];
    size_t i;
    size_t ret;

    /* 1. 初始化 */
    CHECK(rb_init(&rb) == 0);
    CHECK(rb_used(&rb) == 0);
    CHECK(rb_free(&rb) == RB_CAPACITY);

    /* 2. 写满缓冲区并验证 head 绕回 */
    for (i = 0; i < sizeof(write_buf); i++) {
        write_buf[i] = (uint8_t)i;
    }
    for (i = 0; i < 409; i++) {
        CHECK(rb_write(&rb, write_buf, 10) == 10);
    }
    CHECK(rb_used(&rb) == 4090);
    CHECK(rb.head == 4090);

    ret = rb_write(&rb, write_buf, 6);
    CHECK(ret == 6);
    CHECK(rb_used(&rb) == 4096);
    CHECK(rb.head == 0);

    /* 3. 满则拒写 */
    ret = rb_write(&rb, write_buf, 1);
    CHECK(ret == 0);
    CHECK(rb_used(&rb) == 4096);
    CHECK(rb.head == 0);

    /* 4. 读空缓冲区并验证 tail 绕回 */
    for (i = 0; i < 409; i++) {
        CHECK(rb_read(&rb, read_buf, 10) == 10);
    }
    CHECK(rb_used(&rb) == 6);
    CHECK(rb.tail == 4090);

    ret = rb_read(&rb, read_buf, 6);
    CHECK(ret == 6);
    CHECK(rb_used(&rb) == 0);
    CHECK(rb.tail == 0);

    /* 5. 空缓冲区读取 */
    ret = rb_read(&rb, read_buf, 1);
    CHECK(ret == 0);
    CHECK(rb_used(&rb) == 0);

    /* 6. 跨绕回边界数据完整性（head=4094, tail=1, free=3 -> 跨 buf[4094..4095,0]） */
    CHECK(rb_init(&rb) == 0);
    memset(write_buf, 0xAB, sizeof(write_buf));
    for (i = 0; i < 409; i++) {
        CHECK(rb_write(&rb, write_buf, 10) == 10);
    }
    {
        uint8_t four_bytes[4] = { 0xAB, 0xAB, 0xAB, 0xAB };
        CHECK(rb_write(&rb, four_bytes, 4) == 4);
    }
    CHECK(rb.head == 4094);
    CHECK(rb_used(&rb) == 4094);

    CHECK(rb_read(&rb, read_buf, 1) == 1);
    CHECK(rb.tail == 1);
    CHECK(rb_free(&rb) == 3);

    {
        uint8_t wrap_data[3] = { 0x11, 0x22, 0x33 };
        CHECK(rb_write(&rb, wrap_data, 3) == 3);
    }
    CHECK(rb.head == 1);
    CHECK(rb_used(&rb) == 4096);

    for (i = 0; i < 409; i++) {
        CHECK(rb_read(&rb, read_buf, 10) == 10);
    }
    CHECK(rb_used(&rb) == 6);

    CHECK(rb_read(&rb, read_buf, 3) == 3);
    CHECK(read_buf[0] == 0xAB);
    CHECK(rb_used(&rb) == 3);

    CHECK(rb_read(&rb, read_buf, 3) == 3);
    CHECK(read_buf[0] == 0x11);
    CHECK(read_buf[1] == 0x22);
    CHECK(read_buf[2] == 0x33);

    rb_destroy(&rb);

    printf("unit_ringbuffer_wrap: %d checks, %d failures\n",
           g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
