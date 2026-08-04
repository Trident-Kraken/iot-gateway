#ifndef IOT_GATEWAY_PACKET_PARSER_H
#define IOT_GATEWAY_PACKET_PARSER_H

#include <stddef.h>
#include <stdint.h>

#include "protocol/protocol.h"

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

/*
 * 状态机（enum parser_state）与 struct parse_ctx 的完整定义位于
 * packet_parser.c（内部实现）。头文件只暴露不透明句柄，避免把 temp_buf
 * 等内部缓冲布局泄露给调用方。
 */

/**
 * @brief 每个连接的协议解析上下文（不透明句柄）。
 */
struct parse_ctx;

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
