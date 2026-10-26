// 配网 DNS 回答的 host 测试:不依赖 SDK,直接验证 wx_dns.c。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wx_dns.h"

static const uint8_t IP[4] = { 192, 168, 4, 1 };

// captive.apple.com 的 A 查询,id 0x1234,RD = 1,带一条 EDNS 附加记录。
static size_t build_query(uint8_t *out, uint16_t qtype) {
    static const uint8_t head[] = { 0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 1 };
    static const uint8_t name[] = { 7, 'c', 'a', 'p', 't', 'i', 'v', 'e', 5, 'a', 'p', 'p', 'l', 'e',
                                    3, 'c', 'o', 'm', 0 };
    static const uint8_t edns[] = { 0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0 };
    size_t n = 0;
    memcpy(out + n, head, sizeof(head));
    n += sizeof(head);
    memcpy(out + n, name, sizeof(name));
    n += sizeof(name);
    out[n++] = (uint8_t)(qtype >> 8);
    out[n++] = (uint8_t)qtype;
    out[n++] = 0;
    out[n++] = 1;
    memcpy(out + n, edns, sizeof(edns));
    n += sizeof(edns);
    return n;
}

static void test_a_query(void) {
    uint8_t msg[256];
    const size_t qlen = build_query(msg, 1);
    const size_t question_end = 12 + 19 + 4;
    const size_t n = wx_dns_reply(msg, qlen, sizeof(msg), IP);
    assert(n == question_end + 16);
    assert(msg[0] == 0x12 && msg[1] == 0x34);                  // 同一个 id
    assert(msg[2] == 0x85 && msg[3] == 0x00);                  // QR AA RD,无错误
    assert(msg[5] == 1 && msg[7] == 1 && msg[9] == 0 && msg[11] == 0);
    const uint8_t *a = msg + question_end;
    assert(a[0] == 0xC0 && a[1] == 12);                        // 指回问题里的名字
    assert(a[3] == 1 && a[5] == 1 && a[11] == 4);
    assert(memcmp(a + 12, IP, 4) == 0);
}

static void test_other_types_get_empty_answer(void) {
    uint8_t msg[256];
    const size_t qlen = build_query(msg, 28);                    // AAAA
    const size_t n = wx_dns_reply(msg, qlen, sizeof(msg), IP);
    assert(n == 12 + 19 + 4);
    assert(msg[7] == 0 && msg[3] == 0x00);
}

static void test_rejects_bad_messages(void) {
    uint8_t msg[256];
    size_t qlen = build_query(msg, 1);
    msg[2] |= 0x80;                                             // 是回答,不是查询
    assert(wx_dns_reply(msg, qlen, sizeof(msg), IP) == 0);

    qlen = build_query(msg, 1);
    msg[5] = 0;                                                 // 没有问题
    assert(wx_dns_reply(msg, qlen, sizeof(msg), IP) == 0);

    qlen = build_query(msg, 1);
    assert(wx_dns_reply(msg, 20, sizeof(msg), IP) == 0);        // 名字没结束就截断了
    assert(wx_dns_reply(msg, 11, sizeof(msg), IP) == 0);
    assert(wx_dns_reply(msg, 12 + 19 + 3, sizeof(msg), IP) == 0);

    qlen = build_query(msg, 1);
    msg[12] = 0xC0;                                             // 问题里不该有压缩指针
    assert(wx_dns_reply(msg, qlen, sizeof(msg), IP) == 0);

    // 缓冲区放不下回答。
    qlen = build_query(msg, 1);
    assert(wx_dns_reply(msg, qlen, 12 + 19 + 4 + 15, IP) == 0);
    assert(wx_dns_reply(NULL, qlen, sizeof(msg), IP) == 0);
}

int main(void) {
    test_a_query();
    test_other_types_get_empty_answer();
    test_rejects_bad_messages();
    printf("test_wx_dns: PASS\n");
    return 0;
}
