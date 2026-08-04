#include "protocol/protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utils/logger.h"

/*
 * CRC16-CCITT-FALSE 参数：
 *   多项式：0x1021（x^16 + x^12 + x^5 + 1）
 *   初始值：0xFFFF
 *   输入反射：无
 *   输出反射：无
 *   最终异或：0x0000
 */
#define CRC16_POLYNOMIAL 0x1021u
#define CRC16_INITIAL    0xFFFFu

static uint16_t s_crc16_table[256];
static int s_crc16_initialized = 0;

/**
 * @brief 首次使用时生成 CRC16 查找表。
 */
static void crc16_ensure_table(void)
{
    size_t i;
    int j;

    if (s_crc16_initialized) {
        return;
    }

    for (i = 0; i < 256; i++) {
        uint16_t crc = (uint16_t)(i << 8);

        for (j = 0; j < 8; j++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ CRC16_POLYNOMIAL);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }

        s_crc16_table[i] = crc;
    }

    s_crc16_initialized = 1;
}

void crc16_init(void)
{
    crc16_ensure_table();
}

uint16_t crc16_compute(const uint8_t *data, size_t len)
{
    uint16_t crc;
    size_t i;

    crc16_ensure_table();

    if (data == NULL && len > 0) {
        errno = EINVAL;
        return 0;
    }

    crc = CRC16_INITIAL;

    for (i = 0; i < len; i++) {
        crc = (uint16_t)((crc << 8) ^
               s_crc16_table[((crc >> 8) ^ data[i]) & 0xFFu]);
    }

    return crc;
}

size_t packet_encoded_size(const struct packet *pkt)
{
    if (pkt == NULL) {
        return 0;
    }

    return PACKET_MIN_SIZE + pkt->payload_len;
}

int packet_encode(const struct packet *pkt, uint8_t *out, size_t out_len)
{
    size_t total_len;
    uint16_t net_len;
    uint16_t crc;

    if (pkt == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    total_len = packet_encoded_size(pkt);
    if (total_len < PACKET_MIN_SIZE) {
        /* 溢出或无效的 payload_len。 */
        errno = EOVERFLOW;
        return -1;
    }

    if (out_len < total_len) {
        errno = ENOSPC;
        return -1;
    }

    /* [0:1] 帧头：固定 0xAA 0x55。 */
    out[0] = PACKET_HEADER_0;
    out[1] = PACKET_HEADER_1;

    /* [2:3] Payload 长度，大端（网络字节序）。 */
    net_len = htons(pkt->payload_len);
    memcpy(&out[2], &net_len, sizeof(net_len));

    /* [4] 命令字节。 */
    out[4] = pkt->cmd;

    /* [5:5+payload_len-1] Payload 字节。 */
    if (pkt->payload_len > 0) {
        memcpy(&out[5], pkt->payload, pkt->payload_len);
    }

    /*
     * CRC 覆盖 [长度 + 命令字 + Payload]，即字节 [2 : total_len-2]。
     * CRC 本身以 big-endian 字节序写在末尾。
     */
    crc = crc16_compute(&out[2], total_len - PACKET_CRC_SIZE - 2);
    crc = htons(crc);
    memcpy(&out[total_len - PACKET_CRC_SIZE], &crc, sizeof(crc));

    return 0;
}

ssize_t packet_decode(const uint8_t *data, size_t len, struct packet **pkt)
{
    uint16_t net_len;
    uint16_t payload_len;
    size_t total_len;
    struct packet *p;

    if (pkt == NULL) {
        errno = EINVAL;
        return -1;
    }

    *pkt = NULL;

    if (data == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (len < PACKET_MIN_SIZE) {
        /* 数据不足连最小帧都无法构成。 */
        return 0;
    }

    /* 验证固定帧头。 */
   if (data[0] != PACKET_HEADER_0 || data[1] != PACKET_HEADER_1) {
        LOG_ERROR("packet_decode: invalid header 0x%02X 0x%02X",
                  data[0], data[1]);
       errno = EBADMSG;
       return -1;
   }

    /* 提取 Payload 长度（大端 → 主机字节序）。 */
    memcpy(&net_len, &data[2], sizeof(net_len));
    payload_len = ntohs(net_len);

    /* 计算总帧长并检查是否完整。 */
    total_len = PACKET_MIN_SIZE + payload_len;
    if (total_len < PACKET_MIN_SIZE) {
        /* payload_len 溢出。 */
        errno = EBADMSG;
        return -1;
    }

    if (len < total_len) {
        /* 帧尚未完全接收。 */
        return 0;
    }

    /* 验证 CRC 覆盖 [长度 + 命令字 + Payload]。 */
   if (!packet_checksum_verify(data, total_len)) {
        LOG_ERROR("packet_decode: CRC mismatch");
       errno = EBADMSG;
       return -1;
    }

    /* 使用柔性 Payload 数组分配 packet。 */
    p = malloc(sizeof(*p) + payload_len);
    if (p == NULL) {
        return -1;
    }

    p->cmd = data[4];
    p->payload_len = payload_len;

    if (payload_len > 0) {
        memcpy(p->payload, &data[5], payload_len);
    }

    *pkt = p;
    return (ssize_t)total_len;
}

int packet_checksum_verify(const uint8_t *data, size_t len)
{
    uint16_t net_crc;
    uint16_t expected_crc;
    uint16_t computed_crc;

    if (len < PACKET_MIN_SIZE) {
        errno = EINVAL;
        return -1;
    }

    /* CRC 存储在帧的最后两个字节，大端字节序。 */
    memcpy(&net_crc, &data[len - PACKET_CRC_SIZE], sizeof(net_crc));
    expected_crc = ntohs(net_crc);

    /*
     * CRC 覆盖长度(2) + 命令字(1) + Payload(N)，位于受保护区域的起始
     * 位置：偏移量 2，长度 = len - 2 - 2。
     */
    computed_crc = crc16_compute(&data[2], len - PACKET_HEADER_SIZE - PACKET_CRC_SIZE);

    return computed_crc == expected_crc ? 1 : 0;
}
