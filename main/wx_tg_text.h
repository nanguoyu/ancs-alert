// main/wx_tg_text.h —— 转发到 Telegram 的文字:消息格式、机器人 Token 的格式检查与打码显示。
// 纯逻辑,不依赖 ESP-IDF,由 tests/test_wx_tg_text.c 覆盖。
#pragma once

#include "wx_lang.h"
#include <stdbool.h>
#include <stddef.h>

// BotFather 发的 Token 形如 "123456789:AAH...",冒号后约 35 个字符。
#define WX_TG_TOKEN_MAX 63

// 一条转发:
//   英文 "[WhatsApp, XR] Alice\nSee you at 7"
//   中文 "【微信，XR】张三\n今晚吃什么"
// phone 为 NULL 或空串时不标手机;title 是应用名之类的通用标题时(title_generic)不写联系人;
// message 为 NULL 或空串时(不转发正文,或通知本身没有正文)写“sent a new message / 发来新消息”。
// 结果按 UTF-8 字符边界截断到 cap - 1 字节。返回写入的字节数。
size_t wx_tg_format(char *out, size_t cap, wx_lang_t lang, const char *app_name, const char *phone,
                    const char *title, bool title_generic, const char *message);

bool wx_tg_token_valid(const char *token);

// 网页上显示已保存的 Token:只露出机器人编号和末尾 4 位,如 "123456789:••••AbCd"。
void wx_tg_token_hint(const char *token, char *out, size_t cap);
