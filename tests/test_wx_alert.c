// 提醒状态机的 host 测试:不依赖 SDK,直接验证 wx_alert.c 的纯逻辑。
#include <assert.h>
#include <stdio.h>

#include "wx_alert.h"

static void test_new_message_raises_and_ack_clears(void) {
    wx_alert_t a;
    wx_alert_init(&a);

    assert(wx_alert_add(&a, 0, 1, WX_APP_WECHAT, false) == WX_ALERT_RAISED);
    assert(a.alerting && wx_alert_unseen(&a) == 1);
    assert(wx_alert_add(&a, 0, 2, WX_APP_WECHAT, false) == WX_ALERT_RAISED);   // 每条新消息都提醒
    assert(wx_alert_unseen(&a) == 2);
    assert(wx_alert_add(&a, 0, 2, WX_APP_WECHAT, false) == WX_ALERT_NO_CHANGE);

    assert(wx_alert_acknowledge(&a) == WX_ALERT_CLEARED);
    assert(!a.alerting && wx_alert_unseen(&a) == 0);
    assert(wx_alert_tracked(&a, 0) == 2);                        // 手机上的通知仍在
    assert(wx_alert_acknowledge(&a) == WX_ALERT_NO_CHANGE);

    // 已确认的旧通知被移除,不会重新触发或清除什么。
    assert(wx_alert_remove(&a, 0, 1) == WX_ALERT_UPDATED);
    assert(!a.alerting);
}

static void test_pre_existing_counts_without_alert(void) {
    wx_alert_t a;
    wx_alert_init(&a);

    assert(wx_alert_add(&a, 0, 5, WX_APP_WECHAT, true) == WX_ALERT_UPDATED);
    assert(!a.alerting && wx_alert_unseen(&a) == 0 && wx_alert_tracked(&a, 0) == 1);
    assert(wx_alert_remove(&a, 0, 99) == WX_ALERT_NO_CHANGE);   // 非微信通知
}

static void test_reading_on_phone_clears_only_new_messages(void) {
    wx_alert_t a;
    wx_alert_init(&a);

    // 手机上本来就有两条旧微信通知,又来一条新的。
    assert(wx_alert_add(&a, 0, 1, WX_APP_WECHAT, true) == WX_ALERT_UPDATED);
    assert(wx_alert_add(&a, 0, 2, WX_APP_WECHAT, true) == WX_ALERT_UPDATED);
    assert(wx_alert_add(&a, 0, 3, WX_APP_WECHAT, false) == WX_ALERT_RAISED);
    assert(wx_alert_unseen(&a) == 1);

    // 删掉一条旧通知不影响提醒。
    assert(wx_alert_remove(&a, 0, 1) == WX_ALERT_UPDATED);
    assert(a.alerting);
    // 在手机上读了新消息:提醒自动结束,即使旧通知还在。
    assert(wx_alert_remove(&a, 0, 3) == WX_ALERT_CLEARED);
    assert(!a.alerting && wx_alert_unseen(&a) == 0 && wx_alert_tracked(&a, 0) == 1);
}

static void test_two_phones_are_independent(void) {
    wx_alert_t a;
    wx_alert_init(&a);

    // 两台手机的 UID 可能相同,互不影响。
    assert(wx_alert_add(&a, 0, 7, WX_APP_WECHAT, false) == WX_ALERT_RAISED);
    assert(wx_alert_add(&a, 1, 7, WX_APP_WECHAT, false) == WX_ALERT_RAISED);
    assert(wx_alert_unseen(&a) == 2);
    assert(wx_alert_tracked(&a, 0) == 1 && wx_alert_tracked(&a, 1) == 1);

    // 在第二台手机上读掉:提醒还在,因为第一台的新消息没读。
    assert(wx_alert_remove(&a, 1, 7) == WX_ALERT_UPDATED);
    assert(a.alerting && wx_alert_unseen(&a) == 1);
    assert(wx_alert_remove(&a, 1, 7) == WX_ALERT_NO_CHANGE);
    // 第一台也读掉:提醒结束。
    assert(wx_alert_remove(&a, 0, 7) == WX_ALERT_CLEARED);

    // 一台手机断开只影响它自己的记录。
    assert(wx_alert_add(&a, 0, 1, WX_APP_WECHAT, true) == WX_ALERT_UPDATED);
    assert(wx_alert_add(&a, 1, 2, WX_APP_WECHAT, false) == WX_ALERT_RAISED);
    wx_alert_link_lost(&a, 0);
    assert(wx_alert_tracked(&a, 0) == 0 && wx_alert_tracked(&a, 1) == 1);
    assert(a.alerting && wx_alert_unseen(&a) == 1);
}

