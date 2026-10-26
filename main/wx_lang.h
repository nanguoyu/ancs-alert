// main/wx_lang.h —— 界面语言。板子、设置网页、转发到 Telegram 的文字和人声提示都跟着它走。
// 纯定义,不依赖 ESP-IDF;当前语言由 wx_settings 保存(wx_lang())。
#pragma once

typedef enum {
    WX_LANG_EN = 0,     // 默认
    WX_LANG_ZH,         // 简体中文
    WX_LANG_COUNT,
} wx_lang_t;

// 网页与 NVS 里用的代码。
static inline const char *wx_lang_code(wx_lang_t lang)
{
    return lang == WX_LANG_ZH ? "zh" : "en";
}
