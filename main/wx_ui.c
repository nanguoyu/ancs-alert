// main/wx_ui.c —— 提醒器界面。240x320 竖屏,四角有 30 px 圆角遮罩,内容避开角落。
//
// 字体:中文用 tools/gen_wx_fonts.py 从霞鹜文楷生成的位图字库。
//   wx_font_cjk_24 覆盖 ASCII + GB2312 全部汉字与常用标点,用于好友名等动态内容;
//   wx_font_cjk_16 只含本文件、main.c 与 wx_apps.c 字符串里出现的字,用于固定的小字说明。
// 两者都 fallback 到 Montserrat 以显示 LV_SYMBOL_* 图标。GB2312 以外的字显示为占位框。
//
// 界面文字有英文和中文两套,写成 TR("English", "中文"),按 model.lang 选。英文按字库的字宽核算过,
// 改动后要确认放得下 240 px 宽的屏幕(四角还有圆角)。
#include "wx_ui.h"

#include "esp_log.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "wx_ui";

LV_FONT_DECLARE(wx_font_cjk_16);
LV_FONT_DECLARE(wx_font_cjk_24);

#define COLOR_BG          0x0E1116
#define COLOR_GREEN       0x07C160
#define COLOR_IDLE_ICON   0x3A3F46
#define COLOR_TEXT_DIM    0x8A929C
#define COLOR_TOAST_BG    0x2A3038
#define COLOR_ERROR       0xFF6B6B
#define COLOR_DANGER      0xD93636
#define COLOR_ROW         0x1C2129

#define ROW_W             216
#define ROW_H             31
#define ROW_STEP          33

#define QR_SIZE           150
#define QR_BORDER         8

#define FLASH_PERIOD_MS   350
#define FLASH_DURATION_MS 12000

static lv_font_t s_font16;
static lv_font_t s_font24;

// 正在显示的语言;WX_LANG_COUNT 表示固定文字还没写上。
static wx_lang_t s_lang = WX_LANG_COUNT;
#define TR(en, zh) (s_lang == WX_LANG_ZH ? (zh) : (en))

static lv_obj_t *s_scr;
static lv_obj_t *s_idle;
static lv_obj_t *s_idle_icon;
static lv_obj_t *s_idle_status;
static lv_obj_t *s_idle_detail;
static lv_obj_t *s_net;
static lv_obj_t *s_sound;
static lv_obj_t *s_pair;
static lv_obj_t *s_pair_title;
static lv_obj_t *s_pair_hint;
static lv_obj_t *s_pair_code;
static lv_obj_t *s_setup;
static lv_obj_t *s_setup_qr_box;
static lv_obj_t *s_setup_qr;
static lv_obj_t *s_setup_ssid;
static lv_obj_t *s_setup_pass;
static lv_obj_t *s_setup_hint;
static lv_obj_t *s_setup_title;
static lv_obj_t *s_setup_exit;
static lv_obj_t *s_menu;
static lv_obj_t *s_menu_title;
static lv_obj_t *s_menu_hint;
static lv_obj_t *s_confirm;
static lv_obj_t *s_confirm_hint;
static lv_obj_t *s_confirm_title;
static lv_obj_t *s_confirm_detail;
static lv_obj_t *s_alert;
static lv_obj_t *s_alert_app;
static lv_obj_t *s_alert_count;
static lv_obj_t *s_alert_sender;
static lv_obj_t *s_alert_text;
static lv_obj_t *s_alert_hint;
static lv_obj_t *s_link_icon;
static lv_obj_t *s_wifi_icon;
static lv_obj_t *s_battery;
static lv_obj_t *s_toast;
static lv_timer_t *s_flash_timer;

// 菜单与确认页的一行:圆角底 + 文字,选中时底色高亮。
typedef struct {
    lv_obj_t *box;
    lv_obj_t *label;
} row_t;

static row_t s_menu_rows[WX_MENU_COUNT];
static row_t s_confirm_rows[2];   // 0 = 取消,1 = 确定清除