static void test_link_loss_keeps_unacknowledged_alert(void) {
    wx_alert_t a;
    wx_alert_init(&a);

    assert(wx_alert_add(&a, 0, 1, WX_APP_WECHAT, false) == WX_ALERT_RAISED);
    assert(wx_alert_add(&a, 0, 2, WX_APP_WECHAT, false) == WX_ALERT_RAISED);
    wx_alert_link_lost(&a, 0);
    assert(wx_alert_tracked(&a, 0) == 0 && a.alerting && wx_alert_unseen(&a) == 2);

    // 重连后手机补发的已有通知只计数;移除它们也不能自动清掉断线前的提醒。
    assert(wx_alert_add(&a, 0, 7, WX_APP_WECHAT, true) == WX_ALERT_UPDATED);
    assert(wx_alert_remove(&a, 0, 7) == WX_ALERT_UPDATED);
    assert(a.alerting && wx_alert_unseen(&a) == 2);
    assert(wx_alert_add(&a, 0, 8, WX_APP_WECHAT, false) == WX_ALERT_RAISED);
    assert(wx_alert_unseen(&a) == 3);
    assert(wx_alert_remove(&a, 0, 8) == WX_ALERT_UPDATED);      // 仍有断线前的未确认消息
    assert(a.alerting && wx_alert_unseen(&a) == 2);
    assert(wx_alert_acknowledge(&a) == WX_ALERT_CLEARED);
    assert(wx_alert_unseen(&a) == 0);
}

static void test_tracking_overflow_keeps_count(void) {
    wx_alert_t a;
    wx_alert_init(&a);

    for (uint32_t uid = 1; uid <= WX_ALERT_TRACK_CAP + 3; uid++) {
        assert(wx_alert_add(&a, 0, uid, WX_APP_WECHAT, false) == WX_ALERT_RAISED);
    }
    assert(a.tracked == WX_ALERT_TRACK_CAP);
    assert(wx_alert_unseen(&a) == WX_ALERT_TRACK_CAP + 3);
    assert(wx_alert_remove(&a, 0, 1) == WX_ALERT_NO_CHANGE);    // 最早的已被挤出

    // 计数封顶。
    for (uint32_t uid = 1000; uid < 2100; uid++) (void)wx_alert_add(&a, 0, uid, WX_APP_WECHAT, false);
    assert(wx_alert_unseen(&a) == WX_ALERT_UNSEEN_MAX);
}

static void test_apps_counted_separately(void) {
    wx_alert_t a;
    wx_alert_init(&a);

    assert(wx_alert_add(&a, 0, 1, WX_APP_WECHAT, false) == WX_ALERT_RAISED);
    assert(wx_alert_add(&a, 0, 2, WX_APP_QQ, false) == WX_ALERT_RAISED);
    assert(wx_alert_add(&a, 1, 3, WX_APP_QQ, false) == WX_ALERT_RAISED);
    assert(wx_alert_add(&a, 1, 4, WX_APP_TELEGRAM, true) == WX_ALERT_UPDATED);
    assert(wx_alert_add(&a, 0, 9, WX_APP_UNKNOWN, false) == WX_ALERT_NO_CHANGE);   // 不提醒的应用
    assert(wx_alert_unseen(&a) == 3);
    assert(wx_alert_unseen_app(&a, WX_APP_WECHAT) == 1);
    assert(wx_alert_unseen_app(&a, WX_APP_QQ) == 2);
    assert(wx_alert_unseen_app(&a, WX_APP_TELEGRAM) == 0);       // 已有通知不算新消息
    assert(wx_alert_unseen_app(&a, WX_APP_UNKNOWN) == 0);
    assert(wx_alert_tracked(&a, 1) == 2);

    // 断线后遗留计数仍按应用分开。
    wx_alert_link_lost(&a, 1);
    assert(wx_alert_unseen_app(&a, WX_APP_QQ) == 2 && wx_alert_unseen(&a) == 3);
    assert(wx_alert_remove(&a, 0, 2) == WX_ALERT_UPDATED);
    assert(wx_alert_unseen_app(&a, WX_APP_QQ) == 1);
    assert(wx_alert_acknowledge(&a) == WX_ALERT_CLEARED);
    for (uint8_t app = 0; app < WX_APP_COUNT; app++) assert(wx_alert_unseen_app(&a, app) == 0);
}

int main(void) {
    test_new_message_raises_and_ack_clears();
    test_pre_existing_counts_without_alert();
    test_reading_on_phone_clears_only_new_messages();
    test_two_phones_are_independent();
    test_link_loss_keeps_unacknowledged_alert();
    test_tracking_overflow_keeps_count();
    test_apps_counted_separately();
    printf("test_wx_alert: PASS\n");
    return 0;
}
