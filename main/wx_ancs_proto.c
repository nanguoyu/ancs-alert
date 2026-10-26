// main/wx_ancs_proto.c —— ANCS 报文编解码与查询队列(纯逻辑)。
#include "wx_ancs_proto.h"

#include <string.h>

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool wx_ancs_parse_ns(const uint8_t *data, size_t len, wx_ancs_ns_event_t *out)
{
    if (!data || !out || len < 8) return false;
    out->event_id = data[0];
    out->flags = data[1];
    out->category_id = data[2];
    out->category_count = data[3];
    out->uid = read_le32(&data[4]);
    return true;
}

void wx_ancs_counts_reset(wx_ancs_counts_t *counts)
{
    memset(counts, 0, sizeof(*counts));
}

uint16_t wx_ancs_counts_total(const wx_ancs_counts_t *counts)
{
    uint16_t total = 0;
    for (size_t i = 0; i < WX_ANCS_CATEGORY_COUNT; i++) total += counts->per_category[i];
    return total;
}

uint16_t wx_ancs_counts_update(wx_ancs_counts_t *counts, const wx_ancs_ns_event_t *event)
{
    if (event->category_id < WX_ANCS_CATEGORY_COUNT) {
        counts->per_category[event->category_id] = event->category_count;
    }
    return wx_ancs_counts_total(counts);
}

static void put_u16(uint8_t *out, uint16_t v)
{
    out[0] = (uint8_t)(v & 0xFF);
    out[1] = (uint8_t)(v >> 8);
}

size_t wx_ancs_build_attrs_request(uint32_t uid, bool with_message, uint8_t *out, size_t cap)
{
    const size_t len = with_message ? WX_ANCS_ATTRS_REQUEST_MAX_LEN : WX_ANCS_ATTRS_REQUEST_LEN;
    if (!out || cap < len) return 0;
    out[0] = WX_ANCS_CMD_GET_NOTIF_ATTRS;
    out[1] = (uint8_t)(uid & 0xFF);
    out[2] = (uint8_t)((uid >> 8) & 0xFF);
    out[3] = (uint8_t)((uid >> 16) & 0xFF);
    out[4] = (uint8_t)((uid >> 24) & 0xFF);
    out[5] = WX_ANCS_ATTR_APP_ID;
    out[6] = WX_ANCS_ATTR_TITLE;
    put_u16(&out[7], WX_ANCS_TITLE_MAX);
    if (with_message) {
        out[9] = WX_ANCS_ATTR_MESSAGE;
        put_u16(&out[10], WX_ANCS_MESSAGE_MAX);
    }
    return len;
}

void wx_ancs_ds_reset(wx_ancs_ds_parser_t *parser, bool with_message)
{
    memset(parser, 0, sizeof(*parser));
    parser->attrs_wanted = with_message ? 3 : 2;
}

// 文本属性(Title / Message)的缓冲区;App ID 单独处理,因为它要记录是否被截断。
static char *text_buffer(wx_ancs_ds_parser_t *parser, size_t *max)
{
    if (parser->cur_id == WX_ANCS_ATTR_TITLE) {
        *max = WX_ANCS_TITLE_MAX;
        return parser->title;
    }
    *max = WX_ANCS_MESSAGE_MAX;
    return parser->message;
}

static void store_value_byte(wx_ancs_ds_parser_t *parser, uint8_t byte)
{
    const uint16_t pos = parser->cur_got;
    if (parser->cur_id == WX_ANCS_ATTR_APP_ID) {
        if (pos < WX_ANCS_APP_ID_MAX) {
            parser->app_id[pos] = (char)byte;
        } else {
            parser->app_id_truncated = true;
        }
        return;
    }
    size_t max;
    char *buf = text_buffer(parser, &max);
    if (pos < max) buf[pos] = (char)byte;
}

static void finish_attr(wx_ancs_ds_parser_t *parser)
{
    const uint16_t len = parser->cur_got;
    if (parser->cur_id == WX_ANCS_ATTR_APP_ID) {
        parser->app_id[len < WX_ANCS_APP_ID_MAX ? len : WX_ANCS_APP_ID_MAX] = '\0';
    } else {
        size_t max;
        char *buf = text_buffer(parser, &max);
        size_t end = len < max ? len : max;
        buf[end] = '\0';
        wx_utf8_trim_incomplete(buf, end);
    }
    parser->attrs_done++;
    parser->attr_hdr_len = 0;
}

static bool attr_expected(const wx_ancs_ds_parser_t *parser, uint8_t id)
{
    return id == WX_ANCS_ATTR_APP_ID || id == WX_ANCS_ATTR_TITLE ||
           (id == WX_ANCS_ATTR_MESSAGE && parser->attrs_wanted == 3);
}

