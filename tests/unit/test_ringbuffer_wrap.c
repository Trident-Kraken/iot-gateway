/**
 * @brief RingBuffer 绕回行为验证程序。
 *
 * 验证要点：
 *   1. 写入到数组末尾后，继续写入会绕回到开头（head 绕回）。
 *   2. 读取到数组末尾后，继续读取会绕回到开头（tail 绕回）。
 *   3. len 始终等于实际有效数据量（绕回时也正确）。
 *   4. 满则拒绝：缓冲区满时不再写入。
 *
 * 编译：gcc -I include tests/unit/test_ringbuffer_wrap.c src/utils/ringbuffer.c -o build/test_rb_wrap
 * 运行：./build/test_rb_wrap
 */
#include <stdio.h>
#include <string.h>
#include "utils/ringbuffer.h"

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT_EQ(desc, actual, expected) do { \
    if ((actual) == (expected)) { \
        printf("  [PASS] %s: %zu\n", desc, (size_t)(actual)); \
        g_pass++; \
    } else { \
        printf("  [FAIL] %s: expected %zu, got %zu\n", desc, (size_t)(expected), (size_t)(actual)); \
        g_fail++; \
    } \
} while (0)

int main(void)
{
    ringbuffer_t rb;
    uint8_t write_buf[10];
    uint8_t read_buf[10];
    size_t i;
    size_t ret;

    printf("=== RingBuffer 绕回行为验证 ===\n\n");

    /* ---- 测试 1：初始化 ---- */
    printf("测试 1: 初始化\n");
    rb_init(&rb);
    ASSERT_EQ("rb_used() == 0", rb_used(&rb), 0);
    ASSERT_EQ("rb_free() == RB_CAPACITY", rb_free(&rb), RB_CAPACITY);

    /* ---- 测试 2：先把缓冲区写满，验证 head 绕回 ---- */
    printf("\n测试 2: 写满缓冲区（验证 head 绕回）\n");

    /* 先写 4090 字节，让 head 接近末尾 */
    for (i = 0; i < sizeof(write_buf); i++) write_buf[i] = (uint8_t)i;
    for (i = 0; i < 409; i++) {   /* 409 * 10 = 4090 字节 */
        ret = rb_write(&rb, write_buf, 10);
        if (ret != 10) { printf("  [FAIL] 写入 %zu 字节时返回 %zu\n", i * 10, ret); g_fail++; break; }
    }
    ASSERT_EQ("写入 4090 字节后 len == 4090", rb_used(&rb), 4090);
    ASSERT_EQ("head 应等于 4090", rb.head, 4090);   /* head 还没绕回 */

    /* 再写 6 字节，head 会从 4090 → 4096 → 0，发生绕回！ */
    ret = rb_write(&rb, write_buf, 6);
    ASSERT_EQ("再写 6 字节返回 6", ret, 6);
    ASSERT_EQ("len == 4096（满）", rb_used(&rb), 4096);
    ASSERT_EQ("head 绕回后等于 0", rb.head, 0);     /* head 绕回了！ */

    /* ---- 测试 3：满则拒绝 ---- */
    printf("\n测试 3: 满则拒绝\n");
    ret = rb_write(&rb, write_buf, 1);
    ASSERT_EQ("满后再写返回 0", ret, 0);
    ASSERT_EQ("len 仍为 4096", rb_used(&rb), 4096);
    ASSERT_EQ("head 仍为 0（没动）", rb.head, 0);

    /* ---- 测试 4：读空缓冲区，验证 tail 绕回 ---- */
    printf("\n测试 4: 读空缓冲区（验证 tail 绕回）\n");

    /* 先读 4090 字节，让 tail 接近末尾 */
    for (i = 0; i < 409; i++) {
        ret = rb_read(&rb, read_buf, 10);
        if (ret != 10) { printf("  [FAIL] 读取 %zu 字节时返回 %zu\n", i * 10, ret); g_fail++; break; }
    }
    ASSERT_EQ("读走 4090 字节后 len == 6", rb_used(&rb), 6);
    ASSERT_EQ("tail 应等于 4090", rb.tail, 4090);   /* tail 还没绕回 */

    /* 再读 6 字节，tail 会从 4090 → 4096 → 0，发生绕回！ */
    ret = rb_read(&rb, read_buf, 6);
    ASSERT_EQ("再读 6 字节返回 6", ret, 6);
    ASSERT_EQ("len == 0（空）", rb_used(&rb), 0);
    ASSERT_EQ("tail 绕回后等于 0", rb.tail, 0);     /* tail 绕回了！ */

    /* ---- 测试 5：空则读不到 ---- */
    printf("\n测试 5: 空缓冲区读取\n");
    ret = rb_read(&rb, read_buf, 1);
    ASSERT_EQ("空后再读返回 0", ret, 0);
    ASSERT_EQ("len 仍为 0", rb_used(&rb), 0);

    /* ---- 测试 6：数据跨绕回边界时内容正确 ---- */
    printf("\n测试 6: 绕回后数据完整性（数据跨数组末尾和开头）\n");
    rb_init(&rb);

    /* 构造场景：head=4094, tail=1, len=4093, free=3
       这样再写 3 字节就会跨边界：buf[4094] → buf[4095] → buf[0] */

    /* 第一步：写 4094 字节 → head=4094, tail=0, len=4094, free=2 */
    memset(write_buf, 0xAB, sizeof(write_buf));
    for (i = 0; i < 409; i++) rb_write(&rb, write_buf, 10); /* 4090 */
    uint8_t four_bytes[4] = {0xAB, 0xAB, 0xAB, 0xAB};
    rb_write(&rb, four_bytes, 4);  /* +4 = 4094 */
    ASSERT_EQ("head 应在 4094", rb.head, 4094);
    ASSERT_EQ("len == 4094", rb_used(&rb), 4094);

    /* 第二步：读 1 字节 → tail=1, len=4093, free=3（腾出 1 字节空间） */
    rb_read(&rb, read_buf, 1);
    ASSERT_EQ("tail 应在 1", rb.tail, 1);
    ASSERT_EQ("free == 3（刚好够写 3 字节）", rb_free(&rb), 3);

    /* 第三步：写 3 字节特殊数据 → 写入 buf[4094], buf[4095], buf[0]，跨绕回边界！ */
    uint8_t wrap_data[3] = {0x11, 0x22, 0x33};
    ret = rb_write(&rb, wrap_data, 3);
    ASSERT_EQ("跨边界写入 3 字节", ret, 3);
    ASSERT_EQ("head 绕回后等于 1  (4094+3)%4096=1", rb.head, 1);
    ASSERT_EQ("len == 4096（写满）", rb_used(&rb), 4096);

    /* 验证：读 4093 字节旧数据（都是 0xAB）后，最后 3 字节是 0x11,0x22,0x33
       数据物理分布：buf[1..4093]=0xAB, buf[4094]=0x11, buf[4095]=0x22, buf[0]=0x33
       读取顺序（从 tail=1 开始）：1,2,...,4093, 4094, 4095, 0 */
    for (i = 0; i < 409; i++) {
        ret = rb_read(&rb, read_buf, 10);
        if (ret != 10) { printf("  [FAIL] 批量读取失败\n"); g_fail++; break; }
    }
    /* 已读 4090 字节，剩 6 字节（4093 - 4090 = 3 个旧的 + 3 个跨边界的 = 6） */
    ASSERT_EQ("剩余 6 字节", rb_used(&rb), 6);
    ret = rb_read(&rb, read_buf, 3);  /* 读走 3 个旧的 0xAB */
    ASSERT_EQ("再读 3 字节旧数据返回 3", ret, 3);
    ASSERT_EQ("旧数据值为 0xAB", read_buf[0], 0xAB);
    ASSERT_EQ("剩最后 3 字节（跨边界的）", rb_used(&rb), 3);

    /* 最关键：最后 3 字节跨了数组末尾和开头，顺序仍然正确 */
    ret = rb_read(&rb, read_buf, 3);
    ASSERT_EQ("读取跨边界 3 字节返回 3", ret, 3);
    ASSERT_EQ("第 1 字节 == 0x11 (buf[4094])", read_buf[0], 0x11);
    ASSERT_EQ("第 2 字节 == 0x22 (buf[4095])", read_buf[1], 0x22);
    ASSERT_EQ("第 3 字节 == 0x33 (buf[0]，绕回开头！)", read_buf[2], 0x33);
    printf("    → 数据跨绕回边界（数组末尾→开头）后仍然完整有序！\n");

    /* ---- 总结 ---- */
    printf("\n========== 总结 ==========\n");
    printf("通过: %d, 失败: %d\n", g_pass, g_fail);
    printf("%s\n", g_fail == 0 ? "🎉 全部通过！绕回行为验证正确。" : "❌ 有失败项，请检查。");

    rb_destroy(&rb);
    return g_fail == 0 ? 0 : 1;
}
