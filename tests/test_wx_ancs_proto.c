// ANCS 报文编解码与查询队列的 host 测试:不依赖 SDK,直接验证 wx_ancs_proto.c。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wx_ancs_proto.h"

// 把 "7905F431-B5CE-..." 形式的 UUID 字符串转成 BLE 小端字节序。
static void uuid_to_le(const char *text, uint8_t out[16]) {
    uint8_t be[16];
    size_t n = 0;
    for (const char *p = text; *p && n < 16; p++) {
        if (*p == '-') continue;
        unsigned v;
        assert(sscanf(p, "%2x", &v) == 1);
        be[n++] = (uint8_t)v;
        p++;
    }
    assert(n == 16);
    for (size_t i = 0; i < 16; i++) out[i] = be[15 - i];
}

static void test_uuid_constants(void) {
    const uint8_t service[] = { WX_ANCS_SERVICE_UUID_LE };
    const uint8_t ns[] = { WX_ANCS_NOTIF_SRC_UUID_LE };
    const uint8_t cp[] = { WX_ANCS_CTRL_PT_UUID_LE };
    const uint8_t ds[] = { WX_ANCS_DATA_SRC_UUID_LE };
    uint8_t expected[16];

    uuid_to_le("7905F431-B5CE-4E99-A40F-4B1E122D00D0", expected);
    assert(sizeof(service) == 16 && memcmp(service, expected, 16) == 0);
    uuid_to_le("9FBF120D-6301-42D9-8C58-25E699A21DBD", expected);
    assert(sizeof(ns) == 16 && memcmp(ns, expected, 16) == 0);
    uuid_to_le("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9", expected);
    assert(sizeof(cp) == 16 && memcmp(cp, expected, 16) == 0);
    uuid_to_le("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB", expected);
    assert(sizeof(ds) == 16 && memcmp(ds, expected, 16) == 0);
}

static void test_notification_source(void) {
    const uint8_t added[8] = { 0, WX_ANCS_FLAG_PRE_EXISTING, 4, 2, 0x78, 0x56, 0x34, 0x12 };
    wx_ancs_ns_event_t ev;

    assert(wx_ancs_parse_ns(added, sizeof(added), &ev));
    assert(ev.event_id == WX_ANCS_EVENT_ADDED);
    assert(ev.flags == WX_ANCS_FLAG_PRE_EXISTING);
    assert(ev.category_id == 4 && ev.category_count == 2);
    assert(ev.uid == 0x12345678u);
    assert(!wx_ancs_parse_ns(added, 7, &ev));        // 截短的通知
}

static void test_category_counts(void) {
    wx_ancs_counts_t c;
    wx_ancs_counts_reset(&c);
    assert(wx_ancs_counts_total(&c) == 0);

    // 连接时补发:Social 类 3 条、Email 类 2 条;每条事件带的是该类的当前总数。
    wx_ancs_ns_event_t ev = { .event_id = WX_ANCS_EVENT_ADDED, .category_id = 4, .category_count = 1 };
    assert(wx_ancs_counts_update(&c, &ev) == 1);
    ev.category_count = 3;
    assert(wx_ancs_counts_update(&c, &ev) == 3);
    ev.category_id = 6;
    ev.category_count = 2;
    assert(wx_ancs_counts_update(&c, &ev) == 5);

    // 移除事件同样带着移除后的数量。
    ev.event_id = WX_ANCS_EVENT_REMOVED;
    ev.category_id = 4;
    ev.category_count = 2;
    assert(wx_ancs_counts_update(&c, &ev) == 4);

    // 未知类别不影响总数。
    ev.category_id = 200;
    ev.category_count = 9;
    assert(wx_ancs_counts_update(&c, &ev) == 4);

    // 12 个类别全部 255 条也不溢出。
    for (uint8_t id = 0; id < WX_ANCS_CATEGORY_COUNT; id++) {
        ev.category_id = id;
        ev.category_count = 255;
        (void)wx_ancs_counts_update(&c, &ev);
    }
    assert(wx_ancs_counts_total(&c) == 255 * WX_ANCS_CATEGORY_COUNT);
}