static uint32_t s_last_alert_seq;
static uint32_t s_flash_until;
static bool s_flash_on;
static uint32_t s_alert_color = COLOR_GREEN;
static char s_qr_text[96];

// LVGL 内存池用量,用来确定 CONFIG_LV_MEM_SIZE。
static void log_lvgl_memory(const char *when)
{
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "LVGL 内存(%s):已用 %u / %u,最多用过 %u", when,
             (unsigned)(mon.total_size - mon.free_size), (unsigned)mon.total_size, (unsigned)mon.max_used);
}

static lv_obj_t *plain_box(lv_obj_t *parent, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *page(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, "");
    return label;
}

// 聊天气泡:圆角矩形 + 左下角的小圆作“尾巴”。
static lv_obj_t *bubble(lv_obj_t *parent, int w, int h, int y, uint32_t color, lv_obj_t **tail)
{
    lv_obj_t *t = plain_box(parent, h / 3, h / 3, color, LV_RADIUS_CIRCLE);
    lv_obj_align(t, LV_ALIGN_CENTER, -w / 2 + h / 4, y + h / 2 - h / 12);
    lv_obj_t *b = plain_box(parent, w, h, color, h / 2);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, y);
    if (tail) *tail = t;
    return b;
}

static void build_idle(void)
{
    s_idle = page(s_scr);
    // 待机图标是通用的铃铛:板子接收 iPhone 的全部通知,只对聊天应用弹出提醒。
    s_idle_icon = plain_box(s_idle, 88, 88, COLOR_IDLE_ICON, LV_RADIUS_CIRCLE);
    lv_obj_align(s_idle_icon, LV_ALIGN_CENTER, 0, -66);
    lv_obj_t *bell = text(s_idle_icon, &lv_font_montserrat_48, 0xFFFFFF);
    lv_label_set_text(bell, LV_SYMBOL_BELL);
    lv_obj_center(bell);
    s_idle_status = text(s_idle, &s_font24, 0xFFFFFF);
    lv_obj_align(s_idle_status, LV_ALIGN_CENTER, 0, 16);
    // 说明区最多三行:每台手机一行。
    s_idle_detail = text(s_idle, &s_font16, COLOR_TEXT_DIM);
    lv_obj_set_width(s_idle_detail, 210);
    lv_label_set_long_mode(s_idle_detail, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_idle_detail, LV_ALIGN_TOP_MID, 0, 194);
    s_net = text(s_idle, &s_font16, COLOR_TEXT_DIM);
    lv_obj_set_width(s_net, 210);
    lv_label_set_long_mode(s_net, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_net, LV_ALIGN_BOTTOM_MID, 0, -42);
    s_sound = text(s_idle, &s_font16, COLOR_TEXT_DIM);
    lv_obj_align(s_sound, LV_ALIGN_BOTTOM_MID, 0, -20);
}

static void build_pair(void)
{
    s_pair = page(s_scr);
    s_pair_title = text(s_pair, &s_font24, 0xFFFFFF);
    lv_obj_align(s_pair_title, LV_ALIGN_CENTER, 0, -70);
    s_pair_code = text(s_pair, &lv_font_montserrat_48, COLOR_GREEN);
    lv_obj_align(s_pair_code, LV_ALIGN_CENTER, 0, -10);
    s_pair_hint = text(s_pair, &s_font16, COLOR_TEXT_DIM);
    lv_obj_set_width(s_pair_hint, 200);
    lv_label_set_long_mode(s_pair_hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_pair_hint, LV_ALIGN_CENTER, 0, 50);
}

