// main/wx_dns.c —— 配网热点上的 DNS 回答(纯逻辑)。报文格式见 RFC 1035 第 4 节。
#include "wx_dns.h"

#include <string.h>

#define HEADER_LEN   12
#define ANSWER_LEN   16
#define TYPE_A       1
#define CLASS_IN     1
#define TTL_SECONDS  60

size_t wx_dns_reply(uint8_t *msg, size_t len, size_t cap, const uint8_t ip[4])
{
    if (!msg || len < HEADER_LEN || len > cap) return 0;
    const uint8_t flags_hi = msg[2];
    const uint16_t qdcount = (uint16_t)(msg[4] << 8 | msg[5]);
    // 只回答标准查询(QR = 0,OPCODE = 0)。
    if ((flags_hi & 0x80) || (flags_hi & 0x78) || qdcount == 0) return 0;

    // 问题里的域名:长度前缀的标签序列,以 0 结尾;问题里不会出现压缩指针。
    size_t pos = HEADER_LEN;
    for (;;) {
        if (pos >= len) return 0;
        const uint8_t label = msg[pos];
        if (label == 0) break;
        if (label > 63) return 0;
        pos += 1u + label;
    }
    pos++;   // 结尾的 0
    if (pos + 4 > len) return 0;
    const uint16_t qtype = (uint16_t)(msg[pos] << 8 | msg[pos + 1]);
    const uint16_t qclass = (uint16_t)(msg[pos + 2] << 8 | msg[pos + 3]);
    pos += 4;

    const int answer = qtype == TYPE_A && (qclass & 0x7FFF) == CLASS_IN;
    if (answer && pos + ANSWER_LEN > cap) return 0;

    // 头部:QR = 1、AA = 1,保留 RD;RA = 0,RCODE = 0。只留一个问题,丢掉附加记录(如 EDNS)。
    msg[2] = (uint8_t)(0x84 | (flags_hi & 0x01));
    msg[3] = 0x00;
    msg[4] = 0;
    msg[5] = 1;
    msg[6] = 0;
    msg[7] = answer ? 1 : 0;
    memset(&msg[8], 0, 4);
    if (!answer) return pos;

    uint8_t *a = &msg[pos];
    a[0] = 0xC0;                 // 名字:指向偏移 12 的问题
    a[1] = HEADER_LEN;
    a[2] = 0;
    a[3] = TYPE_A;
    a[4] = 0;
    a[5] = CLASS_IN;
    a[6] = 0;
    a[7] = 0;
    a[8] = 0;
    a[9] = TTL_SECONDS;
    a[10] = 0;
    a[11] = 4;
    memcpy(&a[12], ip, 4);
    return pos + ANSWER_LEN;
}
