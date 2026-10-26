// main/wx_apps.h —— 需要提醒的应用:iOS 应用标识到应用种类、显示名称和颜色的对照。
// 纯逻辑,由 tests/test_wx_apps.c 覆盖。
#pragma once

#include "wx_lang.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WX_APP_WECHAT = 0,
    WX_APP_MESSAGES,     // 信息(iMessage / 短信)
    WX_APP_TELEGRAM,
    WX_APP_QQ,           // QQ 与 TIM
    WX_APP_WHATSAPP,     // WhatsApp 与 WhatsApp Business
    WX_APP_SIGNAL,
    WX_APP_LINE,
    WX_APP_COUNT,        // 不超过 8:设置里用一个字节的位图保存
} wx_app_t;

#define WX_APP_UNKNOWN   0xFF
#define WX_APP_BIT(app)  ((uint8_t)(1u << (app)))
#define WX_APP_ALL_BITS  ((uint8_t)((1u << WX_APP_COUNT) - 1))

// 应用标识对应的种类;不提醒的应用(或被截断的标识)返回 WX_APP_UNKNOWN。
uint8_t wx_app_from_bundle(const char *bundle_id, bool truncated);

// 界面与转发里显示的名称,如 "WeChat" / "微信"。未知种类返回 ""。
const char *wx_app_name(uint8_t app, wx_lang_t lang);

// 提醒页的底色(RGB888)。
uint32_t wx_app_color(uint8_t app);

// 关闭消息详情或开了隐私保护时,通知标题只有应用名;这类标题不当作联系人显示。
bool wx_app_title_is_generic(uint8_t app, const char *title);
