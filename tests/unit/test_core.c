/*
 * iot-gateway 核心模块单元测试（无外部框架）。
 *
 * 直接编译被测模块源码（protocol/packet_parser/ringbuffer/logger），
 * 不链接 iot-gateway 主程序。覆盖：
 *   - CRC16-CCITT-FALSE 已知向量
 *   - packet_encode / packet_checksum_verify / packet_decode（含超大 Payload 防御）
 *   - RingBuffer 读写/peek/环绕
 *   - packet_parser 状态机：拆包（分片送达）、粘包、坏帧恢复、超长长度防御
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol/packet_parser.h"
#include "protocol/protocol.h"
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

/* ------------------------------------------------------------------ */
/* on_frame_complete：packet_parser 的 extern 回调，由本测试实现。      */
/* ------------------------------------------------------------------ */
static int g_frame_count = 0;
static uint8_t g_last_frame[PROTOCOL_MAX_FRAME_SIZE];
static size_t g_last_len = 0;

void on_frame_complete(int fd, const uint8_t *frame, size_t len)
{
    (void)fd;
    g_frame_count++;
    g_last_len = len;
    if (len <= sizeof(g_last_frame)) {
        memcpy(g_last_frame, frame, len);
    }
}

/* ------------------------------------------------------------------ */
/* 辅助：用 packet_encode 构造一帧。                                    */
/* ------------------------------------------------------------------ */
static size_t build_frame(uint8_t cmd, const uint8_t *payload,
                          size_t payload_len, uint8_t *out, size_t out_len)
{
    struct packet *pkt = malloc(sizeof(*pkt) + payload_len);
    size_t total;

    if (pkt == NULL) {
        return 0;
    }

    pkt->cmd = cmd;
    pkt->payload_len = (uint16_t)payload_len;
    if (payload_len > 0) {
        memcpy(pkt->payload, payload, payload_len);
    }

    if (packet_encode(pkt, out, out_len) != 0) {
        free(pkt);
        return 0;
    }

    total = PACKET_MIN_SIZE + payload_len;
    free(pkt);
    return total;
}

/* ------------------------------------------------------------------ */
/* 测试用例                                                            */
/* ------------------------------------------------------------------ */
static void test_crc16(void)
{
    CHECK(crc16_compute((const uint8_t *)"123456789", 9) == 0x29B1);
}

static void test_packet_encode_verify(void)
{
    uint8_t frame[PROTOCOL_MAX_FRAME_SIZE];
    const uint8_t payload[] = "hello";
    size_t total;
    uint8_t small[4];

    total = build_frame(CMD_SENSOR_DATA, payload, 5, frame, sizeof(frame));
    CHECK(total == PACKET_MIN_SIZE + 5);

    /* 帧头固定。 */
    CHECK(frame[0] == PACKET_HEADER_0 && frame[1] == PACKET_HEADER_1);

    /* CRC 校验通过。 */
    CHECK(packet_checksum_verify(frame, total) == 1);

    /* 篡改 Payload 后 CRC 校验失败。 */
    frame[5] ^= 0xFF;
    CHECK(packet_checksum_verify(frame, total) == 0);
    frame[5] ^= 0xFF;

    /* CRC 空指针防御 -> EINVAL。 */
    errno = 0;
    CHECK(packet_checksum_verify(NULL, 10) == -1);
    CHECK(errno == EINVAL);

    /* 输出缓冲区过小 -> ENOSPC。 */
    {
        struct packet *pkt = malloc(sizeof(*pkt) + 5);
        CHECK(pkt != NULL);
        pkt->cmd = CMD_SENSOR_DATA;
        pkt->payload_len = 5;
        memcpy(pkt->payload, "hello", 5);
        errno = 0;
        CHECK(packet_encode(pkt, small, sizeof(small)) == -1);
        CHECK(errno == ENOSPC);
        free(pkt);
    }

    /* payload_len 超出上限（1025 > 1024）-> EMSGSIZE。 */
    {
        struct packet *pkt = malloc(sizeof(*pkt));
        CHECK(pkt != NULL);
        pkt->cmd = CMD_SENSOR_DATA;
        pkt->payload_len = PROTOCOL_MAX_PAYLOAD_LEN + 1;
        errno = 0;
        CHECK(packet_encode(pkt, frame, sizeof(frame)) == -1);
        CHECK(errno == EMSGSIZE);
        free(pkt);
    }

    /* packet_encode_raw：空 payload 与常规 payload 零堆分配编码。 */
    {
        uint8_t raw_frame[PROTOCOL_MAX_FRAME_SIZE];
        struct packet *decoded = NULL;

        CHECK(packet_encode_raw(CMD_HEARTBEAT, NULL, 0,
                                raw_frame, sizeof(raw_frame)) == 0);
        CHECK(packet_checksum_verify(raw_frame, PACKET_MIN_SIZE) == 1);

        CHECK(packet_encode_raw(CMD_CONFIG, (const uint8_t *)"abc", 3,
                                raw_frame, sizeof(raw_frame)) == 0);
        CHECK(packet_decode(raw_frame, PACKET_MIN_SIZE + 3, &decoded) ==
              (ssize_t)(PACKET_MIN_SIZE + 3));
        CHECK(decoded != NULL && decoded->cmd == CMD_CONFIG);
        CHECK(decoded->payload_len == 3 &&
              memcmp(decoded->payload, "abc", 3) == 0);
        free(decoded);
    }
}

