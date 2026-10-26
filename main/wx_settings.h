// main/wx_settings.h —— 保存在 NVS 里的设置:Wi-Fi 网络、Telegram 转发、哪些应用要在板子上提醒、界面语言。
// 由配网网页修改,应用任务、联网任务和 Telegram 任务读取;所有函数都可在任意任务调用(内部加锁)。
// 机器人 Token 只存在 NVS,不写进固件,也不打印到日志。
#pragma once

#include "esp_err.h"
#include "wx_apps.h"
#include "wx_lang.h"
#include "wx_tg_text.h"
#include "wx_wifi_list.h"
#include <stdbool.h>
#include <stdint.h>

#define WX_TG_NAME_MAX    64
#define WX_TG_CHAT_ID_MAX 23    // 最长如 "-1001234567890",按字符串保存
#define WX_TG_API_MAX     95
// 直连 Telegram。连不上时(例如在国内)可以在网页上改成自己的反向代理,如 Cloudflare Worker。
#define WX_TG_API_DEFAULT "https://api.telegram.org"

typedef struct {
    char token[WX_TG_TOKEN_MAX + 1];         // 空串表示没设置
    char bot_name[WX_TG_NAME_MAX + 1];       // 机器人的显示名:它发来的通知就是这个标题
    char bot_user[WX_TG_NAME_MAX + 1];       // @用户名,不含 @
    char chat_id[WX_TG_CHAT_ID_MAX + 1];     // 空串表示还没绑定聊天
    char chat_name[WX_TG_NAME_MAX + 1];
    char api_base[WX_TG_API_MAX + 1];        // 不带末尾的 "/"
    bool body;                               // 转发正文;关掉时只转发“谁发来新消息”
    uint8_t forward_mask;                    // WX_APP_BIT(...) 的组合
} wx_tg_config_t;

// 默认转发 Telegram 以外的全部聊天应用。Telegram 自己的消息不转发,也不提供这个选项。
#define WX_TG_FORWARD_ALLOWED ((uint8_t)(WX_APP_ALL_BITS & ~WX_APP_BIT(WX_APP_TELEGRAM)))
#define WX_TG_FORWARD_DEFAULT WX_TG_FORWARD_ALLOWED

// 需在 nvs_flash_init() 成功后调用;NVS 不可用时也会成功,只是设置不能保存。
esp_err_t wx_settings_init(bool nvs_ok);

void wx_settings_wifi(wx_wifi_list_t *out);
wx_wifi_put_t wx_settings_wifi_put(const char *ssid, const char *password, esp_err_t *saved);
esp_err_t wx_settings_wifi_remove(const char *ssid);
esp_err_t wx_settings_wifi_clear(void);
bool wx_settings_has_wifi(void);

void wx_settings_tg(wx_tg_config_t *out);
esp_err_t wx_settings_set_tg(const wx_tg_config_t *config);
// Token 和聊天都设置好了,才会转发。
bool wx_settings_tg_ready(void);
// 这个应用的新消息是否要转发(已设置好 Telegram 且勾选了该应用)。开销很小,可在 BLE 回调里调用。
bool wx_settings_forwards(uint8_t app);

uint8_t wx_settings_alert_mask(void);
esp_err_t wx_settings_set_alert_mask(uint8_t mask);

bool wx_settings_muted(void);
esp_err_t wx_settings_set_muted(bool muted);

// 当前界面语言。开销很小,可在任意任务(包括 BLE 回调)里调用。
wx_lang_t wx_lang(void);
esp_err_t wx_settings_set_lang(wx_lang_t lang);

// 按当前语言选一句固定文字:WX_TR("Sound on", "提示音已开")。两句都要是字符串常量。
#define WX_TR(en, zh) (wx_lang() == WX_LANG_ZH ? (zh) : (en))
