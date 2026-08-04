#ifndef IOT_GATEWAY_PROTOCOL_H
#define IOT_GATEWAY_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 自定义二进制协议帧布局（big-endian / 网络字节序）：
 *   偏移  大小  字段
 *   ------ ----  -----
 *      0     2    帧头：固定 0xAA 0x55
 *      2     2    长度：Payload 长度（不含帧头、长度字段、CRC）
 *      4     1    命令字：见下文 CMD_* 枚举
 *      5     N    Payload（N = 长度字段的值）
 *    5+N     2    CRC16 覆盖范围：[长度 + 命令字 + Payload]
 *
 * 总帧大小 = 2 + 2 + 1 + N + 2 = 7 + N 字节
 */

#define PACKET_HEADER_0 0xAA
#define PACKET_HEADER_1 0x55
#define PACKET_HEADER_SIZE 2
#define PACKET_LENGTH_SIZE 2
#define PACKET_CMD_SIZE    1
#define PACKET_CRC_SIZE    2
#define PACKET_MIN_SIZE    (PACKET_HEADER_SIZE + PACKET_LENGTH_SIZE + \
                            PACKET_CMD_SIZE + PACKET_CRC_SIZE) /* 7 */

/* Maximum payload length accepted by the gateway (prevents oversized frames). */
#define PROTOCOL_MAX_PAYLOAD_LEN 1024u

/* Maximum total frame size: header(2) + length(2) + cmd(1) + payload + CRC(2). */
#define PROTOCOL_MAX_FRAME_SIZE  (PACKET_HEADER_SIZE + PACKET_LENGTH_SIZE + \
                                  PACKET_CMD_SIZE + PROTOCOL_MAX_PAYLOAD_LEN + \
                                  PACKET_CRC_SIZE)

/* 1 字节命令字字段携带的命令码。*/
enum {
    CMD_SENSOR_DATA = 0x01, /* 传感器数据上报，Payload 为JSON 字符串。*/
    CMD_HEARTBEAT   = 0x02, /* 心跳包，Payload 通常为空。*/
    CMD_CONFIG      = 0x03  /* 配置下行，Payload 为JSON/配置数据。*/
};

/**
 * @brief 协议包结构体。
 *
 * 使用 C99/C11 柔性数组成员存储 Payload，使得同一结构体在 32 位和 64 位
 * 系统上都能工作，不会浪费固定缓冲区空间。
 *
 * 始终使用 malloc(sizeof(struct packet) + payload_len) 分配。
 */
struct packet {
    uint8_t  cmd;         /* 命令码，例如 CMD_SENSOR_DATA。*/
    uint16_t payload_len; /* Payload 长度（主机字节序）。*/
    uint8_t  payload[];   /* 柔性数组成员：变长 Payload。*/
};

/**
 * @brief 计算一段数据的 CRC16 值。
 *
 * 使用 CRC16-CCITT-FALSE（多项式 0x1021，初始值 0xFFFF）。
 *
 * @param data 数据缓冲区指针。
 * @param len  待处理的字节数。
 * @return 计算得到的 CRC16 值（主机字节序）。
 */
uint16_t crc16_compute(const uint8_t *data, size_t len);

/**
 * @brief 返回一个 packet 编码后的总帧大小。
 *
 * @param pkt 待测量的包。
 * @return 总编码大小（字节），若 pkt 为 NULL 则返回 0。
 */
size_t packet_encoded_size(const struct packet *pkt);

/**
 * @brief 将 packet 编码为原始字节流。
 *
 * 输出缓冲区依次填充：帧头、长度（大端）、命令字、Payload、CRC16（大端）。
 *
 * @param pkt     待编码的包。
 * @param out     输出缓冲区。
 * @param out_len 输出缓冲区大小（字节）。
 * @return 成功返回 0，失败返回 -1 并设置 errno（如 EINVAL、ENOSPC）。
 */
int packet_encode(const struct packet *pkt, uint8_t *out, size_t out_len);

/**
 * @brief 从原始字节缓冲区解码一帧。
 *
 * 验证帧头，提取 Payload 长度，等待完整帧，校验 CRC16，然后分配
 * struct packet 给调用方。调用方必须使用 free() 释放返回的 packet。
 *
 * @param data 原始字节指针（可能包含不完整的帧）。
 * @param len  data 中的可用字节数。
 * @param pkt  输出参数：成功时指向分配的 packet。
 * @return >0：一个完整帧消耗的字节数；
 *          0：帧不完整，调用方应追加更多数据后重试；
 *         -1：错误坏帧（无效帧头或 CRC 不匹配）。
 */
ssize_t packet_decode(const uint8_t *data, size_t len, struct packet **pkt);

/**
 * @brief 验证一个完整编码帧的 CRC16。
 *
 * @param data 指向完整编码帧的指针（帧头 + 长度 + 命令字 + Payload + CRC）。
 * @param len  帧的总长度（字节）。
 * @return  1 CRC 有效，
 *          0 CRC 不匹配，
 *         -1 帧太短。
 */
int packet_checksum_verify(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* IOT_GATEWAY_PROTOCOL_H */
