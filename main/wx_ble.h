// main/wx_ble.h —— NimBLE 外设 + ANCS 客户端,最多同时连接 WX_BLE_MAX_PHONES 台 iPhone。
// 每台手机绑定配对后订阅它的全部系统通知,向应用报告:聊天应用(微信、信息、Telegram、QQ,
// 见 wx_apps.h)通知的新增、所有通知的移除、通知中心的总数,以及手机型号名称。
#pragma once

#include "esp_err.h"
#include "wx_ancs_proto.h"
#include "wx_apps.h"
#include "wx_phone.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 蓝牙名形如 "Passport Alert 1A2B",后缀取芯片出厂 MAC 的末两字节,每台不同;
// 配网热点名用同一个后缀。
#define WX_BLE_DEVICE_NAME_MAX sizeof("Passport Alert XXXX")
// 与 CONFIG_BT_NIMBLE_MAX_CONNECTIONS 一致(wx_ble.c 里有静态检查)。
#define WX_BLE_MAX_PHONES 3

typedef enum {
    WX_BLE_STACK_STARTING = 0,
    WX_BLE_STACK_RUNNING,       // 协议栈正常;未满 WX_BLE_MAX_PHONES 台时持续广播
    WX_BLE_STACK_FAILED,
} wx_ble_stack_t;

typedef enum {
    WX_PHONE_OFF = 0,           // 这个槽位没有手机
    WX_PHONE_CONNECTED,         // 已连接,正在加密/配对
    WX_PHONE_DISCOVERING,       // 已加密,正在查找并订阅 ANCS
    WX_PHONE_READY,             // 正在接收通知
    WX_PHONE_NO_ANCS,           // 已连接但手机未开放通知服务(未允许共享通知)
} wx_phone_state_t;

typedef enum {
    WX_BLE_EV_STACK = 0,        // data.stack
    WX_BLE_EV_PHONE_STATE,      // phone, data.phone_state
    WX_BLE_EV_PHONE_NAME,       // phone, data.name:型号名称,如 "iPhone XR"
    WX_BLE_EV_PASSKEY,          // phone, data.passkey:需在该 iPhone 上输入的 6 位配对码
    WX_BLE_EV_NOTIF_ADDED,      // phone, data.added:聊天应用的一条通知
    WX_BLE_EV_NOTIF_REMOVED,    // phone, data.removed_uid(任何应用的通知)
    WX_BLE_EV_NOTIF_TOTAL,      // phone, data.total:该手机通知中心里所有应用的通知数
    WX_BLE_EV_BONDS_CLEARED,    // data.status:ble_store_clear() 返回值
} wx_ble_event_type_t;

typedef struct {
    wx_ble_event_type_t type;
    uint8_t phone;              // 手机槽位 0..WX_BLE_MAX_PHONES-1,按连接分配,断开后可复用
    union {
        wx_ble_stack_t stack;
        wx_phone_state_t phone_state;
        char name[WX_PHONE_NAME_MAX + 1];
        uint32_t passkey;
        struct {
            uint32_t uid;
            uint8_t app;                         // wx_app_t
            bool pre_existing;
            char title[WX_ANCS_TITLE_MAX + 1];   // 好友名或群名,UTF-8;关闭消息详情时是应用名
            // 正文,UTF-8。只在回调期间有效,要保留须自行复制。已有通知不查正文,为空串。
            const char *message;
        } added;
        uint32_t removed_uid;
        uint16_t total;
        int status;
    } data;
} wx_ble_event_t;

// 回调运行在 NimBLE host 任务:只能入队或做同等级的有界操作,不得阻塞或访问 LVGL。
typedef void (*wx_ble_event_cb_t)(const wx_ble_event_t *event, void *ctx);

// 写出本机的蓝牙名(buf 至少 WX_BLE_DEVICE_NAME_MAX 字节)。可在任意任务调用。
void wx_ble_device_name(char *buf, size_t cap);

// 启动协议栈并开始广播。需先完成 nvs_flash_init(),否则配对信息无法保存。
esp_err_t wx_ble_start(wx_ble_event_cb_t cb, void *ctx);

// 删除所有绑定并断开所有手机,随后重新广播等待配对。可在任意任务调用,异步执行。
void wx_ble_forget_phones(void);
