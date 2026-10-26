// main/wx_ancs_proto.h —— Apple Notification Center Service(ANCS)报文编解码与查询队列。
// 纯逻辑,不依赖 ESP-IDF / NimBLE,由 tests/test_wx_ancs_proto.c 覆盖。
// 协议定义见 Apple《ANCS Specification》。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 128-bit UUID 按 BLE 线上字节序(小端)排列,可直接交给 NimBLE 的 BLE_UUID128_INIT。
// 7905F431-B5CE-4E99-A40F-4B1E122D00D0
#define WX_ANCS_SERVICE_UUID_LE \
    0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4, \
    0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79
// Notification Source: 9FBF120D-6301-42D9-8C58-25E699A21DBD
#define WX_ANCS_NOTIF_SRC_UUID_LE \
    0xBD, 0x1D, 0xA2, 0x99, 0xE6, 0x25, 0x58, 0x8C, \
    0xD9, 0x42, 0x01, 0x63, 0x0D, 0x12, 0xBF, 0x9F
// Control Point: 69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9
#define WX_ANCS_CTRL_PT_UUID_LE \
    0xD9, 0xD9, 0xAA, 0xFD, 0xBD, 0x9B, 0x21, 0x98, \
    0xA8, 0x49, 0xE1, 0x45, 0xF3, 0xD8, 0xD1, 0x69
// Data Source: 22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB
#define WX_ANCS_DATA_SRC_UUID_LE \
    0xFB, 0x7B, 0x7C, 0xCE, 0x6A, 0xB3, 0x44, 0xBE, \
    0xB5, 0x4B, 0xD6, 0x24, 0xE9, 0xC6, 0xEA, 0x22

#define WX_ANCS_EVENT_ADDED    0
#define WX_ANCS_EVENT_MODIFIED 1
#define WX_ANCS_EVENT_REMOVED  2

#define WX_ANCS_FLAG_SILENT       0x01
#define WX_ANCS_FLAG_IMPORTANT    0x02
#define WX_ANCS_FLAG_PRE_EXISTING 0x04

#define WX_ANCS_CMD_GET_NOTIF_ATTRS 0
#define WX_ANCS_ATTR_APP_ID         0
#define WX_ANCS_ATTR_TITLE          1
#define WX_ANCS_ATTR_MESSAGE        3

// Get Notification Attributes:AppIdentifier + 最长 WX_ANCS_TITLE_MAX 字节的 Title 是 9 字节,
// 再加最长 WX_ANCS_MESSAGE_MAX 字节的 Message 是 12 字节。
#define WX_ANCS_ATTRS_REQUEST_LEN     9
#define WX_ANCS_ATTRS_REQUEST_MAX_LEN 12
// 超过这个长度的 App ID 会被截断;截断后的值不会被认作任何要提醒的应用。
#define WX_ANCS_APP_ID_MAX 47
// 通知标题(聊天应用里是好友名或群名)最多取 64 字节 UTF-8,约 21 个汉字。
#define WX_ANCS_TITLE_MAX 64
// 通知正文(用于转发)最多取 240 字节 UTF-8,约 80 个汉字;iOS 会截断更长的正文。
#define WX_ANCS_MESSAGE_MAX 240

typedef struct {
    uint8_t event_id;
    uint8_t flags;
    uint8_t category_id;
    uint8_t category_count;
    uint32_t uid;
} wx_ancs_ns_event_t;

// 解析 Notification Source 的 8 字节通知。长度不足返回 false;多出的字节忽略。
bool wx_ancs_parse_ns(const uint8_t *data, size_t len, wx_ancs_ns_event_t *out);

// iPhone 通知中心里的通知总数。每条 Notification Source 事件都带着该类别
// (Other、Social、Email 等 12 类)当前的通知数,各类相加即总数,无需逐条跟踪。
#define WX_ANCS_CATEGORY_COUNT 12

typedef struct {
    uint8_t per_category[WX_ANCS_CATEGORY_COUNT];
} wx_ancs_counts_t;

void wx_ancs_counts_reset(wx_ancs_counts_t *counts);
// 用一条事件更新计数,返回更新后的总数。未知类别被忽略。
uint16_t wx_ancs_counts_update(wx_ancs_counts_t *counts, const wx_ancs_ns_event_t *event);
uint16_t wx_ancs_counts_total(const wx_ancs_counts_t *counts);

// 生成请求 AppIdentifier 和 Title(with_message 时再加 Message)的 Get Notification Attributes
// 命令。cap 不足返回 0,否则返回写入字节数。
size_t wx_ancs_build_attrs_request(uint32_t uid, bool with_message, uint8_t *out, size_t cap);

typedef enum {
    WX_ANCS_DS_MORE = 0,   // 还没收完,等下一包 Data Source 通知
    WX_ANCS_DS_DONE,       // 请求的属性都已收齐
    WX_ANCS_DS_ERROR,      // 不是本模块发出的请求格式
} wx_ancs_ds_status_t;

// Data Source 响应可能拆成多包通知到达,本解析器负责拼接。
// 响应格式:CommandID(1) + UID(4),随后每个属性为 AttributeID(1) + Length(2) + 值。
typedef struct {
    uint8_t head[5];
    size_t head_len;
    uint8_t attr_hdr[3];
    size_t attr_hdr_len;
    uint8_t attrs_wanted;
    uint8_t attrs_done;
    uint8_t cur_id;
    uint16_t cur_len;
    uint16_t cur_got;
    uint32_t uid;
    char app_id[WX_ANCS_APP_ID_MAX + 1];
    bool app_id_truncated;
    char title[WX_ANCS_TITLE_MAX + 1];       // 已去掉末尾不完整的 UTF-8 字符
    char message[WX_ANCS_MESSAGE_MAX + 1];   // 同上;没请求正文时为空串
    bool done;
} wx_ancs_ds_parser_t;

// 每次发出请求前调用;with_message 与 wx_ancs_build_attrs_request 的参数一致。
void wx_ancs_ds_reset(wx_ancs_ds_parser_t *parser, bool with_message);
wx_ancs_ds_status_t wx_ancs_ds_feed(wx_ancs_ds_parser_t *parser,
                                    const uint8_t *data, size_t len);

// 截掉字符串末尾不完整的 UTF-8 多字节序列(iOS 按字节截断标题和正文),返回新长度。
size_t wx_utf8_trim_incomplete(char *text, size_t len);

// 等待查询 AppIdentifier 的通知。手机刚连上时会一次性推来全部已有通知
// (PreExisting),新到的通知排在它们前面,保证新消息先被查询、尽快提醒。
#define WX_ANCS_QUEUE_CAP 64

typedef struct {
    uint32_t uid;
    bool pre_existing;
} wx_ancs_pending_t;

typedef struct {
    wx_ancs_pending_t items[WX_ANCS_QUEUE_CAP];
    size_t count;
} wx_ancs_queue_t;

void wx_ancs_queue_init(wx_ancs_queue_t *queue);
// 已在队列中的 UID 视为成功。队列满时优先丢弃已有通知;返回 false 表示本条被丢弃。
bool wx_ancs_queue_push(wx_ancs_queue_t *queue, uint32_t uid, bool pre_existing);
bool wx_ancs_queue_pop(wx_ancs_queue_t *queue, wx_ancs_pending_t *out);
void wx_ancs_queue_remove(wx_ancs_queue_t *queue, uint32_t uid);
