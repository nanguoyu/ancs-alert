// main/main.c —— 消息提醒器:通过 ANCS 接收最多 3 台 iPhone 的通知;微信、信息、Telegram、QQ、
// WhatsApp、Signal、LINE 的新消息用整屏闪烁和人声播报醒目提醒,并可经 Wi-Fi 转发到 Telegram 机器人。
// 界面、网页、转发文字和人声都有英文与中文两种,在设置菜单或设置网页里切换。
//
// 按键(只用单击;长按不做任何事,免得按错键就清掉了配对):
//   配网中   任意键       退出配网
//   提醒中   任意键       清除提醒
//   待机时   确定         打开设置菜单
//            上           提示音 开 / 关(保存在 NVS)
//            下           试听提示音(每按一次换一个应用)
//   菜单里   上 / 下      选择,确定键进入。菜单项:提示音、试听、Wi-Fi 与转发设置(配网热点)、
//                         语言、清除 Wi-Fi 网络、清除 iPhone 配对、返回。两个清除都要在确认页再选一次
//                         “确定清除”,确认页默认停在“取消”。30 秒没操作自动关闭菜单。
//
// 在 iPhone 上读掉新消息(通知从通知中心消失)后,提醒也会自动结束。
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "wx_alert.h"
#include "wx_apps.h"
#include "wx_ble.h"
#include "wx_net.h"
#include "wx_portal.h"
#include "wx_settings.h"
#include "wx_sound.h"
#include "wx_tg.h"
#include "wx_ui.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "main";

#define APP_QUEUE_DEPTH      48
#define APP_TICK_MS          500
#define BATTERY_PERIOD_US    (30LL * 1000 * 1000)
#define HEAP_LOG_PERIOD_US   (10LL * 60 * 1000 * 1000)
#define SETUP_TIMEOUT_US     (15LL * 60 * 1000 * 1000)
#define MENU_TIMEOUT_US      (30LL * 1000 * 1000)
#define TOAST_US             (2500LL * 1000)
#define BACKLIGHT_IDLE       40
#define BACKLIGHT_ALERT      100

typedef enum {
    APP_EV_KEY = 0,
    APP_EV_BLE,
    APP_EV_NET,          // Wi-Fi 状态有变化
    APP_EV_TG,           // 转发统计有变化
    APP_EV_SETUP_DONE,   // 网页上点了“完成”
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    union {
        struct {
            bsp_btn_t btn;
            bsp_btn_ev_t event;
        } key;
        struct {
            wx_ble_event_t event;
            char *message;   // NOTIF_ADDED 的正文副本(要转发时才有),由应用任务释放
        } ble;
    } data;
} app_event_t;

static QueueHandle_t s_queue;
static volatile bool s_input_ready;
static bool s_sound_ok;
static bool s_battery_ok;

static wx_alert_t s_alert;
static wx_ui_model_t s_model;
static int s_passkey_phone = -1;   // 正在配对的手机槽位
static bool s_portal_running;
static volatile bool s_portal_stopping;   // 停止网页的辅助任务还在运行
static int64_t s_setup_until;
static int64_t s_menu_until;
static uint8_t s_test_app;
static char s_toast_buf[48];
static int64_t s_toast_until;
static int64_t s_next_battery;
static int64_t s_next_heap_log;
static int s_backlight = -1;

// ---------------------------------------------------------------------------
// 应用任务:唯一修改 s_alert / s_model 并刷新界面的地方
// ---------------------------------------------------------------------------
static void show_toast(const char *text)
{
    s_model.toast = text;
    s_toast_until = esp_timer_get_time() + TOAST_US;
}

