// main/wx_apps.c —— 需要提醒的应用对照表(纯逻辑)。
#include "wx_apps.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    const char *bundle_id;
    uint8_t app;
} bundle_map_t;

static const bundle_map_t BUNDLES[] = {
    { "com.tencent.xin", WX_APP_WECHAT },
    { "com.apple.MobileSMS", WX_APP_MESSAGES },
    { "ph.telegra.Telegraph", WX_APP_TELEGRAM },
    { "com.tencent.mqq", WX_APP_QQ },
    { "com.tencent.tim", WX_APP_QQ },
    { "net.whatsapp.WhatsApp", WX_APP_WHATSAPP },
    { "net.whatsapp.WhatsAppSMB", WX_APP_WHATSAPP },
    { "org.whispersystems.signal", WX_APP_SIGNAL },
    { "jp.naver.line", WX_APP_LINE },
};

_Static_assert(WX_APP_COUNT <= 8, "app masks are one byte");

static const char *const NAMES[WX_LANG_COUNT][WX_APP_COUNT] = {
    [WX_LANG_EN] = { "WeChat", "Messages", "Telegram", "QQ", "WhatsApp", "Signal", "LINE" },
    [WX_LANG_ZH] = { "微信", "信息", "Telegram", "QQ", "WhatsApp", "Signal", "LINE" },
};
// 提醒页整屏底色,上面是白字:亮绿色的品牌色压暗了一些。
static const uint32_t COLORS[WX_APP_COUNT] = {
    0x07C160, 0x30B455, 0x229ED9, 0x1E88E5, 0x128C7E, 0x3A76F0, 0x06A84F,
};

// 各应用在不显示详情时使用的通用标题(手机是什么语言都可能出现)。
static const char *const GENERIC_TITLES[] = {
    "微信", "WeChat", "Weixin", "信息", "Messages", "Telegram", "QQ", "TIM",
    "WhatsApp", "WhatsApp Business", "Signal", "LINE",
};

uint8_t wx_app_from_bundle(const char *bundle_id, bool truncated)
{
    if (!bundle_id || truncated) return WX_APP_UNKNOWN;
    for (size_t i = 0; i < sizeof(BUNDLES) / sizeof(BUNDLES[0]); i++) {
        if (strcmp(bundle_id, BUNDLES[i].bundle_id) == 0) return BUNDLES[i].app;
    }
    return WX_APP_UNKNOWN;
}

const char *wx_app_name(uint8_t app, wx_lang_t lang)
{
    if (lang >= WX_LANG_COUNT) lang = WX_LANG_EN;
    return app < WX_APP_COUNT ? NAMES[lang][app] : "";
}

uint32_t wx_app_color(uint8_t app)
{
    return app < WX_APP_COUNT ? COLORS[app] : COLORS[WX_APP_WECHAT];
}

bool wx_app_title_is_generic(uint8_t app, const char *title)
{
    if (!title || title[0] == '\0') return true;
    for (int lang = 0; app < WX_APP_COUNT && lang < WX_LANG_COUNT; lang++) {
        if (strcmp(title, NAMES[lang][app]) == 0) return true;
    }
    for (size_t i = 0; i < sizeof(GENERIC_TITLES) / sizeof(GENERIC_TITLES[0]); i++) {
        if (strcmp(title, GENERIC_TITLES[i]) == 0) return true;
    }
    return false;
}
