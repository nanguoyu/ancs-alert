// main/wx_ui.h —— 界面:待机页(各台 iPhone 的连接状态与通知数、Wi-Fi 与转发状态)、配对页(配对码)、
// 配网页(热点二维码)、提醒页(按应用配色整屏闪烁 + 好友名 + 未读数)、设置菜单与危险操作的确认页。
#pragma once

#include "esp_err.h"
#include "wx_apps.h"
#include "wx_ble.h"
#include "wx_lang.h"
#include "wx_net.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 板子上的设置菜单,顺序就是屏幕上的顺序。
typedef enum {
    WX_MENU_SOUND = 0,          // 提示音 开/关
    WX_MENU_TEST,               // 试听提示音
    WX_MENU_SETUP,              // 打开 Wi-Fi 与 Telegram 设置(配网热点)
    WX_MENU_LANGUAGE,           // 切换界面语言(英文 / 中文)
    WX_MENU_CLEAR_WIFI,         // 清除全部 Wi-Fi 网络(要确认)
    WX_MENU_FORGET_PHONES,      // 清除全部 iPhone 配对(要确认)
    WX_MENU_BACK,
    WX_MENU_COUNT,
} wx_menu_item_t;

// 正在确认的危险操作。
typedef enum {
    WX_CONFIRM_NONE = 0,
    WX_CONFIRM_CLEAR_WIFI,
    WX_CONFIRM_FORGET_PHONES,
} wx_confirm_t;

typedef struct {
    wx_phone_state_t state;
    char name[WX_PHONE_NAME_MAX + 1];    // 型号名称,如 "iPhone XR";空串表示还没读到
    uint16_t total;                      // 通知中心里所有应用的通知数
    size_t chats;                        // 其中要提醒的聊天通知数
} wx_ui_phone_t;

typedef struct {
    wx_lang_t lang;
    wx_ble_stack_t stack;
    wx_ui_phone_t phones[WX_BLE_MAX_PHONES];
    bool show_passkey;
    uint32_t passkey;

    bool alerting;
    uint16_t unseen;
    uint16_t unseen_app[WX_APP_COUNT];
    uint8_t alert_app;       // 最近一条新消息来自哪个应用(wx_app_t)
    uint32_t alert_seq;      // 每来一条新消息 +1;变化时重新开始闪烁
    char sender[WX_ANCS_TITLE_MAX + 1];        // 最近一条新消息的好友名或群名,空串表示未知
    char sender_phone[WX_PHONE_NAME_MAX + 1];  // 它来自哪台手机

    wx_net_status_t net;
    bool has_wifi;           // 保存了 Wi-Fi 网络
    bool tg_ready;           // Telegram 已设置好(Token 与聊天)
    bool tg_failing;         // 有转发在等重试

    bool menu;               // 设置菜单开着
    uint8_t menu_index;      // 选中的菜单项(wx_menu_item_t)
    wx_confirm_t confirm;    // 正在确认的操作;NONE 表示在菜单列表
    bool confirm_yes;        // 确认页选中了“确定清除”;默认是“取消”

    bool muted;
    int battery;             // 0..100,-1 表示不可用
    const char *toast;       // 底部短暂提示,NULL 表示无
} wx_ui_model_t;

// 两个函数都必须在持有 bsp_lvgl_lock() 时调用。
esp_err_t wx_ui_init(void);
void wx_ui_render(const wx_ui_model_t *model);