wx_ancs_ds_status_t wx_ancs_ds_feed(wx_ancs_ds_parser_t *parser,
                                    const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len && !parser->done; i++) {
        const uint8_t byte = data[i];

        if (parser->head_len < sizeof(parser->head)) {
            parser->head[parser->head_len++] = byte;
            if (parser->head_len == sizeof(parser->head)) {
                if (parser->head[0] != WX_ANCS_CMD_GET_NOTIF_ATTRS) return WX_ANCS_DS_ERROR;
                parser->uid = read_le32(&parser->head[1]);
            }
            continue;
        }

        if (parser->attr_hdr_len < sizeof(parser->attr_hdr)) {
            parser->attr_hdr[parser->attr_hdr_len++] = byte;
            if (parser->attr_hdr_len == sizeof(parser->attr_hdr)) {
                parser->cur_id = parser->attr_hdr[0];
                parser->cur_len = (uint16_t)(parser->attr_hdr[1] | (parser->attr_hdr[2] << 8));
                parser->cur_got = 0;
                if (!attr_expected(parser, parser->cur_id)) return WX_ANCS_DS_ERROR;
                if (parser->cur_len == 0) finish_attr(parser);
            }
        } else {
            store_value_byte(parser, byte);
            if (++parser->cur_got == parser->cur_len) finish_attr(parser);
        }
        if (parser->attrs_done == parser->attrs_wanted) parser->done = true;
    }
    return parser->done ? WX_ANCS_DS_DONE : WX_ANCS_DS_MORE;
}

size_t wx_utf8_trim_incomplete(char *text, size_t len)
{
    // 从末尾往回找最后一个非续字节(10xxxxxx 之外),判断它引导的序列是否完整。
    size_t start = len;
    size_t back = 0;
    while (start > 0 && back < 4 && ((uint8_t)text[start - 1] & 0xC0) == 0x80) {
        start--;
        back++;
    }
    if (start == 0) {
        text[0] = '\0';
        return back ? 0 : len;
    }
    const uint8_t lead = (uint8_t)text[start - 1];
    size_t need;
    if (lead < 0x80) need = 1;
    else if ((lead & 0xE0) == 0xC0) need = 2;
    else if ((lead & 0xF0) == 0xE0) need = 3;
    else if ((lead & 0xF8) == 0xF0) need = 4;
    else need = 0;   // 非法引导字节:连同其后的续字节一起去掉

    const size_t have = back + 1;
    if (need == 0 || have < need) {
        text[start - 1] = '\0';
        return start - 1;
    }
    return len;
}

void wx_ancs_queue_init(wx_ancs_queue_t *queue)
{
    memset(queue, 0, sizeof(*queue));
}

static void queue_delete_at(wx_ancs_queue_t *queue, size_t index)
{
    memmove(&queue->items[index], &queue->items[index + 1],
            (queue->count - index - 1) * sizeof(queue->items[0]));
    queue->count--;
}

static void queue_insert_at(wx_ancs_queue_t *queue, size_t index, wx_ancs_pending_t item)
{
    memmove(&queue->items[index + 1], &queue->items[index],
            (queue->count - index) * sizeof(queue->items[0]));
    queue->items[index] = item;
    queue->count++;
}

bool wx_ancs_queue_push(wx_ancs_queue_t *queue, uint32_t uid, bool pre_existing)
{
    for (size_t i = 0; i < queue->count; i++) {
        if (queue->items[i].uid == uid) return true;
    }

    if (queue->count == WX_ANCS_QUEUE_CAP) {
        if (pre_existing) return false;
        // 腾出位置:先丢最后一条已有通知;全是新通知时丢最早的一条。
        size_t victim = 0;
        for (size_t i = queue->count; i > 0; i--) {
            if (queue->items[i - 1].pre_existing) {
                victim = i - 1;
                break;
            }
        }
        queue_delete_at(queue, victim);
    }

    size_t index = queue->count;
    if (!pre_existing) {
        index = 0;
        while (index < queue->count && !queue->items[index].pre_existing) index++;
    }
    queue_insert_at(queue, index, (wx_ancs_pending_t){ .uid = uid, .pre_existing = pre_existing });
    return true;
}

bool wx_ancs_queue_pop(wx_ancs_queue_t *queue, wx_ancs_pending_t *out)
{
    if (queue->count == 0) return false;
    *out = queue->items[0];
    queue_delete_at(queue, 0);
    return true;
}

void wx_ancs_queue_remove(wx_ancs_queue_t *queue, uint32_t uid)
{
    for (size_t i = 0; i < queue->count; i++) {
        if (queue->items[i].uid == uid) {
            queue_delete_at(queue, i);
            return;
        }
    }
}