static void test_attrs_request(void) {
    uint8_t buf[16];
    assert(wx_ancs_build_attrs_request(0xA1B2C3D4u, false, buf, 8) == 0);
    assert(wx_ancs_build_attrs_request(0xA1B2C3D4u, false, buf, sizeof(buf)) == WX_ANCS_ATTRS_REQUEST_LEN);
    const uint8_t expected[9] = { 0, 0xD4, 0xC3, 0xB2, 0xA1, 0, 1, WX_ANCS_TITLE_MAX, 0 };
    assert(memcmp(buf, expected, sizeof(expected)) == 0);

    // 带正文:Message 属性后跟 2 字节小端最大长度(240 = 0x00F0)。
    assert(wx_ancs_build_attrs_request(1, true, buf, 11) == 0);
    assert(wx_ancs_build_attrs_request(0xA1B2C3D4u, true, buf, sizeof(buf)) == WX_ANCS_ATTRS_REQUEST_MAX_LEN);
    const uint8_t with_message[12] = { 0, 0xD4, 0xC3, 0xB2, 0xA1, 0, 1, WX_ANCS_TITLE_MAX, 0, 3, 0xF0, 0 };
    assert(memcmp(buf, with_message, sizeof(with_message)) == 0);
}

static size_t put_attr(uint8_t *out, uint8_t id, const char *value) {
    size_t len = strlen(value);
    out[0] = id;
    out[1] = (uint8_t)len;
    out[2] = (uint8_t)(len >> 8);
    memcpy(out + 3, value, len);
    return 3 + len;
}

// 拼一条 Data Source 响应:命令头 + AppIdentifier + Title(message 非空时再加 Message)。
static size_t build_response3(uint8_t *out, uint32_t uid, const char *app_id, const char *title,
                              const char *message) {
    size_t n = 0;
    out[n++] = 0;
    for (int i = 0; i < 4; i++) out[n++] = (uint8_t)(uid >> (8 * i));
    n += put_attr(out + n, 0, app_id);
    n += put_attr(out + n, 1, title);
    if (message) n += put_attr(out + n, 3, message);
    return n;
}

static size_t build_response(uint8_t *out, uint32_t uid, const char *app_id, const char *title) {
    return build_response3(out, uid, app_id, title, NULL);
}

static void test_data_source_single_packet(void) {
    uint8_t pkt[128];
    size_t n = build_response(pkt, 7, "com.tencent.xin", "\xE5\xBC\xA0\xE4\xB8\x89");  // 张三
    wx_ancs_ds_parser_t p;
    wx_ancs_ds_reset(&p, false);
    assert(wx_ancs_ds_feed(&p, pkt, n) == WX_ANCS_DS_DONE);
    assert(p.uid == 7);
    assert(strcmp(p.app_id, "com.tencent.xin") == 0 && !p.app_id_truncated);
    assert(strcmp(p.title, "\xE5\xBC\xA0\xE4\xB8\x89") == 0);
    assert(p.message[0] == '\0');
    // 收完后多余的包被忽略。
    assert(wx_ancs_ds_feed(&p, pkt, 3) == WX_ANCS_DS_DONE);
}

static void test_data_source_fragmented(void) {
    // MTU 23 时一包只有 20 字节载荷:头部、属性头和值都可能被拆开。逐字节喂入。
    uint8_t pkt[128];
    size_t n = build_response(pkt, 9, "com.apple.MobileSMS", "Mom");
    wx_ancs_ds_parser_t p;
    wx_ancs_ds_reset(&p, false);
    for (size_t i = 0; i + 1 < n; i++) assert(wx_ancs_ds_feed(&p, pkt + i, 1) == WX_ANCS_DS_MORE);
    assert(wx_ancs_ds_feed(&p, pkt + n - 1, 1) == WX_ANCS_DS_DONE);
    assert(p.uid == 9);
    assert(strcmp(p.app_id, "com.apple.MobileSMS") == 0);
    assert(strcmp(p.title, "Mom") == 0);

    // 按 20 字节一包切开。
    wx_ancs_ds_reset(&p, false);
    size_t off = 0;
    wx_ancs_ds_status_t st = WX_ANCS_DS_MORE;
    while (off < n) {
        size_t chunk = n - off < 20 ? n - off : 20;
        st = wx_ancs_ds_feed(&p, pkt + off, chunk);
        off += chunk;
    }
    assert(st == WX_ANCS_DS_DONE && strcmp(p.title, "Mom") == 0);
}