static void test_packet_decode(void)
{
    uint8_t frame[PROTOCOL_MAX_FRAME_SIZE];
    struct packet *out = NULL;
    size_t total;
    ssize_t consumed;

    total = build_frame(CMD_SENSOR_DATA, (const uint8_t *)"hello", 5,
                        frame, sizeof(frame));
    CHECK(total > 0);

    /* 正常解码。 */
    consumed = packet_decode(frame, total, &out);
    CHECK(consumed == (ssize_t)total);
    CHECK(out != NULL && out->cmd == CMD_SENSOR_DATA);
    CHECK(out->payload_len == 5 && memcmp(out->payload, "hello", 5) == 0);
    free(out);
    out = NULL;

    /* 非法帧头 -> EBADMSG。 */
    {
        uint8_t bad[] = { 0xBB, 0x55, 0x00, 0x02, 0x01, 0x7B, 0x7D, 0, 0 };
        errno = 0;
        CHECK(packet_decode(bad, sizeof(bad), &out) == -1);
        CHECK(errno == EBADMSG && out == NULL);
    }

    /* 超大 Payload（0x0800 = 2048 > 1024）-> EBADMSG，不分配。 */
    {
        uint8_t huge[] = { 0xAA, 0x55, 0x08, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0 };
        errno = 0;
        CHECK(packet_decode(huge, sizeof(huge), &out) == -1);
        CHECK(errno == EBADMSG && out == NULL);
    }

    /* 帧不完整 -> 返回 0。 */
    {
        uint8_t partial[] = { 0xAA, 0x55, 0x00, 0x02 };
        CHECK(packet_decode(partial, sizeof(partial), &out) == 0);
        CHECK(out == NULL);
    }
}

static void test_ringbuffer(void)
{
    ringbuffer_t rb;
    uint8_t w1[3000];
    uint8_t w2[2000];
    uint8_t rd[RB_CAPACITY];
    uint8_t big[RB_CAPACITY];
    uint8_t peek_buf[16];
    size_t i;

    CHECK(rb_init(&rb) == 0);

    /* 基本读写 + peek 不消费。 */
    for (i = 0; i < sizeof(w1); i++) {
        w1[i] = (uint8_t)(i & 0xFF);
    }
    CHECK(rb_write(&rb, w1, 3000) == 3000);
    CHECK(rb_used(&rb) == 3000);
    CHECK(rb_peek(&rb, peek_buf, 16) == 16);
    CHECK(memcmp(peek_buf, w1, 16) == 0);
    CHECK(rb_used(&rb) == 3000); /* peek 不推进读指针。 */

    /* 读 2000，留 1000；再写 2000 触发 head 环绕。 */
    CHECK(rb_read(&rb, rd, 2000) == 2000);
    CHECK(memcmp(rd, w1, 2000) == 0);
    CHECK(rb_used(&rb) == 1000);

    for (i = 0; i < sizeof(w2); i++) {
        w2[i] = (uint8_t)(0x80 | (i & 0x7F));
    }
    CHECK(rb_write(&rb, w2, sizeof(w2)) == sizeof(w2));
    CHECK(rb_used(&rb) == 3000);

    /* 顺序读出：w1 剩余 1000 字节 + w2 全部 2000 字节。 */
    CHECK(rb_read(&rb, rd, 3000) == 3000);
    CHECK(memcmp(rd, w1 + 2000, 1000) == 0);
    CHECK(memcmp(rd + 1000, w2, sizeof(w2)) == 0);
    CHECK(rb_used(&rb) == 0);

    /* 满则拒写：只写可用空间，不覆盖旧数据。 */
    for (i = 0; i < RB_CAPACITY; i++) {
        big[i] = (uint8_t)(i & 0xFF);
    }
    CHECK(rb_write(&rb, big, RB_CAPACITY) == RB_CAPACITY);
    CHECK(rb_write(&rb, w2, sizeof(w2)) == 0);
    CHECK(rb_used(&rb) == RB_CAPACITY);
    CHECK(rb_read(&rb, rd, RB_CAPACITY) == RB_CAPACITY);
    CHECK(memcmp(rd, big, RB_CAPACITY) == 0);
}

