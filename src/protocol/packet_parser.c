#include "protocol/packet_parser.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol/protocol.h"
#include "utils/logger.h"
#include "utils/ringbuffer.h"

/* 解析器接受的最大 Payload 长度（防御超大帧）。 */
#define PARSER_MAX_PAYLOAD_LEN PROTOCOL_MAX_PAYLOAD_LEN

/* 最大编码帧大小：帧头(2) + 长度(2) + 命令字(1) + Payload + CRC(2)。 */
#define PARSER_MAX_FRAME_SIZE  PROTOCOL_MAX_FRAME_SIZE

/* 粘包/拆包解析的状态机状态。 */
enum parser_state {
    STATE_WAIT_HEAD,     /* 等待帧头 0xAA 0x55。 */
    STATE_WAIT_LEN,      /* 等待 2 字节的 Payload 长度。 */
    STATE_WAIT_PAYLOAD,  /* 等待命令字 + Payload。 */
    STATE_WAIT_CRC,      /* 等待 2 字节的 CRC。 */
    STATE_FRAME_COMPLETE /* 完整帧已组装完成。 */
};

/* 每个连接的协议解析上下文。 */
struct parse_ctx {
    int fd;                       /* 所属连接的文件描述符。 */
    ringbuffer_t rb;              /* 该连接独立的 RingBuffer。 */
    enum parser_state state;      /* 当前解析器状态。 */
    uint16_t expected_len;        /* 从帧头解码出的 Payload 长度。 */
    size_t recv_offset;           /* temp_buf 中已积累的字节数。 */
    uint8_t temp_buf[PARSER_MAX_FRAME_SIZE]; /* 帧组装缓冲区。 */
    struct parse_ctx *next;       /* 链表指针（内部使用）。 */
};

/*
 * 状态迁移图：
 *
 *     +-----------+   0xAA   +-----------+   0x55   +-----------+
 *     | WAIT_HEAD | -------->| WAIT_HEAD |--------->| WAIT_LEN  |
 *     +-----------+  wrong   +-----------+  wrong    +-----------+
 *           ^                byte 重置              |
 *           |                                        | 2 字节
 *           |                                        v
 *           |                                   +-----------+
 *           |                                   |WAIT_PAYLOAD|
 *           |                                   +-----------+
 *           |                                        |
 *           |                             命令字 + Payload 字节
 *           |                                        v
 *           |                                   +-----------+
 *           |                                   | WAIT_CRC  |
 *           |                                   +-----------+
 *           |                                        | 2 字节
 *           |                                        v
 *           |                                   +---------------+
 *           +-----------------------------------| FRAME_COMPLETE|
 *             CRC 失败 → 重置；CRC 成功 → 回调 → 重置
 *                                               +---------------+
 *
 * 粘包/拆包处理：
 *
 * 1) 一个 TCP 段包含多个完整帧：
 *    状态机逐字节消费。在 FRAME_COMPLETE 后重置为 WAIT_HEAD 并继续解析
 *    同一 RingBuffer 中的剩余字节，无需等待更多网络数据。
 *
 * 2) 一帧横跨多个 TCP 段：
 *    recv() 将每块数据追加到 RingBuffer。如果 rb_used() 小于当前状态
 *    所需的字节数，解析器返回。下一次 epoll IN 事件从保存的状态和
 *    recv_offset 继续解析。
 *
 * 3) 帧头在段边界处拆分：
 *    WAIT_HEAD 将部分帧头字节保存在 temp_buf 中。如果第一个段只到达
 *    0xAA，解析器留在 WAIT_HEAD；下一个段提供 0x55 后解析继续。
 */

/* 解析器统计计数器（仅 epoll 线程自增，relaxed 原子保证任意线程无竞争读取）。 */
static atomic_uint_fast64_t g_frames_ok  = 0;
static atomic_uint_fast64_t g_crc_errors = 0;
static atomic_uint_fast64_t g_len_errors = 0;

/* 解析器上下文哈希表桶数（2 的幂，与连接池容量匹配）。 */
#define PARSER_HASH_BUCKETS 1024u

/* 活跃解析器上下文的开链哈希表（仅 epoll 线程访问，O(1) 查找/增删）。 */
static struct parse_ctx *g_ctx_buckets[PARSER_HASH_BUCKETS];

static size_t parser_bucket(int fd)
{
    return (size_t)(((unsigned int)fd) & (PARSER_HASH_BUCKETS - 1u));
}

/**
 * @brief 将解析器上下文重置为初始状态。
 */
static void parser_reset(struct parse_ctx *ctx)
{
    if (ctx == NULL) {
        return;
    }

    ctx->state = STATE_WAIT_HEAD;
    ctx->expected_len = 0;
    ctx->recv_offset = 0;
}

/**
 * @brief 按文件描述符查找解析器上下文（O(1) 哈希桶）。
 */
static struct parse_ctx *parser_find(int fd)
{
    struct parse_ctx *ctx;