static void refresh(void)
{
    s_model.alerting = s_alert.alerting;
    s_model.unseen = wx_alert_unseen(&s_alert);
    for (uint8_t app = 0; app < WX_APP_COUNT; app++) {
        s_model.unseen_app[app] = wx_alert_unseen_app(&s_alert, app);
    }
    for (uint8_t i = 0; i < WX_BLE_MAX_PHONES; i++) {
        s_model.phones[i].chats = wx_alert_tracked(&s_alert, i);
    }
    s_model.has_wifi = wx_settings_has_wifi();
    s_model.tg_ready = wx_settings_tg_ready();
    s_model.lang = wx_lang();   // 设置网页也能改语言

    const bool lit = s_model.alerting || s_model.net.setup;
    const int backlight = lit ? BACKLIGHT_ALERT : BACKLIGHT_IDLE;
    if (backlight != s_backlight) {
        bsp_display_backlight((uint8_t)backlight);
        s_backlight = backlight;
    }
    if (bsp_lvgl_lock(500)) {
        wx_ui_render(&s_model);
        bsp_lvgl_unlock();
    }
}

static size_t connected_phones(void)
{
    size_t n = 0;
    for (size_t i = 0; i < WX_BLE_MAX_PHONES; i++) {
        if (s_model.phones[i].state != WX_PHONE_OFF) n++;
    }
    return n;
}

static void on_portal_done(void *ctx);
static void post_simple(app_event_type_t type);

static void enter_setup(void)
{
    if (s_model.net.setup || s_setup_until) return;
    if (s_portal_stopping) {
        show_toast(WX_TR("Still closing, wait", "上次的设置还在关闭"));
        return;
    }
    ESP_LOGI(TAG, "打开配网");
    s_setup_until = esp_timer_get_time() + SETUP_TIMEOUT_US;
    show_toast(WX_TR("Opening Wi-Fi setup…", "正在打开 Wi-Fi 设置…"));
    wx_net_setup_begin();   // 热点打开后经 APP_EV_NET 启动网页
}

// 停止网页要等正在处理的请求结束(可能在等 Telegram,长达几十秒),放到辅助任务里做,
// 应用任务照常处理通知和按键。网页停了才关热点。
static void portal_stop_task(void *arg)
{
    (void)arg;
    wx_portal_stop();
    wx_net_setup_end();
    s_portal_stopping = false;
    post_simple(APP_EV_NET);
    vTaskDelete(NULL);
}

static void leave_setup(const char *why)
{
    if (!s_setup_until && !s_portal_running && !s_model.net.setup) return;
    ESP_LOGI(TAG, "退出配网:%s", why);
    s_setup_until = 0;
    s_model.net.setup = false;
    if (s_portal_running) {
        s_portal_running = false;
        s_portal_stopping = true;
        if (xTaskCreate(portal_stop_task, "wx_stop", 3072, NULL, 4, NULL) != pdPASS) {
            wx_portal_stop();
            wx_net_setup_end();
            s_portal_stopping = false;
        }
    } else {
        wx_net_setup_end();
    }
    show_toast(WX_TR("Wi-Fi setup closed", "已退出 Wi-Fi 设置"));
}

static void toggle_mute(void)
{
    s_model.muted = !s_model.muted;
    (void)wx_settings_set_muted(s_model.muted);
    show_toast(s_model.muted ? WX_TR("Sound off", "提示音已关") : WX_TR("Sound on", "提示音已开"));
}

static void test_sound(void)
{
    if (!s_sound_ok) {
        show_toast(WX_TR("Speaker not available", "喇叭不可用"));
        return;
    }
    wx_sound_play(s_test_app, s_model.lang);
    snprintf(s_toast_buf, sizeof(s_toast_buf), WX_TR("Test: %s", "试听：%s"), wx_app_name(s_test_app, s_model.lang));
    show_toast(s_toast_buf);
    s_test_app = (uint8_t)((s_test_app + 1) % WX_APP_COUNT);
}

static void open_menu(void)
{
    s_model.menu = true;
    s_model.menu_index = 0;
    s_model.confirm = WX_CONFIRM_NONE;
    s_model.confirm_yes = false;
    s_menu_until = esp_timer_get_time() + MENU_TIMEOUT_US;
}

static void close_menu(void)
{
    s_model.menu = false;
    s_model.confirm = WX_CONFIRM_NONE;
    s_menu_until = 0;
}