static void test_packet_parser(void)
{
    uint8_t frame1[PROTOCOL_MAX_FRAME_SIZE];
    uint8_t frame2[PROTOCOL_MAX_FRAME_SIZE];
    uint8_t feed[PROTOCOL_MAX_FRAME_SIZE * 2];
    size_t len1, len2;

    len1 = build_frame(CMD_SENSOR_DATA, (const uint8_t *)"hello", 5,
                       frame1, sizeof(frame1));
    len2 = build_frame(CMD_HEARTBEAT, NULL, 0, frame2, sizeof(frame2));
    CHECK(len1 == PACKET_MIN_SIZE + 5);
    CHECK(len2 == PACKET_MIN_SIZE);

    g_frame_count = 0;
    CHECK(packet_parser_register(1, NULL) == 0);

    /* 拆包：分两段送达同一帧。 */
    CHECK(packet_parser_feed(1, frame1, 5) == 0);
    CHECK(g_frame_count == 0); /* 帧尚未完整。 */
    CHECK(packet_parser_feed(1, frame1 + 5, len1 - 5) == 0);
    CHECK(g_frame_count == 1);
    CHECK(g_last_len == len1 && memcmp(g_last_frame, frame1, len1) == 0);

    /* 粘包：两帧一次性送达。 */
    memcpy(feed, frame1, len1);
    memcpy(feed + len1, frame2, len2);
    CHECK(packet_parser_feed(1, feed, len1 + len2) == 0);
    CHECK(g_frame_count == 3);
    CHECK(g_last_len == len2 && memcmp(g_last_frame, frame2, len2) == 0);

    /* 坏帧：连续垃圾字节不产生帧，状态机自动恢复。 */
    {
        uint8_t garbage[64];
        memset(garbage, 0xFF, sizeof(garbage));
        CHECK(packet_parser_feed(1, garbage, sizeof(garbage)) == 0);
        CHECK(g_frame_count == 3); /* 无新增。 */
    }

    /* 超长长度字段（0x0800 > 1024）：重置状态机，不产生帧、不崩溃。 */
    {
        uint8_t huge[] = { 0xAA, 0x55, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00 };
        CHECK(packet_parser_feed(1, huge, sizeof(huge)) == 0);
        CHECK(g_frame_count == 3);
    }

    /* 注销后 feed 返回 -1。 */
    packet_parser_unregister(1);
    errno = 0;
    CHECK(packet_parser_feed(1, frame1, len1) == -1);
    CHECK(errno == ENOENT);

    /* 大载荷（600B）多轮分片送达，跨 4KB RingBuffer 绕回边界验证批量读取。 */
    {
        uint8_t big_payload[600];
        uint8_t big_frame[PROTOCOL_MAX_FRAME_SIZE];
        size_t big_len;
        int r;

        for (size_t k = 0; k < sizeof(big_payload); k++) {
            big_payload[k] = (uint8_t)(k & 0xFF);
        }
        big_len = build_frame(CMD_SENSOR_DATA, big_payload, sizeof(big_payload),
                              big_frame, sizeof(big_frame));
        CHECK(big_len == PACKET_MIN_SIZE + sizeof(big_payload));

        g_frame_count = 0;
        CHECK(packet_parser_register(7, NULL) == 0);
        for (r = 0; r < 8; r++) {
            size_t off = 0;
            while (off < big_len) {
                size_t chunk = (big_len - off) > 137 ? 137 : (big_len - off);
                CHECK(packet_parser_feed(7, big_frame + off, chunk) == 0);
                off += chunk;
            }
            CHECK(g_frame_count == r + 1);
            CHECK(g_last_len == big_len &&
                  memcmp(g_last_frame, big_frame, big_len) == 0);
        }
        packet_parser_unregister(7);
    }

    /* ctx 直连解析 + 前导非 0xAA 垃圾批量跳过 + 解析器统计计数校验。 */
    {
        struct parse_ctx *ctx = NULL;
        struct parser_stats before;
        struct parser_stats after;
        uint8_t noise_and_frame[128 + PROTOCOL_MAX_FRAME_SIZE];
        uint8_t bad_crc[PROTOCOL_MAX_FRAME_SIZE];
        uint8_t bad_len[] = { 0xAA, 0x55, 0x09, 0x00, 0x01, 0x00 };

        packet_parser_get_stats(&before);
        g_frame_count = 0;

        CHECK(packet_parser_register(8, &ctx) == 0);
        CHECK(ctx != NULL);

        memset(noise_and_frame, 0x33, 128);
        memcpy(noise_and_frame + 128, frame1, len1);
        CHECK(packet_parser_feed_ctx(ctx, noise_and_frame, 128 + len1) == 0);
        CHECK(g_frame_count == 1);
        CHECK(g_last_len == len1 && memcmp(g_last_frame, frame1, len1) == 0);

        memcpy(bad_crc, frame1, len1);
        bad_crc[len1 - 1] ^= 0xFF;
        CHECK(packet_parser_feed_ctx(ctx, bad_crc, len1) == 0);
        CHECK(packet_parser_feed_ctx(ctx, bad_len, sizeof(bad_len)) == 0);
        CHECK(g_frame_count == 1);

        packet_parser_get_stats(&after);
        CHECK(after.frames_ok == before.frames_ok + 1);
        CHECK(after.crc_errors == before.crc_errors + 1);
        CHECK(after.len_errors == before.len_errors + 1);

        packet_parser_unregister(8);
    }

}

int main(void)
{
    test_crc16();
    test_packet_encode_verify();
    test_packet_decode();
    test_ringbuffer();
    test_packet_parser();

    printf("unit_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
