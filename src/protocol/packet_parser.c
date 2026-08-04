#include "protocol/packet_parser.h"

#include <errno.h>
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

/* 活跃解析器上下文的链表。 */
static struct parse_ctx *g_ctx_list = NULL;

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
 * @brief 按文件描述符查找解析器上下文。
 */
static struct parse_ctx *parser_find(int fd)
{
    struct parse_ctx *ctx;

    for (ctx = g_ctx_list; ctx != NULL; ctx = ctx->next) {
        if (ctx->fd == fd) {
            return ctx;
        }
    }

    return NULL;
}

int packet_parser_register(int fd, struct parse_ctx **ctx)
{
    struct parse_ctx *new_ctx;

    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }

    if (parser_find(fd) != NULL) {
        /* 已注册。 */
        if (ctx != NULL) {
            *ctx = parser_find(fd);
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

    new_ctx->next = g_ctx_list;
    g_ctx_list = new_ctx;

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

    pp = &g_ctx_list;
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
 * @brief 从 RingBuffer 消费一个字节并驱动状态机。
 *
 * @param ctx  解析器上下文。
 * @param byte 来自 RingBuffer 的下一个字节。
 *
 * @return 帧完成时返回 1，否则返回 0。
 */
static int parser_consume_byte(struct parse_ctx *ctx, uint8_t byte)
{
    /*
     * 硬件级防护：如果累积偏移量达到 temp_buf 的实际大小，说明出了问题
     *（例如 PARSER_MAX_PAYLOAD_LEN 和 PARSER_MAX_FRAME_SIZE 不一致）。
     * 重置状态机以避免缓冲区写入越界。
     */
   if (ctx->recv_offset >= sizeof(ctx->temp_buf)) {
        LOG_ERROR("parser: fd=%d temp_buf overflow, reset", ctx->fd);
       parser_reset(ctx);
       return 0;
    }

    switch (ctx->state) {
    case STATE_WAIT_HEAD:
        /*
         * 需要两个帧头字节：0xAA 0x55。recv_offset 跟踪已存储的帧头
         * 字节数。
         */
        if (ctx->recv_offset == 0) {
            if (byte == PACKET_HEADER_0) {
                ctx->temp_buf[0] = byte;
                ctx->recv_offset = 1;
            }
            /* 在等待 0xAA 期间任何其他字节都被忽略。 */
        } else { /* recv_offset == 1，等待 0x55 */
            if (byte == PACKET_HEADER_1) {
                ctx->temp_buf[1] = byte;
                ctx->recv_offset = 2;
                ctx->state = STATE_WAIT_LEN;
            } else if (byte == PACKET_HEADER_0) {
                /* 0xAA 后跟另一个 0xAA：保留新的 0xAA。 */
                ctx->temp_buf[0] = byte;
                ctx->recv_offset = 1;
            } else {
                /* 帧头损坏，重新开始。 */
                parser_reset(ctx);
            }
        }
        break;

    case STATE_WAIT_LEN:
        /*
         * 将 2 字节大端 Payload 长度累加到 temp_buf[2:3]。
         */
        ctx->temp_buf[ctx->recv_offset++] = byte;

        if (ctx->recv_offset == 4) {
            ctx->expected_len = (uint16_t)((ctx->temp_buf[2] << 8) |
                                           ctx->temp_buf[3]);

            /* 防御恶意或损坏的长度字段。 */
           if (ctx->expected_len > PARSER_MAX_PAYLOAD_LEN) {
                LOG_ERROR("parser: fd=%d excessive payload len %u, reset",
                          ctx->fd, (unsigned int)ctx->expected_len);
               parser_reset(ctx);
               break;
            }

            ctx->state = STATE_WAIT_PAYLOAD;
        }
        break;

    case STATE_WAIT_PAYLOAD:
        /*
         * 需要命令字（1）加上 Payload（expected_len）。
         * 它们从 temp_buf[4] 开始存放。
         */
        ctx->temp_buf[ctx->recv_offset++] = byte;

        if (ctx->recv_offset >= 4u + 1u + ctx->expected_len) {
            ctx->state = STATE_WAIT_CRC;
        }
        break;

    case STATE_WAIT_CRC:
        /* 在帧末尾累加 2 字节的 CRC。 */
        ctx->temp_buf[ctx->recv_offset++] = byte;

        if (ctx->recv_offset >= 4u + 1u + ctx->expected_len + 2u) {
            size_t frame_len;

            frame_len = ctx->recv_offset;

            if (packet_checksum_verify(ctx->temp_buf, frame_len) == 1) {
                ctx->state = STATE_FRAME_COMPLETE;
                return 1;
            }

            LOG_ERROR("parser: fd=%d CRC mismatch, drop frame", ctx->fd);
            parser_reset(ctx);
        }
        break;

    case STATE_FRAME_COMPLETE:
        /* 不应到达此处；安全起见重置。 */
        parser_reset(ctx);
        break;
    }

    return 0;
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
    uint8_t byte;

    if (ctx == NULL) {
        return;
    }

    while (rb_used(&ctx->rb) > 0) {
        if (rb_read(&ctx->rb, &byte, 1) != 1) {
            break;
        }

        if (parser_consume_byte(ctx, byte)) {
            parser_deliver_frame(ctx);
        }
    }
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

    if (len > 0) {
        size_t written;

        written = rb_write(&ctx->rb, data, len);
       if (written < len) {
            LOG_ERROR("parser: fd=%d ring buffer full, wrote %zu/%zu bytes, "
                      "closing connection", fd, written, len);
           parser_reset(ctx);
            errno = ENOBUFS;
            return -1;
        }
    }

    parser_run(ctx);

    return 0;
}
