// main/wx_tg.h —— Telegram 机器人:把新消息转发到绑定的聊天,并为配网网页验证 Token、绑定聊天、发测试消息。
// 板子直接调用 Telegram Bot API(或网页上填的反向代理),不需要中转服务器。
// 所有网络请求都在 Telegram 任务里串行进行;没联网时转发先排队,联网后补发。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t sent;              // 开机以来转发成功的条数
    uint32_t dropped;           // 放弃的条数(Telegram 拒收,或重试太多次)
    uint8_t pending;            // 排队等发送的条数
    const char *last_error;     // 最近一次失败的原因(中文短句);最近一次成功后为 NULL
} wx_tg_stats_t;

// 统计有变化。运行在 Telegram 任务:只能入队,读统计请调用 wx_tg_stats()。
typedef void (*wx_tg_cb_t)(void *ctx);

// 需在 wx_settings_init() 和 wx_net_start() 之后调用。
esp_err_t wx_tg_start(wx_tg_cb_t cb, void *ctx);

// 排队转发一条新消息,非阻塞。message 是 malloc 得到的正文(可为 NULL),所有权交给本模块。
// phone 为空串时转发里不标手机。
void wx_tg_forward(uint8_t app, const char *phone, const char *title, char *message);

// 以下三个给配网网页用:阻塞到请求完成(最长约 30 秒)。失败时 *error 是给用户看的原因。
// 用 token 调 getMe,验证它并取机器人的名字。api_base 为 NULL 时用已保存的地址。
esp_err_t wx_tg_check_token(const char *api_base, const char *token, char *bot_name, size_t name_cap,
                            char *bot_user, size_t user_cap, const char **error);
// 用已保存的 Token 查最近给机器人发消息的聊天(getUpdates),并在那个聊天里发一句确认。
esp_err_t wx_tg_find_chat(char *chat_id, size_t id_cap, char *chat_name, size_t name_cap,
                          const char **error);
// 用已保存的设置发一条测试消息。
esp_err_t wx_tg_send_test(const char **error);

void wx_tg_stats(wx_tg_stats_t *out);