static void run_confirmed(wx_confirm_t what)
{
    if (what == WX_CONFIRM_FORGET_PHONES) {
        ESP_LOGI(TAG, "菜单:清除 iPhone 配对");
        show_toast(WX_TR("Forgetting iPhones…", "正在清除配对…"));
        wx_ble_forget_phones();
    } else if (what == WX_CONFIRM_CLEAR_WIFI) {
        ESP_LOGI(TAG, "菜单:清除 Wi-Fi 网络");
        if (wx_settings_wifi_clear() == ESP_OK) {
            wx_net_reload(NULL);
            show_toast(WX_TR("Wi-Fi networks cleared", "Wi-Fi 网络已清除"));
        } else {
            show_toast(WX_TR("Could not clear", "清除失败"));
        }
    }
}

static void toggle_language(void)
{
    s_model.lang = s_model.lang == WX_LANG_ZH ? WX_LANG_EN : WX_LANG_ZH;
    (void)wx_settings_set_lang(s_model.lang);
    ESP_LOGI(TAG, "界面语言改为 %s", wx_lang_code(s_model.lang));
}

static void menu_activate(void)
{
    switch (s_model.menu_index) {
    case WX_MENU_SOUND:
        toggle_mute();
        break;
    case WX_MENU_TEST:
        test_sound();
        break;
    case WX_MENU_SETUP:
        close_menu();
        enter_setup();
        break;
    case WX_MENU_LANGUAGE:
        toggle_language();
        break;
    case WX_MENU_CLEAR_WIFI:
        if (!wx_settings_has_wifi()) {
            show_toast(WX_TR("No saved Wi-Fi yet", "还没有保存的 Wi-Fi"));
            break;
        }
        s_model.confirm = WX_CONFIRM_CLEAR_WIFI;
        s_model.confirm_yes = false;
        break;
    case WX_MENU_FORGET_PHONES:
        s_model.confirm = WX_CONFIRM_FORGET_PHONES;
        s_model.confirm_yes = false;
        break;
    case WX_MENU_BACK:
    default:
        close_menu();
        break;
    }
}

static void handle_menu_key(bsp_btn_t btn)
{
    s_menu_until = esp_timer_get_time() + MENU_TIMEOUT_US;
    if (s_model.confirm != WX_CONFIRM_NONE) {
        // 确认页:上下键在“取消”和“确定清除”之间切换;确定键执行选中的那个。
        if (btn != BSP_BTN_OK) {
            s_model.confirm_yes = !s_model.confirm_yes;
            return;
        }
        const wx_confirm_t what = s_model.confirm;
        s_model.confirm = WX_CONFIRM_NONE;
        if (s_model.confirm_yes) {
            close_menu();
            run_confirmed(what);
        }
        return;
    }
    switch (btn) {
    case BSP_BTN_UP:
        s_model.menu_index = (uint8_t)((s_model.menu_index + WX_MENU_COUNT - 1) % WX_MENU_COUNT);
        break;
    case BSP_BTN_DOWN:
        s_model.menu_index = (uint8_t)((s_model.menu_index + 1) % WX_MENU_COUNT);
        break;
    case BSP_BTN_OK:
        menu_activate();
        break;
    }
}

static void handle_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (s_model.net.setup || s_setup_until) {
        if (event == BSP_BTN_CLICK) leave_setup("按键");
        return;
    }
    if (event == BSP_BTN_LONG) {
        // 长按没有功能:以前长按会清配对或开配网,现在都在设置菜单里。
        if (!s_model.menu && !s_alert.alerting) show_toast(WX_TR("Press OK for settings", "按确定键打开设置菜单"));
        return;
    }
    if (s_alert.alerting) {
        (void)wx_alert_acknowledge(&s_alert);
        return;
    }
    if (s_model.menu) {
        handle_menu_key(btn);
        return;
    }
    switch (btn) {
    case BSP_BTN_OK:
        open_menu();
        break;
    case BSP_BTN_UP:
        toggle_mute();
        break;
    case BSP_BTN_DOWN:
        test_sound();
        break;
    }
}