static void test_data_source_edge_cases(void) {
    wx_ancs_ds_parser_t p;
    uint8_t pkt[256];

    // 空 App ID 与空标题(长度为 0 的属性)。
    size_t n = build_response(pkt, 1, "", "");
    wx_ancs_ds_reset(&p, false);
    assert(wx_ancs_ds_feed(&p, pkt, n) == WX_ANCS_DS_DONE);
    assert(p.app_id[0] == '\0' && p.title[0] == '\0');

    // 不是本模块发的命令或属性。
    const uint8_t wrong_cmd[] = { 1, 1, 0, 0, 0, 0, 0, 0 };
    wx_ancs_ds_reset(&p, false);
    assert(wx_ancs_ds_feed(&p, wrong_cmd, sizeof(wrong_cmd)) == WX_ANCS_DS_ERROR);
    // 没请求正文时收到 Message 属性,或收到从没请求过的 Subtitle(2)。
    const uint8_t wrong_attr[] = { 0, 1, 0, 0, 0, 3, 0, 0 };
    wx_ancs_ds_reset(&p, false);
    assert(wx_ancs_ds_feed(&p, wrong_attr, sizeof(wrong_attr)) == WX_ANCS_DS_ERROR);
    const uint8_t subtitle[] = { 0, 1, 0, 0, 0, 2, 0, 0 };
    wx_ancs_ds_reset(&p, true);
    assert(wx_ancs_ds_feed(&p, subtitle, sizeof(subtitle)) == WX_ANCS_DS_ERROR);

    // 超长 App ID 被截断,且截断后的前缀即使碰巧相同也不算微信。
    char long_id[61];
    memset(long_id, 'a', 60);
    long_id[60] = '\0';
    memcpy(long_id, "com.tencent.xin", 15);
    n = build_response(pkt, 2, long_id, "x");
    wx_ancs_ds_reset(&p, false);
    assert(wx_ancs_ds_feed(&p, pkt, n) == WX_ANCS_DS_DONE);
    assert(p.app_id_truncated && strlen(p.app_id) == WX_ANCS_APP_ID_MAX);
    assert(strcmp(p.title, "x") == 0);

    // iOS 按字节截断标题:末尾半个汉字被去掉,不留非法 UTF-8。
    char title[WX_ANCS_TITLE_MAX + 1];
    memset(title, 'a', WX_ANCS_TITLE_MAX - 2);
    memcpy(title + WX_ANCS_TITLE_MAX - 2, "\xE4\xB8", 2);              // “中”的前两个字节
    title[WX_ANCS_TITLE_MAX] = '\0';
    n = build_response(pkt, 3, "com.tencent.xin", title);
    wx_ancs_ds_reset(&p, false);
    assert(wx_ancs_ds_feed(&p, pkt, n) == WX_ANCS_DS_DONE);
    assert(strlen(p.title) == WX_ANCS_TITLE_MAX - 2);
}

static void test_data_source_with_message(void) {
    wx_ancs_ds_parser_t p;
    uint8_t pkt[512];

    // 三个属性,按 MTU 185(每包 182 字节)切开。
    size_t n = build_response3(pkt, 42, "com.tencent.mqq", "Bob", "\xE4\xBD\xA0\xE5\xA5\xBD");  // 你好
    wx_ancs_ds_reset(&p, true);
    assert(wx_ancs_ds_feed(&p, pkt, 20) == WX_ANCS_DS_MORE);
    assert(wx_ancs_ds_feed(&p, pkt + 20, n - 20) == WX_ANCS_DS_DONE);
    assert(p.uid == 42 && strcmp(p.app_id, "com.tencent.mqq") == 0);
    assert(strcmp(p.title, "Bob") == 0 && strcmp(p.message, "\xE4\xBD\xA0\xE5\xA5\xBD") == 0);

    // 请求了正文,但两个属性收完时还没结束。
    n = build_response(pkt, 43, "com.tencent.xin", "A");
    wx_ancs_ds_reset(&p, true);
    assert(wx_ancs_ds_feed(&p, pkt, n) == WX_ANCS_DS_MORE);

    // 空正文。
    n = build_response3(pkt, 44, "com.tencent.xin", "A", "");
    wx_ancs_ds_reset(&p, true);
    assert(wx_ancs_ds_feed(&p, pkt, n) == WX_ANCS_DS_DONE && p.message[0] == '\0');

    // 正文比缓冲区长(手机不该这样,但要防住):只留前 240 字节,并去掉半个字。
    char msg[301];
    memset(msg, 'a', 300);
    msg[300] = '\0';
    memcpy(msg + WX_ANCS_MESSAGE_MAX - 1, "\xE4\xB8\xAD", 3);   // “中”跨在第 240 字节上
    n = build_response3(pkt, 45, "com.tencent.xin", "A", msg);
    wx_ancs_ds_reset(&p, true);
    size_t off = 0;
    wx_ancs_ds_status_t st = WX_ANCS_DS_MORE;
    while (off < n) {
        size_t chunk = n - off < 182 ? n - off : 182;
        st = wx_ancs_ds_feed(&p, pkt + off, chunk);
        off += chunk;
    }
    assert(st == WX_ANCS_DS_DONE);
    assert(strlen(p.message) == WX_ANCS_MESSAGE_MAX - 1);
}