    if (fd < 0) {
        return NULL;
    }

    for (ctx = g_ctx_buckets[parser_bucket(fd)]; ctx != NULL; ctx = ctx->next) {
        if (ctx->fd == fd) {
            return ctx;
        }
    }

    return NULL;
}

int packet_parser_register(int fd, struct parse_ctx **ctx)
{
    struct parse_ctx *existing;
    struct parse_ctx *new_ctx;
    size_t bucket;

    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }

    existing = parser_find(fd);
    if (existing != NULL) {
        /* 已注册。 */
        if (ctx != NULL) {
            *ctx = existing;
        }
        return 0;
    }

    new_ctx = calloc(1, sizeof(*new_ctx));
    if (new_ctx == NULL) {
        return -1;
    }

    new_ctx->fd = fd;
    if (rb_init(&new_ctx->rb) != 0) {
        free(new_ctx);
        return -1;
    }
    parser_reset(new_ctx);

    bucket = parser_bucket(fd);
    new_ctx->next = g_ctx_buckets[bucket];
    g_ctx_buckets[bucket] = new_ctx;

    if (ctx != NULL) {
        *ctx = new_ctx;
    }

    return 0;
}

void packet_parser_unregister(int fd)
{
    struct parse_ctx **pp;
    struct parse_ctx *cur;

    if (fd < 0) {
        return;
    }

    pp = &g_ctx_buckets[parser_bucket(fd)];
    while (*pp != NULL) {
        cur = *pp;
        if (cur->fd == fd) {
            *pp = cur->next;
            free(cur);
            return;
        }
        pp = &cur->next;
    }
}

/**
 * @brief 递送一个完成的帧并重置解析器。
 */
static void parser_deliver_frame(struct parse_ctx *ctx)
{
    size_t frame_len;

    if (ctx == NULL || ctx->state != STATE_FRAME_COMPLETE) {
        return;
    }

    atomic_fetch_add_explicit(&g_frames_ok, 1, memory_order_relaxed);
    frame_len = 4u + 1u + ctx->expected_len + 2u;
    on_frame_complete(ctx->fd, ctx->temp_buf, frame_len);

    /* 为下一帧做好准备。 */
    parser_reset(ctx);
}

/**
 * @brief 运行状态机直到 RingBuffer 耗尽。
 */