// 我们转发到 Telegram 的消息,会作为机器人发来的 Telegram 通知回到 iPhone 上:不再提醒。
static bool from_our_bot(uint8_t app, const char *title)
{
    if (app != WX_APP_TELEGRAM) return false;
    static wx_tg_config_t config;   // 只在应用任务里使用
    wx_settings_tg(&config);
    const bool ours = config.bot_name[0] && strcmp(title, config.bot_name) == 0;
    memset(config.token, 0, sizeof(config.token));
    return ours;
}

static void handle_notif_added(uint8_t i, const wx_ble_event_t *ev, char *message)
{
    const uint8_t app = ev->data.added.app;
    const char *title = ev->data.added.title;
    const bool pre_existing = ev->data.added.pre_existing;
    if (from_our_bot(app, title)) {
        free(message);
        return;
    }

    if (wx_settings_alert_mask() & WX_APP_BIT(app)) {
        if (wx_alert_add(&s_alert, i, ev->data.added.uid, app, pre_existing) == WX_ALERT_RAISED) {
            close_menu();   // 新消息优先:按键先用来清除提醒,不会误操作菜单
            s_model.alert_seq++;
            s_model.alert_app = app;
            memcpy(s_model.sender, title, sizeof(s_model.sender));
            memcpy(s_model.sender_phone, s_model.phones[i].name, sizeof(s_model.sender_phone));
            if (!s_model.muted && s_sound_ok) wx_sound_play(app, s_model.lang);
        }
    }

    // 连上时补发的已有通知不转发,免得每次重连都把旧消息再发一遍。
    if (!pre_existing && wx_settings_forwards(app)) {
        const char *phone = connected_phones() > 1 && s_model.phones[i].name[0]
            ? wx_phone_short_name(s_model.phones[i].name) : "";
        wx_tg_forward(app, phone, title, message);   // 接管 message
        return;
    }
    free(message);
}

static void handle_ble(const wx_ble_event_t *ev, char *message)
{
    if (ev->phone >= WX_BLE_MAX_PHONES) {
        free(message);
        return;
    }
    const uint8_t i = ev->phone;
    wx_ui_phone_t *phone = &s_model.phones[i];

    switch (ev->type) {
    case WX_BLE_EV_STACK:
        s_model.stack = ev->data.stack;
        break;
    case WX_BLE_EV_PHONE_STATE:
        phone->state = ev->data.phone_state;
        if (phone->state == WX_PHONE_OFF) {
            wx_alert_link_lost(&s_alert, i);
            phone->total = 0;
            phone->name[0] = '\0';
        }
        if (phone->state != WX_PHONE_CONNECTED && s_passkey_phone == i) {
            s_model.show_passkey = false;
            s_passkey_phone = -1;
        }
        break;
    case WX_BLE_EV_PHONE_NAME:
        memcpy(phone->name, ev->data.name, sizeof(phone->name));
        break;
    case WX_BLE_EV_NOTIF_TOTAL:
        phone->total = ev->data.total;
        break;
    case WX_BLE_EV_PASSKEY:
        s_model.passkey = ev->data.passkey;
        s_model.show_passkey = true;
        s_passkey_phone = i;
        break;
    case WX_BLE_EV_NOTIF_ADDED:
        handle_notif_added(i, ev, message);
        message = NULL;
        break;
    case WX_BLE_EV_NOTIF_REMOVED:
        (void)wx_alert_remove(&s_alert, i, ev->data.removed_uid);
        break;
    case WX_BLE_EV_BONDS_CLEARED:
        show_toast(ev->data.status == 0 ? WX_TR("Pairings cleared", "配对已清除")
                                        : WX_TR("Could not clear pairings", "清除配对失败"));
        break;
    }
    free(message);
}