static void test_utf8_trim(void) {
    char s[16];
    strcpy(s, "ab");
    assert(wx_utf8_trim_incomplete(s, 2) == 2 && strcmp(s, "ab") == 0);
    strcpy(s, "\xE4\xB8\xAD");                                         // 中(完整)
    assert(wx_utf8_trim_incomplete(s, 3) == 3);
    strcpy(s, "a\xE4\xB8");                                            // a + 半个“中”
    assert(wx_utf8_trim_incomplete(s, 3) == 1 && strcmp(s, "a") == 0);
    strcpy(s, "a\xE4");
    assert(wx_utf8_trim_incomplete(s, 2) == 1 && strcmp(s, "a") == 0);
    strcpy(s, "\xF0\x9F\x98\x80");                                     // 完整 emoji
    assert(wx_utf8_trim_incomplete(s, 4) == 4);
    strcpy(s, "\xF0\x9F\x98");
    assert(wx_utf8_trim_incomplete(s, 3) == 0 && s[0] == '\0');
    s[0] = '\0';
    assert(wx_utf8_trim_incomplete(s, 0) == 0);
}

static void test_queue_order_and_overflow(void) {
    wx_ancs_queue_t q;
    wx_ancs_pending_t item;
    wx_ancs_queue_init(&q);

    // 已有通知按到达顺序排队,新通知插到它们前面但保持彼此的先后。
    assert(wx_ancs_queue_push(&q, 1, true));
    assert(wx_ancs_queue_push(&q, 2, true));
    assert(wx_ancs_queue_push(&q, 10, false));
    assert(wx_ancs_queue_push(&q, 11, false));
    assert(wx_ancs_queue_push(&q, 10, false));       // 重复 UID 不重复排队
    assert(q.count == 4);
    assert(wx_ancs_queue_pop(&q, &item) && item.uid == 10 && !item.pre_existing);
    assert(wx_ancs_queue_pop(&q, &item) && item.uid == 11);
    assert(wx_ancs_queue_pop(&q, &item) && item.uid == 1 && item.pre_existing);
    wx_ancs_queue_remove(&q, 2);                      // 查询前就被移除
    assert(!wx_ancs_queue_pop(&q, &item));

    // 满队列:已有通知被拒;新通知挤掉最后一条已有通知。
    for (uint32_t uid = 100; uid < 100 + WX_ANCS_QUEUE_CAP; uid++) {
        assert(wx_ancs_queue_push(&q, uid, true));
    }
    assert(!wx_ancs_queue_push(&q, 999, true));
    assert(wx_ancs_queue_push(&q, 500, false));
    assert(q.count == WX_ANCS_QUEUE_CAP);
    assert(wx_ancs_queue_pop(&q, &item) && item.uid == 500);
    for (uint32_t uid = 100; uid < 100 + WX_ANCS_QUEUE_CAP - 1; uid++) {
        assert(wx_ancs_queue_pop(&q, &item) && item.uid == uid);
    }
    assert(!wx_ancs_queue_pop(&q, &item));

    // 全是新通知时,新来的挤掉最早的一条。
    for (uint32_t uid = 1; uid <= WX_ANCS_QUEUE_CAP; uid++) {
        assert(wx_ancs_queue_push(&q, uid, false));
    }
    assert(wx_ancs_queue_push(&q, 777, false));
    assert(wx_ancs_queue_pop(&q, &item) && item.uid == 2);
}

int main(void) {
    test_uuid_constants();
    test_notification_source();
    test_category_counts();
    test_attrs_request();
    test_data_source_single_packet();
    test_data_source_fragmented();
    test_data_source_edge_cases();
    test_data_source_with_message();
    test_utf8_trim();
    test_queue_order_and_overflow();
    printf("test_wx_ancs_proto: PASS\n");
    return 0;
}
