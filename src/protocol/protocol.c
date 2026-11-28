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

/* 编译期 CRC16-CCITT-FALSE（多项式 0x1021）常量查找表，无锁线程安全。 */
static const uint16_t s_crc16_table[256] = {
    0x0000u, 0x1021u, 0x2042u, 0x3063u, 0x4084u, 0x50A5u, 0x60C6u, 0x70E7u,
    0x8108u, 0x9129u, 0xA14Au, 0xB16Bu, 0xC18Cu, 0xD1ADu, 0xE1CEu, 0xF1EFu,
    0x1231u, 0x0210u, 0x3273u, 0x2252u, 0x52B5u, 0x4294u, 0x72F7u, 0x62D6u,
    0x9339u, 0x8318u, 0xB37Bu, 0xA35Au, 0xD3BDu, 0xC39Cu, 0xF3FFu, 0xE3DEu,
    0x2462u, 0x3443u, 0x0420u, 0x1401u, 0x64E6u, 0x74C7u, 0x44A4u, 0x5485u,
    0xA56Au, 0xB54Bu, 0x8528u, 0x9509u, 0xE5EEu, 0xF5CFu, 0xC5ACu, 0xD58Du,
    0x3653u, 0x2672u, 0x1611u, 0x0630u, 0x76D7u, 0x66F6u, 0x5695u, 0x46B4u,
    0xB75Bu, 0xA77Au, 0x9719u, 0x8738u, 0xF7DFu, 0xE7FEu, 0xD79Du, 0xC7BCu,
    0x48C4u, 0x58E5u, 0x6886u, 0x78A7u, 0x0840u, 0x1861u, 0x2802u, 0x3823u,
    0xC9CCu, 0xD9EDu, 0xE98Eu, 0xF9AFu, 0x8948u, 0x9969u, 0xA90Au, 0xB92Bu,
    0x5AF5u, 0x4AD4u, 0x7AB7u, 0x6A96u, 0x1A71u, 0x0A50u, 0x3A33u, 0x2A12u,
    0xDBFDu, 0xCBDCu, 0xFBBFu, 0xEB9Eu, 0x9B79u, 0x8B58u, 0xBB3Bu, 0xAB1Au,
    0x6CA6u, 0x7C87u, 0x4CE4u, 0x5CC5u, 0x2C22u, 0x3C03u, 0x0C60u, 0x1C41u,
    0xEDAEu, 0xFD8Fu, 0xCDECu, 0xDDCDu, 0xAD2Au, 0xBD0Bu, 0x8D68u, 0x9D49u,
    0x7E97u, 0x6EB6u, 0x5ED5u, 0x4EF4u, 0x3E13u, 0x2E32u, 0x1E51u, 0x0E70u,
    0xFF9Fu, 0xEFBEu, 0xDFDDu, 0xCFFCu, 0xBF1Bu, 0xAF3Au, 0x9F59u, 0x8F78u,
    0x9188u, 0x81A9u, 0xB1CAu, 0xA1EBu, 0xD10Cu, 0xC12Du, 0xF14Eu, 0xE16Fu,
    0x1080u, 0x00A1u, 0x30C2u, 0x20E3u, 0x5004u, 0x4025u, 0x7046u, 0x6067u,
    0x83B9u, 0x9398u, 0xA3FBu, 0xB3DAu, 0xC33Du, 0xD31Cu, 0xE37Fu, 0xF35Eu,
    0x02B1u, 0x1290u, 0x22F3u, 0x32D2u, 0x4235u, 0x5214u, 0x6277u, 0x7256u,
    0xB5EAu, 0xA5CBu, 0x95A8u, 0x8589u, 0xF56Eu, 0xE54Fu, 0xD52Cu, 0xC50Du,
    0x34E2u, 0x24C3u, 0x14A0u, 0x0481u, 0x7466u, 0x6447u, 0x5424u, 0x4405u,
    0xA7DBu, 0xB7FAu, 0x8799u, 0x97B8u, 0xE75Fu, 0xF77Eu, 0xC71Du, 0xD73Cu,
    0x26D3u, 0x36F2u, 0x0691u, 0x16B0u, 0x6657u, 0x7676u, 0x4615u, 0x5634u,
    0xD94Cu, 0xC96Du, 0xF90Eu, 0xE92Fu, 0x99C8u, 0x89E9u, 0xB98Au, 0xA9ABu,
    0x5844u, 0x4865u, 0x7806u, 0x6827u, 0x18C0u, 0x08E1u, 0x3882u, 0x28A3u,
    0xCB7Du, 0xDB5Cu, 0xEB3Fu, 0xFB1Eu, 0x8BF9u, 0x9BD8u, 0xABBBu, 0xBB9Au,
    0x4A75u, 0x5A54u, 0x6A37u, 0x7A16u, 0x0AF1u, 0x1AD0u, 0x2AB3u, 0x3A92u,
    0xFD2Eu, 0xED0Fu, 0xDD6Cu, 0xCD4Du, 0xBDAAu, 0xAD8Bu, 0x9DE8u, 0x8DC9u,
    0x7C26u, 0x6C07u, 0x5C64u, 0x4C45u, 0x3CA2u, 0x2C83u, 0x1CE0u, 0x0CC1u,
    0xEF1Fu, 0xFF3Eu, 0xCF5Du, 0xDF7Cu, 0xAF9Bu, 0xBFBAu, 0x8FD9u, 0x9FF8u,
    0x6E17u, 0x7E36u, 0x4E55u, 0x5E74u, 0x2E93u, 0x3EB2u, 0x0ED1u, 0x1EF0u
};

uint16_t crc16_compute(const uint8_t *data, size_t len)
{
    uint16_t crc;
    size_t i;

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

int packet_encode_raw(uint8_t cmd, const uint8_t *payload, uint16_t payload_len,
                      uint8_t *out, size_t out_len)
{
    size_t total_len;
    uint16_t net_len;
    uint16_t crc;

    if (out == NULL || (payload == NULL && payload_len > 0)) {
        errno = EINVAL;
        return -1;
    }

    if (payload_len > PROTOCOL_MAX_PAYLOAD_LEN) {
        errno = EMSGSIZE;
        return -1;
    }

    total_len = PACKET_MIN_SIZE + (size_t)payload_len;
    if (out_len < total_len) {
        errno = ENOSPC;
        return -1;
    }

    /* [0:1] 帧头：固定 0xAA 0x55。 */
    out[0] = PACKET_HEADER_0;
    out[1] = PACKET_HEADER_1;

    /* [2:3] Payload 长度，大端（网络字节序）。 */
    net_len = htons(payload_len);
    memcpy(&out[2], &net_len, sizeof(net_len));

    /* [4] 命令字节。 */
    out[4] = cmd;

    /* [5:5+payload_len-1] Payload 字节。 */
    if (payload_len > 0) {
        memcpy(&out[5], payload, payload_len);
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

int packet_encode(const struct packet *pkt, uint8_t *out, size_t out_len)
{
    if (pkt == NULL) {
        errno = EINVAL;
        return -1;
    }

    return packet_encode_raw(pkt->cmd, pkt->payload, pkt->payload_len,
                             out, out_len);
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

    /* 防御超大 Payload：与 packet_parser 状态机的上限保持一致。 */
    if (payload_len > PROTOCOL_MAX_PAYLOAD_LEN) {
        errno = EBADMSG;
        return -1;
    }

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

    if (data == NULL || len < PACKET_MIN_SIZE) {
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
