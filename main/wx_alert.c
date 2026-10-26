// main/wx_alert.c —— 提醒状态(纯逻辑)。
#include "wx_alert.h"

#include <string.h>

void wx_alert_init(wx_alert_t *alert)
{
    memset(alert, 0, sizeof(*alert));
}

static bool find_entry(const wx_alert_t *alert, uint8_t phone, uint32_t uid, size_t *index)
{
    for (size_t i = 0; i < alert->tracked; i++) {
        if (alert->entries[i].phone == phone && alert->entries[i].uid == uid) {
            *index = i;
            return true;
        }
    }
    return false;
}

static void delete_at(wx_alert_t *alert, size_t index)
{
    memmove(&alert->entries[index], &alert->entries[index + 1],
            (alert->tracked - index - 1) * sizeof(alert->entries[0]));
    alert->tracked--;
}

static uint16_t clamp_unseen(unsigned total)
{
    return (uint16_t)(total > WX_ALERT_UNSEEN_MAX ? WX_ALERT_UNSEEN_MAX : total);
}

// 一条还没确认的通知不再跟踪了(断线或溢出):把它记进遗留计数。
static void carry(wx_alert_t *alert, const wx_alert_entry_t *entry)
{
    if (entry->fresh) alert->carried[entry->app] = clamp_unseen(alert->carried[entry->app] + 1u);
}

uint16_t wx_alert_unseen_app(const wx_alert_t *alert, uint8_t app)
{
    if (app >= WX_APP_COUNT) return 0;
    unsigned total = alert->carried[app];
    for (size_t i = 0; i < alert->tracked; i++) {
        if (alert->entries[i].fresh && alert->entries[i].app == app) total++;
    }
    return clamp_unseen(total);
}

uint16_t wx_alert_unseen(const wx_alert_t *alert)
{
    unsigned total = 0;
    for (uint8_t app = 0; app < WX_APP_COUNT; app++) total += wx_alert_unseen_app(alert, app);
    return clamp_unseen(total);
}

size_t wx_alert_tracked(const wx_alert_t *alert, uint8_t phone)
{
    size_t n = 0;
    for (size_t i = 0; i < alert->tracked; i++) {
        if (alert->entries[i].phone == phone) n++;
    }
    return n;
}

wx_alert_change_t wx_alert_add(wx_alert_t *alert, uint8_t phone, uint32_t uid, uint8_t app,
                               bool pre_existing)
{
    size_t index;
    if (app >= WX_APP_COUNT || find_entry(alert, phone, uid, &index)) return WX_ALERT_NO_CHANGE;

    if (alert->tracked == WX_ALERT_TRACK_CAP) {
        carry(alert, &alert->entries[0]);
        delete_at(alert, 0);
    }
    alert->entries[alert->tracked++] = (wx_alert_entry_t){
        .phone = phone, .app = app, .uid = uid, .fresh = !pre_existing,
    };

    if (pre_existing) return WX_ALERT_UPDATED;
    alert->alerting = true;
    return WX_ALERT_RAISED;
}

wx_alert_change_t wx_alert_remove(wx_alert_t *alert, uint8_t phone, uint32_t uid)
{
    size_t index;
    if (!find_entry(alert, phone, uid, &index)) return WX_ALERT_NO_CHANGE;
    const bool was_fresh = alert->entries[index].fresh;
    delete_at(alert, index);

    if (was_fresh && alert->alerting && wx_alert_unseen(alert) == 0) {
        alert->alerting = false;
        return WX_ALERT_CLEARED;
    }
    return WX_ALERT_UPDATED;
}

wx_alert_change_t wx_alert_acknowledge(wx_alert_t *alert)
{
    if (!alert->alerting && wx_alert_unseen(alert) == 0) return WX_ALERT_NO_CHANGE;
    for (size_t i = 0; i < alert->tracked; i++) alert->entries[i].fresh = false;
    memset(alert->carried, 0, sizeof(alert->carried));
    alert->alerting = false;
    return WX_ALERT_CLEARED;
}

void wx_alert_link_lost(wx_alert_t *alert, uint8_t phone)
{
    size_t i = 0;
    while (i < alert->tracked) {
        if (alert->entries[i].phone == phone) {
            carry(alert, &alert->entries[i]);
            delete_at(alert, i);
        } else {
            i++;
        }
    }
}