static void handle_net(void)
{
    wx_net_status(&s_model.net);
    if (s_model.net.setup && !s_setup_until) {
        // 已经退出配网,但热点才打开(打开途中就按了退出),或者关热点的命令还在排队:
        // 不显示配网页,也不启动网页;再发一次关闭(重复关闭无害)。
        s_model.net.setup = false;
        if (!s_portal_stopping) wx_net_setup_end();
    } else if (s_model.net.setup && !s_portal_running) {
        if (wx_portal_start(on_portal_done, NULL) == ESP_OK) {
            s_portal_running = true;
        } else {
            show_toast(WX_TR("Setup page didn't start", "设置网页启动失败"));
        }
    }
    if (s_setup_until && s_model.net.setup_failed) {
        // 热点没能打开(原因见 wx_net 日志)。
        s_setup_until = 0;
        wx_net_setup_end();
        show_toast(WX_TR("Wi-Fi setup failed", "Wi-Fi 设置打不开"));
    }
}

static void handle_tg(void)
{
    wx_tg_stats_t stats;
    wx_tg_stats(&stats);
    s_model.tg_failing = stats.pending > 0 && stats.last_error != NULL;
}

static void app_task(void *arg)
{
    (void)arg;
    app_event_t ev;
    for (;;) {
        bool changed = false;
        TickType_t wait = pdMS_TO_TICKS(APP_TICK_MS);
        // 刚连上时手机会一口气补发几十条通知:先把积压的事件处理完,再刷新一次界面
        // (每批最多一个队列长度,避免事件源源不断时界面一直不刷新)。
        int batch = 0;
        while (batch++ < APP_QUEUE_DEPTH && xQueueReceive(s_queue, &ev, wait) == pdTRUE) {
            switch (ev.type) {
            case APP_EV_KEY:
                handle_key(ev.data.key.btn, ev.data.key.event);
                break;
            case APP_EV_BLE:
                handle_ble(&ev.data.ble.event, ev.data.ble.message);
                break;
            case APP_EV_NET:
                handle_net();
                break;
            case APP_EV_TG:
                handle_tg();
                break;
            case APP_EV_SETUP_DONE:
                leave_setup("网页上点了完成");
                break;
            }
            changed = true;
            wait = 0;
        }

        const int64_t now = esp_timer_get_time();
        if (s_model.menu && now >= s_menu_until) {
            close_menu();
            changed = true;
        }
        if (s_setup_until && now >= s_setup_until) {
            leave_setup("超时");
            changed = true;
        }
        if (wx_lang() != s_model.lang) {
            changed = true;     // 在设置网页上换了语言;refresh() 会读进来
        }
        if (s_model.toast && now >= s_toast_until) {
            s_model.toast = NULL;
            changed = true;
        }
        if (s_battery_ok && now >= s_next_battery) {
            s_model.battery = bsp_battery_soc();
            s_next_battery = now + BATTERY_PERIOD_US;
            changed = true;
        }
        if (now >= s_next_heap_log) {
            s_next_heap_log = now + HEAP_LOG_PERIOD_US;
            ESP_LOGI(TAG, "内存:剩余 %u,最低 %u,最大块 %u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }
        if (changed) refresh();
    }
}

// ---------------------------------------------------------------------------
// 事件入口:按键回调运行在共享 esp_timer 任务,BLE 回调运行在 NimBLE host 任务,
// 联网、Telegram 与网页的回调运行在各自的任务里;都只入队立即返回。
// ---------------------------------------------------------------------------
static void post_simple(app_event_type_t type)
{
    if (!s_queue) return;
    const app_event_t event = { .type = type };
    (void)xQueueSend(s_queue, &event, 0);
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_input_ready || !s_queue) return;
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_LONG) return;
    const app_event_t event = { .type = APP_EV_KEY, .data.key = { .btn = btn, .event = ev } };
    (void)xQueueSend(s_queue, &event, 0);
}

