// main/wx_alert.h —— 提醒状态:跟踪各台 iPhone 通知中心里要提醒的聊天通知(微信、信息、
// Telegram、QQ),决定何时提醒、何时清除。
// 纯逻辑,不依赖 ESP-IDF / LVGL,由 tests/test_wx_alert.c 覆盖。
// ANCS 的通知 UID 只在一台手机的一次连接内唯一,所以以 (手机, UID) 作为键。
#pragma once

#include "wx_apps.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 同时跟踪的聊天通知上限(所有手机合计);超出时丢弃最早的一条。
#define WX_ALERT_TRACK_CAP 64
#define WX_ALERT_UNSEEN_MAX 999

typedef struct {
    uint8_t phone;
    uint8_t app;        // wx_app_t
    uint32_t uid;
    bool fresh;         // 上次清除提醒后新到的
} wx_alert_entry_t;

typedef struct {
    wx_alert_entry_t entries[WX_ALERT_TRACK_CAP];   // 手机通知中心里仍存在的聊天通知
    size_t tracked;
    uint16_t carried[WX_APP_COUNT];   // 已无法跟踪(断线或溢出)但尚未确认的新消息数
    bool alerting;
} wx_alert_t;

typedef enum {
    WX_ALERT_NO_CHANGE = 0,
    WX_ALERT_RAISED,    // 新消息到达:需要闪屏和提示音
    WX_ALERT_UPDATED,   // 计数变化,无需提示音
    WX_ALERT_CLEARED,   // 提醒结束(按键清除,或新消息在手机上都已读掉)
} wx_alert_change_t;

void wx_alert_init(wx_alert_t *alert);

// pre_existing:手机刚连上时补发的已有通知,只计数不提醒。app 超出范围时忽略。
wx_alert_change_t wx_alert_add(wx_alert_t *alert, uint8_t phone, uint32_t uid, uint8_t app,
                               bool pre_existing);

// 手机通知中心移除了一条通知(例如在手机上读了消息)。没在跟踪的 UID 会被忽略。
wx_alert_change_t wx_alert_remove(wx_alert_t *alert, uint8_t phone, uint32_t uid);

// 用户在板子上按键确认。
wx_alert_change_t wx_alert_acknowledge(wx_alert_t *alert);

// 某台手机断开:丢掉它的跟踪记录,但保留它未确认的提醒。
void wx_alert_link_lost(wx_alert_t *alert, uint8_t phone);

// 未确认的新消息数(所有手机、所有应用合计),最大 WX_ALERT_UNSEEN_MAX。
uint16_t wx_alert_unseen(const wx_alert_t *alert);

// 某个应用未确认的新消息数(所有手机合计),最大 WX_ALERT_UNSEEN_MAX。
uint16_t wx_alert_unseen_app(const wx_alert_t *alert, uint8_t app);

// 某台手机通知中心里的聊天通知数。
size_t wx_alert_tracked(const wx_alert_t *alert, uint8_t phone);