static void parser_run(struct parse_ctx *ctx)
{
    if (ctx == NULL) {
        return;
    }

    while (rb_used(&ctx->rb) > 0) {
        /*
         * 设计说明（批量同步与拷贝优化）：
         *   1. WAIT_HEAD(offset=0) 遇到大段非 0xAA 垃圾数据时，先通过 rb_peek +
         *      memchr 跳过整段前导垃圾，命中 0xAA 后一并消费；
         *   2. WAIT_LEN / WAIT_PAYLOAD / WAIT_CRC 已知确切剩余字节数，直接批量
         *      rb_read 进 temp_buf（配合 sizeof(temp_buf) 双边界校验），减少函数
         *      调用开销，同时严格维持五态迁移与跨段拆包语义。
         */
        switch (ctx->state) {
        case STATE_WAIT_HEAD:
            if (ctx->recv_offset == 0) {
                uint8_t peek_buf[64];
                size_t n = rb_peek(&ctx->rb, peek_buf, sizeof(peek_buf));
                const uint8_t *hit;
                size_t skip;

                if (n == 0) {
                    return;
                }
                hit = memchr(peek_buf, PACKET_HEADER_0, n);
                if (hit == NULL) {
                    (void)rb_read(&ctx->rb, peek_buf, n);
                    break;
                }
                skip = (size_t)(hit - peek_buf);
                if (rb_read(&ctx->rb, peek_buf, skip + 1u) != skip + 1u) {
                    return;
                }
                ctx->temp_buf[0] = PACKET_HEADER_0;
                ctx->recv_offset = 1u;
            } else if (ctx->recv_offset == 1u) {
                uint8_t byte;

                if (rb_read(&ctx->rb, &byte, 1u) != 1u) {
                    return;
                }
                if (byte == PACKET_HEADER_1) {
                    ctx->temp_buf[1] = byte;
                    ctx->recv_offset = 2u;
                    ctx->state = STATE_WAIT_LEN;
                } else if (byte == PACKET_HEADER_0) {
                    /* 0xAA 后跟另一个 0xAA：保留新的 0xAA 作为候选帧头。 */
                    ctx->temp_buf[0] = byte;
                    ctx->recv_offset = 1u;
                } else {
                    parser_reset(ctx);
                }
            } else {
                parser_reset(ctx);
            }
            break;

        case STATE_WAIT_LEN: {
            size_t remain = 4u - ctx->recv_offset;
            size_t avail = rb_used(&ctx->rb);
            size_t chunk = avail < remain ? avail : remain;

            if (chunk > 0) {
                size_t got = rb_read(&ctx->rb,
                                     &ctx->temp_buf[ctx->recv_offset], chunk);
                if (got == 0) {
                    return;
                }
                ctx->recv_offset += got;
            }
            if (ctx->recv_offset == 4u) {
                ctx->expected_len = (uint16_t)((ctx->temp_buf[2] << 8) |
                                               ctx->temp_buf[3]);
                if (ctx->expected_len > PARSER_MAX_PAYLOAD_LEN) {
                    LOG_ERROR("parser: fd=%d excessive payload len %u, reset",
                              ctx->fd, (unsigned int)ctx->expected_len);
                    atomic_fetch_add_explicit(&g_len_errors, 1,
                                              memory_order_relaxed);
                    parser_reset(ctx);
                } else {
                    ctx->state = STATE_WAIT_PAYLOAD;
                }
            }
            break;
        }

        case STATE_WAIT_PAYLOAD: {
            size_t target = 4u + 1u + (size_t)ctx->expected_len;
            size_t avail;
            size_t remain;
            size_t chunk;

            if (target > sizeof(ctx->temp_buf)) {
                LOG_ERROR("parser: fd=%d temp_buf overflow, reset", ctx->fd);
                parser_reset(ctx);
                break;
            }

            if (ctx->recv_offset < target) {
                avail = rb_used(&ctx->rb);
                remain = target - ctx->recv_offset;
                chunk = avail < remain ? avail : remain;
                if (chunk > 0) {
                    size_t got = rb_read(&ctx->rb,
                                         &ctx->temp_buf[ctx->recv_offset],
                                         chunk);
                    if (got == 0) {
                        return;
                    }
                    ctx->recv_offset += got;
                }
            }

            if (ctx->recv_offset >= target) {
                ctx->state = STATE_WAIT_CRC;
            }
            break;
        }

        case STATE_WAIT_CRC: {
            size_t target = 4u + 1u + (size_t)ctx->expected_len + 2u;
            size_t avail;
            size_t remain;
            size_t chunk;

            if (target > sizeof(ctx->temp_buf)) {
                LOG_ERROR("parser: fd=%d temp_buf overflow, reset", ctx->fd);
                parser_reset(ctx);
                break;
            }

            if (ctx->recv_offset < target) {
                avail = rb_used(&ctx->rb);
                remain = target - ctx->recv_offset;
                chunk = avail < remain ? avail : remain;
                if (chunk > 0) {
                    size_t got = rb_read(&ctx->rb,
                                         &ctx->temp_buf[ctx->recv_offset],
                                         chunk);
                    if (got == 0) {
                        return;
                    }
                    ctx->recv_offset += got;
                }
            }

            if (ctx->recv_offset >= target) {
                if (packet_checksum_verify(ctx->temp_buf, ctx->recv_offset) == 1) {
                    ctx->state = STATE_FRAME_COMPLETE;
                    parser_deliver_frame(ctx);
                } else {
                    LOG_ERROR("parser: fd=%d CRC mismatch, drop frame", ctx->fd);
                    atomic_fetch_add_explicit(&g_crc_errors, 1,
                                              memory_order_relaxed);
                    parser_reset(ctx);
                }
            }
            break;
        }

        case STATE_FRAME_COMPLETE:
        default:
            parser_reset(ctx);
            break;
        }
    }
}

int packet_parser_feed_ctx(struct parse_ctx *ctx, const uint8_t *data, size_t len)
{
    if (ctx == NULL || (data == NULL && len > 0)) {
        errno = EINVAL;
        return -1;
    }

    if (len > 0) {
        size_t written;

        written = rb_write(&ctx->rb, data, len);
        if (written < len) {
            LOG_ERROR("parser: fd=%d ring buffer full, wrote %zu/%zu bytes, "
                      "closing connection", ctx->fd, written, len);
            parser_reset(ctx);
            errno = ENOBUFS;
            return -1;
        }
    }

    parser_run(ctx);
    return 0;
}

int packet_parser_feed(int fd, const uint8_t *data, size_t len)
{
    struct parse_ctx *ctx;

    if (fd < 0 || (data == NULL && len > 0)) {
        errno = EINVAL;
        return -1;
    }

    ctx = parser_find(fd);
    if (ctx == NULL) {
        LOG_ERROR("packet_parser_feed: fd=%d not registered", fd);
        errno = ENOENT;
        return -1;
    }

    return packet_parser_feed_ctx(ctx, data, len);
}

void packet_parser_get_stats(struct parser_stats *out)
{
    if (out == NULL) {
        return;
    }

    out->frames_ok  = (uint64_t)atomic_load_explicit(&g_frames_ok,
                                                     memory_order_relaxed);
    out->crc_errors = (uint64_t)atomic_load_explicit(&g_crc_errors,
                                                     memory_order_relaxed);
    out->len_errors = (uint64_t)atomic_load_explicit(&g_len_errors,
                                                     memory_order_relaxed);
}
