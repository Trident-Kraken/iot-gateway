#ifndef IOT_GATEWAY_PACKET_PARSER_H
#define IOT_GATEWAY_PACKET_PARSER_H

#include <stddef.h>
#include <stdint.h>

#include "protocol/protocol.h"
#include "utils/ringbuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 为什么需要每个连接独立的 RingBuffer？
 *
 * TCP 是字节流。多个客户端连接共享同一个 accept socket，但每个连接是
 * 独立的流。如果使用单一的共享缓冲区，来自连接 A 和连接 B 的字节会交错
 * 混合，破坏帧边界。每个连接独立的 RingBuffer 使每条流保持隔离，状态机
 * 可以为每个客户端独立进行粘包拆包解析。
 */

/* 解析器接受的最大Payload 长度（防御超大帧）。*/
#define PARSER_MAX_PAYLOAD_LEN PROTOCOL_MAX_PAYLOAD_LEN

/* 最大编码帧大小：帧头2) + 长度(2) + 命令字1) + Payload + CRC(2)。*/
#define PARSER_MAX_FRAME_SIZE  PROTOCOL_MAX_FRAME_SIZE

/* 粘包/拆包解析的状态机状态。*/
enum parser_state {
    STATE_WAIT_HEAD,     /* 等待帧头 0xAA 0x55。*/
    STATE_WAIT_LEN,      /* 等待 2 字节的Payload 长度。*/
    STATE_WAIT_PAYLOAD,  /* 等待命令字+ Payload。*/
    STATE_WAIT_CRC,      /* 等待 2 字节的CRC。*/
    STATE_FRAME_COMPLETE /* 完整帧已组装完成。*/
};

/**
 * @brief 每个连接的协议解析上下文。
 */
struct parse_ctx {
    int fd;                       /* 所属连接的文件描述符。*/
    ringbuffer_t rb;              /* 该连接独立的 RingBuffer。*/
    enum parser_state state;      /* 当前解析器状态。*/
    uint16_t expected_len;        /* 从帧头解码出的Payload 长度。*/
    size_t recv_offset;           /* temp_buf 中已积累的字节数。*/
    uint8_t temp_buf[PARSER_MAX_FRAME_SIZE]; /* 帧组装缓冲区。*/
    struct parse_ctx *next;       /* 链表指针（内部使用）。*/
};

/**
 * @brief 当完整帧被解析时调用的回调函数。
 *
 * 上层（如网关主循环或线程池入队器）必须实现此符号。@p frame 指向包含
 * 帧头和 CRC 的完整编码帧；@p len 是帧的总长度。
 *
 * @param fd    连接文件描述符。
 * @param frame 完整编码帧。
 * @param len   帧长度（字节）。
 */
extern void on_frame_complete(int fd, const uint8_t *frame, size_t len);

/**
 * @brief 注册一个连接用于包解析。
 *
 * 创建带已初始化 RingBuffer 的解析器上下文。
 *
 * @param fd  连接文件描述符。
 * @param ctx 可选输出参数，用于接收创建的上下文。
 * @return 成功返回 0，失败返回 -1。
 */
int packet_parser_register(int fd, struct parse_ctx **ctx);

/**
 * @brief 注销一个连接并释放其解析器上下文。
 *
 * @param fd 连接文件描述符。
 */
void packet_parser_unregister(int fd);

/**
 * @brief 根据文件描述符查找解析器上下文。
 *
 * @param fd 连接文件描述符。
 * @return 指向上下文的指针，未找到时返回 NULL。
 */
struct parse_ctx *packet_parser_get_ctx(int fd);

/**
 * @brief 将新收到的原始字节送入解析器。
 *
 * 数据被追加到连接的 RingBuffer 中，然后状态机运行，直到 RingBuffer
 * 不再包含当前状态所需的足够字节。当完整帧组装完成后，会调用
 * on_frame_complete()。
 *
 * @param fd   连接文件描述符。
 * @param data 从 recv() 接收到的原始字节。
 * @param len  接收到的字节数。
 * @return 成功返回 0，连接未注册时返回 -1。
 */
int packet_parser_feed(int fd, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* IOT_GATEWAY_PACKET_PARSER_H */