static void on_ble(const wx_ble_event_t *ble, void *ctx)
{
    (void)ctx;
    if (!s_queue) return;
    app_event_t event = { .type = APP_EV_BLE, .data.ble = { .event = *ble } };
    if (ble->type == WX_BLE_EV_NOTIF_ADDED) {
        // 正文指向 BLE 模块的缓冲,出了回调就会被覆盖:要转发时复制一份。
        const char *message = ble->data.added.message;
        event.data.ble.event.data.added.message = NULL;
        if (!ble->data.added.pre_existing && message && message[0] &&
            wx_settings_forwards(ble->data.added.app)) {
            event.data.ble.message = strdup(message);
        }
    }
    if (xQueueSend(s_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "应用事件队列已满,丢弃 BLE 事件 %d", ble->type);
        free(event.data.ble.message);
    }
}

static void on_net(void *ctx)
{
    (void)ctx;
    post_simple(APP_EV_NET);
}

static void on_tg(void *ctx)
{
    (void)ctx;
    post_simple(APP_EV_TG);
}

static void on_portal_done(void *ctx)
{
    (void)ctx;
    post_simple(APP_EV_SETUP_DONE);
}

void app_main(void)
{
    ESP_LOGI(TAG, "消息提醒器启动");

    bsp_i2c_init();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }

    // 配对密钥与各项设置都存在 NVS。初始化失败时不擦除分区(可能有其他数据),
    // 只是配对和设置在重启后会丢失。
    esp_err_t err = nvs_flash_init();
    const bool nvs_ok = err == ESP_OK;
    if (!nvs_ok) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s;未自动擦除,配对与设置将无法保存", esp_err_to_name(err));
    }
    err = wx_settings_init(nvs_ok);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置初始化失败: %s", esp_err_to_name(err));
        return;
    }

    err = bsp_audio_init();
    if (err == ESP_OK) err = wx_sound_start();
    s_sound_ok = err == ESP_OK;
    if (!s_sound_ok) ESP_LOGE(TAG, "音频不可用: %s,仅闪屏提醒", esp_err_to_name(err));

    s_battery_ok = bsp_battery_init() == ESP_OK;

    wx_alert_init(&s_alert);
    s_model.stack = WX_BLE_STACK_STARTING;
    s_model.muted = wx_settings_muted();
    s_model.lang = wx_lang();
    s_model.battery = -1;
    s_model.has_wifi = wx_settings_has_wifi();
    s_model.tg_ready = wx_settings_tg_ready();
    if (s_model.has_wifi) s_model.net.state = WX_NET_CONNECTING;

    // 界面必须在应用任务之前建好:之后只有应用任务刷新界面。
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "无法获取 LVGL 锁,界面初始化失败");
        return;
    }
    err = wx_ui_init();
    if (err == ESP_OK) wx_ui_render(&s_model);
    bsp_lvgl_unlock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "界面初始化失败: %s", esp_err_to_name(err));
        return;
    }
    bsp_display_backlight(BACKLIGHT_IDLE);
    s_backlight = BACKLIGHT_IDLE;

    s_queue = xQueueCreate(APP_QUEUE_DEPTH, sizeof(app_event_t));
    if (!s_queue || xTaskCreate(app_task, "wx_app", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "应用任务创建失败");
        return;
    }

    err = bsp_button_init(on_key, NULL);
    if (err != ESP_OK) ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(err));
    s_input_ready = true;

    err = wx_ble_start(on_ble, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "蓝牙启动失败: %s", esp_err_to_name(err));
        const app_event_t event = {
            .type = APP_EV_BLE,
            .data.ble.event = { .type = WX_BLE_EV_STACK, .data.stack = WX_BLE_STACK_FAILED },
        };
        (void)xQueueSend(s_queue, &event, 0);
    }

    err = wx_tg_start(on_tg, NULL);
    if (err == ESP_OK) err = wx_net_start(on_net, NULL);
    if (err != ESP_OK) ESP_LOGE(TAG, "联网模块启动失败: %s;只做本地提醒", esp_err_to_name(err));

    ESP_LOGI(TAG, "就绪:Sound=%d Battery=%d NVS=%d;剩余内存 %u", s_sound_ok, s_battery_ok, nvs_ok,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}