// 配网页:二维码让 iPhone 相机一扫就加入热点;连上后手机会自动弹出设置网页。
static void build_setup(void)
{
    s_setup = page(s_scr);
    s_setup_title = text(s_setup, &s_font24, 0xFFFFFF);
    lv_obj_align(s_setup_title, LV_ALIGN_TOP_MID, 0, 30);
    // 二维码要白底和留白才好扫;二维码本身在进入配网时才创建,退出后释放。
    s_setup_qr_box = plain_box(s_setup, QR_SIZE + 2 * QR_BORDER, QR_SIZE + 2 * QR_BORDER, 0xFFFFFF, 8);
    lv_obj_align(s_setup_qr_box, LV_ALIGN_TOP_MID, 0, 62);
    s_setup_ssid = text(s_setup, &s_font16, 0xFFFFFF);
    lv_obj_align(s_setup_ssid, LV_ALIGN_TOP_MID, 0, 234);
    s_setup_pass = text(s_setup, &s_font16, 0xFFFFFF);
    lv_obj_align(s_setup_pass, LV_ALIGN_TOP_MID, 0, 254);
    s_setup_hint = text(s_setup, &s_font16, COLOR_TEXT_DIM);
    lv_obj_set_width(s_setup_hint, 200);
    lv_label_set_long_mode(s_setup_hint, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_setup_hint, LV_ALIGN_TOP_MID, 0, 274);
    s_setup_exit = text(s_setup, &s_font16, COLOR_TEXT_DIM);
    lv_obj_align(s_setup_exit, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static row_t make_row(lv_obj_t *parent, int y)
{
    row_t row;
    row.box = plain_box(parent, ROW_W, ROW_H, COLOR_ROW, 10);
    lv_obj_align(row.box, LV_ALIGN_TOP_MID, 0, y);
    row.label = text(row.box, &s_font24, 0xFFFFFF);
    lv_obj_align(row.label, LV_ALIGN_LEFT_MID, 12, 0);
    return row;
}

// 危险的行(清除)不选中时文字发红,选中时整行变红。
static void style_row(const row_t *row, bool selected, bool danger)
{
    const uint32_t bg = selected ? (danger ? COLOR_DANGER : COLOR_GREEN) : COLOR_ROW;
    lv_obj_set_style_bg_color(row->box, lv_color_hex(bg), 0);
    lv_obj_set_style_text_color(row->label, lv_color_hex(danger && !selected ? COLOR_ERROR : 0xFFFFFF), 0);
}

// 设置菜单:上下键移动,确定键进入。清除类操作都要在确认页再确认一次。
static void build_menu(void)
{
    s_menu = page(s_scr);
    s_menu_title = text(s_menu, &s_font24, 0xFFFFFF);
    lv_obj_align(s_menu_title, LV_ALIGN_TOP_MID, 0, 18);
    // 7 行:52 + 6 × 33 + 31 = 281,底下留给提示。
    for (size_t i = 0; i < WX_MENU_COUNT; i++) {
        s_menu_rows[i] = make_row(s_menu, 52 + (int)i * ROW_STEP);
    }
    s_menu_hint = text(s_menu, &s_font16, COLOR_TEXT_DIM);
    lv_obj_align(s_menu_hint, LV_ALIGN_BOTTOM_MID, 0, -12);
}

static void build_confirm(void)
{
    s_confirm = page(s_scr);
    s_confirm_title = text(s_confirm, &s_font24, 0xFFFFFF);
    lv_obj_set_width(s_confirm_title, 210);
    lv_label_set_long_mode(s_confirm_title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_confirm_title, LV_ALIGN_TOP_MID, 0, 56);
    s_confirm_detail = text(s_confirm, &s_font16, COLOR_TEXT_DIM);
    lv_obj_set_width(s_confirm_detail, 200);
    lv_label_set_long_mode(s_confirm_detail, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_confirm_detail, LV_ALIGN_TOP_MID, 0, 100);
    s_confirm_rows[0] = make_row(s_confirm, 180);
    s_confirm_rows[1] = make_row(s_confirm, 218);
    s_confirm_hint = text(s_confirm, &s_font16, COLOR_TEXT_DIM);
    lv_obj_align(s_confirm_hint, LV_ALIGN_BOTTOM_MID, 0, -12);
}

static void build_alert(void)
{
    s_alert = page(s_scr);
    lv_obj_set_style_bg_color(s_alert, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_bg_opa(s_alert, LV_OPA_COVER, 0);

    s_alert_app = text(s_alert, &s_font24, 0xFFFFFF);
    lv_obj_align(s_alert_app, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *b = bubble(s_alert, 150, 100, -28, 0xFFFFFF, NULL);
    s_alert_count = text(b, &lv_font_montserrat_48, COLOR_GREEN);
    lv_obj_center(s_alert_count);

    s_alert_sender = text(s_alert, &s_font24, 0xFFFFFF);
    lv_obj_set_width(s_alert_sender, 200);
    lv_label_set_long_mode(s_alert_sender, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_alert_sender, LV_ALIGN_CENTER, 0, 52);

    s_alert_text = text(s_alert, &s_font16, 0xFFFFFF);
    lv_obj_set_width(s_alert_text, 210);
    lv_label_set_long_mode(s_alert_text, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_alert_text, LV_ALIGN_CENTER, 0, 84);

    s_alert_hint = text(s_alert, &s_font16, 0xFFFFFF);
    lv_obj_set_style_text_opa(s_alert_hint, LV_OPA_80, 0);
    lv_obj_align(s_alert_hint, LV_ALIGN_BOTTOM_MID, 0, -22);
}

static void flash_cb(lv_timer_t *timer)
{
    (void)timer;
    if (lv_obj_has_flag(s_alert, LV_OBJ_FLAG_HIDDEN)) return;
    bool flashing = (int32_t)(s_flash_until - lv_tick_get()) > 0;
    s_flash_on = flashing ? !s_flash_on : false;
    // 闪烁的暗色:应用底色压暗约 1/3。
    const lv_color_t base = lv_color_hex(s_alert_color);
    lv_obj_set_style_bg_color(s_alert, s_flash_on ? lv_color_darken(base, LV_OPA_30) : base, 0);
}

esp_err_t wx_ui_init(void)
{
    // 可写的描述符副本:只为挂 fallback,不修改只读的生成字体。
    s_font16 = wx_font_cjk_16;
    s_font16.fallback = &lv_font_montserrat_14;
    s_font24 = wx_font_cjk_24;
    s_font24.fallback = &lv_font_montserrat_20;

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    build_idle();
    build_pair();
    build_setup();
    build_menu();
    build_confirm();
    build_alert();

    // 顶栏放在最后创建,压在各页之上。
    s_link_icon = text(s_scr, &lv_font_montserrat_14, COLOR_TEXT_DIM);
    lv_label_set_text(s_link_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_align(s_link_icon, LV_ALIGN_TOP_LEFT, 32, 14);
    s_wifi_icon = text(s_scr, &lv_font_montserrat_14, COLOR_TEXT_DIM);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_align(s_wifi_icon, LV_ALIGN_TOP_LEFT, 50, 14);
    s_battery = text(s_scr, &lv_font_montserrat_14, COLOR_TEXT_DIM);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -32, 14);

    s_toast = text(s_scr, &s_font16, 0xFFFFFF);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(COLOR_TOAST_BG), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, 10, 0);
    lv_obj_set_style_pad_hor(s_toast, 12, 0);
    lv_obj_set_style_pad_ver(s_toast, 6, 0);
    lv_obj_set_style_max_width(s_toast, 216, 0);
    lv_label_set_long_mode(s_toast, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    s_flash_timer = lv_timer_create(flash_cb, FLASH_PERIOD_MS, NULL);
    if (!s_flash_timer) return ESP_ERR_NO_MEM;

    lv_screen_load(s_scr);
    log_lvgl_memory("界面建好");
    return ESP_OK;
}

static void set_visible(lv_obj_t *obj, bool visible)
{
    if (visible) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

// 手机在界面上的短名:“iPhone XR” → “XR”;还没读到型号时用 “iPhone 1” 这样的序号。
static const char *phone_label(const wx_ui_model_t *m, size_t i, char *buf, size_t cap)
{
    if (m->phones[i].name[0]) return wx_phone_short_name(m->phones[i].name);
    snprintf(buf, cap, "iPhone %u", (unsigned)(i + 1));
    return buf;
}

static size_t count_phones(const wx_ui_model_t *m, bool ready_only)
{
    size_t n = 0;
    for (size_t i = 0; i < WX_BLE_MAX_PHONES; i++) {
        const wx_phone_state_t st = m->phones[i].state;
        if (ready_only ? st == WX_PHONE_READY : st != WX_PHONE_OFF) n++;
    }
    return n;
}

// 一台手机一行。板子接收的是每台 iPhone 的全部通知,只对聊天应用弹出提醒。
static void append_phone_line(const wx_ui_model_t *m, size_t i, char *detail, size_t cap)
{
    char fallback[12];
    const char *name = phone_label(m, i, fallback, sizeof(fallback));
    const wx_ui_phone_t *p = &m->phones[i];
    size_t used = strlen(detail);
    const char *sep = used ? "\n" : "";
    switch (p->state) {
    case WX_PHONE_READY:
        snprintf(detail + used, cap - used, TR("%s%s: %u total, %u chats", "%s%s：共 %u 条，聊天 %u 条"),
                 sep, name, (unsigned)p->total, (unsigned)p->chats);
        break;
    case WX_PHONE_CONNECTED:
        snprintf(detail + used, cap - used, TR("%s%s: encrypting", "%s%s：正在加密连接"), sep, name);
        break;
    case WX_PHONE_DISCOVERING:
        snprintf(detail + used, cap - used, TR("%s%s: tap Allow", "%s%s：弹窗时请点“允许”"), sep, name);
        break;
    case WX_PHONE_NO_ANCS:
        snprintf(detail + used, cap - used, TR("%s%s: notifications off", "%s%s：未允许共享通知"), sep, name);
        break;
    case WX_PHONE_OFF:
    default:
        break;
    }
}

static void idle_texts(const wx_ui_model_t *m, char *status, size_t status_cap,
                       char *detail, size_t cap)
{
    detail[0] = '\0';
    const size_t connected = count_phones(m, false);
    const size_t ready = count_phones(m, true);

    if (m->stack == WX_BLE_STACK_FAILED) {
        snprintf(status, status_cap, "%s", TR("Bluetooth error", "蓝牙出错"));
        snprintf(detail, cap, "%s", TR("Restart the device", "请重启设备"));
    } else if (m->stack == WX_BLE_STACK_STARTING) {
        snprintf(status, status_cap, "%s", TR("Starting…", "正在启动…"));
    } else if (connected == 0) {
        snprintf(status, status_cap, "%s", TR("Waiting for iPhone", "等待 iPhone 连接"));
        char name[WX_BLE_DEVICE_NAME_MAX];
        wx_ble_device_name(name, sizeof(name));
        snprintf(detail, cap, TR("New phone: Settings > Bluetooth > %s", "新手机：设置 > 蓝牙 > %s"), name);
    } else if (connected == 1 && ready == 1) {
        // 只连一台时多给一行说明。
        snprintf(status, status_cap, "%s", TR("iPhone connected", "已连接 iPhone"));
        for (size_t i = 0; i < WX_BLE_MAX_PHONES; i++) {
            const wx_ui_phone_t *p = &m->phones[i];
            if (p->state != WX_PHONE_READY) continue;
            if (p->total > 0) {
                snprintf(detail, cap, TR("Receiving all notifications\n%u in total, %u from chats",
                                         "正在接收全部通知\n共 %u 条，其中聊天 %u 条"),
                         (unsigned)p->total, (unsigned)p->chats);
            } else {
                snprintf(detail, cap, "%s", TR("Receiving all notifications\nNew messages appear here",
                                               "正在接收全部通知\n来新消息会在这里提醒"));
            }
        }
    } else {
        if (ready == 0) {
            snprintf(status, status_cap, "%s", TR("Connecting…", "正在连接 iPhone…"));
        } else {
            snprintf(status, status_cap, TR("%u iPhones linked", "已连接 %u 台 iPhone"), (unsigned)connected);
        }
        for (size_t i = 0; i < WX_BLE_MAX_PHONES; i++) append_phone_line(m, i, detail, cap);
    }
}

// Wi-Fi 与 Telegram 转发的一行状态。网络名可能含 16 px 小字库里没有的字,所以不显示;
// Wi-Fi 连没连上看顶栏的图标颜色。
static const char *net_text(const wx_ui_model_t *m)
{
    if (!m->has_wifi) return TR("Press OK to set up Wi-Fi", "按确定键设置 Wi-Fi 与转发");
    switch (m->net.state) {
    case WX_NET_CONNECTED:
        if (!m->tg_ready) return TR("Telegram not set up", "未设置 Telegram 转发");
        return m->tg_failing ? TR("Telegram failed, will retry", "Telegram 发送失败，将重试")
                             : TR("Forwarding to Telegram", "正在转发到 Telegram");
    case WX_NET_FAILED:
        return TR("Wi-Fi failed, will retry", "Wi-Fi 连不上，稍后自动重试");
    case WX_NET_CONNECTING:
    case WX_NET_OFF:
    default:
        return TR("Connecting to Wi-Fi…", "Wi-Fi 正在连接…");
    }
}

// 提醒页小字:几个应用都有新消息时分别列出,如 “WeChat 2, QQ 1”;否则 “3 new messages”。
static void alert_summary(const wx_ui_model_t *m, char *out, size_t cap)
{
    size_t apps = 0;
    for (uint8_t app = 0; app < WX_APP_COUNT; app++) {
        if (m->unseen_app[app]) apps++;
    }
    out[0] = '\0';
    if (apps <= 1) {
        if (m->unseen == 1) {
            snprintf(out, cap, "%s", TR("1 new message", "1 条新消息"));
        } else {
            snprintf(out, cap, TR("%u new messages", "%u 条新消息"), (unsigned)m->unseen);
        }
    } else {
        for (uint8_t app = 0; app < WX_APP_COUNT; app++) {
            if (!m->unseen_app[app]) continue;
            const size_t used = strlen(out);
            snprintf(out + used, cap - used, "%s%s %u", used ? TR(", ", "，") : "", wx_app_name(app, s_lang),
                     (unsigned)m->unseen_app[app]);
        }
    }
    if (count_phones(m, false) > 1 && m->sender_phone[0]) {
        // 连着多台手机时,标出最近一条消息来自哪台。
        const size_t used = strlen(out);
        snprintf(out + used, cap - used, TR(", %s", "，%s"), wx_phone_short_name(m->sender_phone));
    }
}

static void render_setup(const wx_ui_model_t *m)
{
    if (!s_setup_qr) {
        s_setup_qr = lv_qrcode_create(s_setup_qr_box);
        lv_qrcode_set_size(s_setup_qr, QR_SIZE);
        lv_qrcode_set_dark_color(s_setup_qr, lv_color_black());
        lv_qrcode_set_light_color(s_setup_qr, lv_color_white());
        lv_obj_center(s_setup_qr);
        s_qr_text[0] = '\0';
    }
    char qr[sizeof(s_qr_text)];
    // iPhone 相机认识的 Wi-Fi 二维码格式;热点名和密码只含字母数字和 "-",无需转义。
    snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", m->net.setup_ssid, m->net.setup_pass);
    if (strcmp(qr, s_qr_text) != 0) {
        memcpy(s_qr_text, qr, sizeof(qr));
        if (lv_qrcode_update(s_setup_qr, qr, strlen(qr)) != LV_RESULT_OK) {
            ESP_LOGE(TAG, "二维码生成失败");
        }
        log_lvgl_memory("二维码");
    }
    lv_label_set_text_fmt(s_setup_ssid, TR("Hotspot  %s", "热点  %s"), m->net.setup_ssid);
    lv_label_set_text_fmt(s_setup_pass, TR("Password  %s", "密码  %s"), m->net.setup_pass);
    if (m->net.setup_clients > 0) {
        lv_obj_set_style_text_color(s_setup_hint, lv_color_hex(COLOR_GREEN), 0);
        lv_label_set_text(s_setup_hint, TR("Connected: 192.168.4.1", "已连上，网页 192.168.4.1"));
    } else {
        lv_obj_set_style_text_color(s_setup_hint, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_label_set_text(s_setup_hint, TR("Scan with iPhone camera", "用 iPhone 相机扫码"));
    }
}

static const char *menu_label(const wx_ui_model_t *m, uint8_t item)
{
    switch (item) {
    case WX_MENU_SOUND: return m->muted ? TR("Sound: Off", "提示音：关") : TR("Sound: On", "提示音：开");
    case WX_MENU_TEST: return TR("Test sound", "试听提示音");
    case WX_MENU_SETUP: return TR("Wi-Fi setup", "Wi-Fi 与转发设置");
    // 两种语言写法相同:看不懂当前语言的人也认得出来。
    case WX_MENU_LANGUAGE: return "English / 中文";
    case WX_MENU_CLEAR_WIFI: return TR("Clear Wi-Fi", "清除 Wi-Fi 网络");
    case WX_MENU_FORGET_PHONES: return TR("Forget iPhones", "清除 iPhone 配对");
    case WX_MENU_BACK:
    default: return TR("Back", "返回");
    }
}

static void render_menu(const wx_ui_model_t *m)
{
    for (uint8_t i = 0; i < WX_MENU_COUNT; i++) {
        lv_label_set_text(s_menu_rows[i].label, menu_label(m, i));
        const bool danger = i == WX_MENU_CLEAR_WIFI || i == WX_MENU_FORGET_PHONES;
        style_row(&s_menu_rows[i], i == m->menu_index, danger);
    }
}

static void render_confirm(const wx_ui_model_t *m)
{
    if (m->confirm == WX_CONFIRM_CLEAR_WIFI) {
        lv_label_set_text(s_confirm_title, TR("Clear Wi-Fi?", "清除 Wi-Fi 网络？"));
        lv_label_set_text(s_confirm_detail, TR("Deletes all saved networks and turns Wi-Fi off. Telegram settings are kept.",
                                               "删除全部已保存的网络，板子断开 Wi-Fi。Telegram 设置保留。"));
    } else {
        lv_label_set_text(s_confirm_title, TR("Forget iPhones?", "清除 iPhone 配对？"));
        lv_label_set_text(s_confirm_detail, TR("Each iPhone must forget this device in Bluetooth settings, then pair again.",
                                               "所有 iPhone 都要在蓝牙设置里忽略本设备，再重新配对。"));
    }
    style_row(&s_confirm_rows[0], !m->confirm_yes, false);
    style_row(&s_confirm_rows[1], m->confirm_yes, true);
}

static void release_setup_qr(void)
{
    if (!s_setup_qr) return;
    lv_obj_delete(s_setup_qr);
    s_setup_qr = NULL;
    s_qr_text[0] = '\0';
}

// 只在语言变了时重写的固定文字。
static void apply_static_texts(void)
{
    lv_label_set_text(s_pair_title, TR("Pair with iPhone", "与 iPhone 配对"));
    lv_label_set_text(s_pair_hint, TR("Enter this code on your iPhone", "在 iPhone 上输入这个配对码"));
    lv_label_set_text(s_setup_title, TR("Wi-Fi setup", "Wi-Fi 设置"));
    lv_label_set_text(s_setup_exit, TR("Press any key to exit", "按任意键退出"));
    lv_label_set_text(s_menu_title, TR("Settings", "设置"));
    lv_label_set_text(s_menu_hint, TR("Up/Down, OK to open", "上下键选择，确定键进入"));
    lv_label_set_text(s_confirm_rows[0].label, TR("Cancel", "取消"));
    lv_label_set_text(s_confirm_rows[1].label, TR("Clear", "确定清除"));
    lv_label_set_text(s_confirm_hint, TR("Up/Down, OK to confirm", "上下键选择，确定键确认"));
    lv_label_set_text(s_alert_hint, TR("Press any key to clear", "按任意键清除"));
}

void wx_ui_render(const wx_ui_model_t *m)
{
    if (m->lang != s_lang) {
        s_lang = m->lang < WX_LANG_COUNT ? m->lang : WX_LANG_EN;
        apply_static_texts();
    }

    // 优先级:配对码(有时限) > 配网 > 提醒 > 设置菜单 > 待机。配网时来的消息照样计数和播报,
    // 退出配网后显示。
    const bool pairing = m->show_passkey;
    const bool setup = !pairing && m->net.setup;
    const bool alerting = !pairing && !setup && m->alerting;
    const bool menu = !pairing && !setup && !alerting && m->menu;
    const bool confirm = menu && m->confirm != WX_CONFIRM_NONE;
    set_visible(s_pair, pairing);
    set_visible(s_setup, setup);
    set_visible(s_alert, alerting);
    set_visible(s_menu, menu && !confirm);
    set_visible(s_confirm, confirm);
    set_visible(s_idle, !pairing && !setup && !alerting && !menu);

    if (confirm) {
        render_confirm(m);
    } else if (menu) {
        render_menu(m);
    }

    if (setup) {
        render_setup(m);
    } else {
        release_setup_qr();
    }

    if (alerting) {
        if (m->alert_seq != s_last_alert_seq) {
            s_last_alert_seq = m->alert_seq;
            s_flash_until = lv_tick_get() + FLASH_DURATION_MS;
        }
        s_alert_color = wx_app_color(m->alert_app);
        if (!s_flash_on) lv_obj_set_style_bg_color(s_alert, lv_color_hex(s_alert_color), 0);
        lv_obj_set_style_text_color(s_alert_count, lv_color_hex(s_alert_color), 0);
        lv_label_set_text(s_alert_app, wx_app_name(m->alert_app, s_lang));
        if (m->unseen > 99) {
            lv_label_set_text(s_alert_count, "99+");
        } else {
            lv_label_set_text_fmt(s_alert_count, "%u", (unsigned)m->unseen);
        }
        const bool named = !wx_app_title_is_generic(m->alert_app, m->sender);
        lv_label_set_text(s_alert_sender, named ? m->sender : TR("New message", "你有新消息"));
        char summary[64];
        alert_summary(m, summary, sizeof(summary));
        lv_label_set_text(s_alert_text, summary);
    } else if (!m->alerting) {
        s_last_alert_seq = m->alert_seq;
        s_flash_until = lv_tick_get();
    }

    if (pairing) {
        lv_label_set_text_fmt(s_pair_code, "%06lu", (unsigned long)m->passkey);
    }

    char status[40];
    char detail[192];
    idle_texts(m, status, sizeof(status), detail, sizeof(detail));
    lv_label_set_text(s_idle_status, status);
    lv_label_set_text(s_idle_detail, detail);
    const bool any_ready = count_phones(m, true) > 0;
    lv_obj_set_style_bg_color(s_idle_icon, lv_color_hex(any_ready ? COLOR_GREEN : COLOR_IDLE_ICON), 0);
    lv_label_set_text(s_net, net_text(m));
    lv_label_set_text(s_sound, m->muted ? TR(LV_SYMBOL_MUTE "  Sound off", LV_SYMBOL_MUTE "  提示音已关")
                                        : TR(LV_SYMBOL_VOLUME_MAX "  Sound on", LV_SYMBOL_VOLUME_MAX "  提示音已开"));

    // 提醒页底色鲜艳,顶栏改用白色。
    const uint32_t top = alerting ? 0xFFFFFF : COLOR_TEXT_DIM;
    lv_obj_set_style_text_color(s_link_icon,
        lv_color_hex(any_ready && !alerting ? COLOR_GREEN : top), 0);
    uint32_t wifi_color = top;
    if (!alerting && m->net.state == WX_NET_CONNECTED) wifi_color = COLOR_GREEN;
    if (!alerting && m->net.state == WX_NET_FAILED) wifi_color = COLOR_ERROR;
    set_visible(s_wifi_icon, m->net.state != WX_NET_OFF);
    lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(wifi_color), 0);
    lv_obj_set_style_text_color(s_battery, lv_color_hex(top), 0);
    if (m->battery >= 0) {
        lv_label_set_text_fmt(s_battery, "%d%%", m->battery);
    } else {
        lv_label_set_text(s_battery, "");
    }

    set_visible(s_toast, m->toast != NULL);
    if (m->toast) lv_label_set_text(s_toast, m->toast);
}
