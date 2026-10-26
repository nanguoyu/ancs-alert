// main/wx_net.c —— Wi-Fi 连接与配网热点。
//
// 所有连接决策都在联网任务里串行进行:系统事件回调只把“断开”“拿到 IP”投递过来。
// 连接流程:扫描 → 在扫描到的已保存网络里按信号从强到弱轮换 → 连不上就换下一个,
// 都试过一轮后按 5 s、10 s、20 s … 最长 5 分钟的间隔重试。扫描不到时也直接按名字连一次,
// 这样隐藏网络也能连上。
#include "wx_net.h"
#include "wx_settings.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "wx_net";

#define SCAN_MAX            20
#define CONNECT_TIMEOUT_MS  20000
#define RETRY_FIRST_MS      5000
#define RETRY_MAX_MS        (5 * 60 * 1000)
#define RETRY_NEXT_AP_MS    1000       // 这一轮还有别的已保存网络没试
#define RECONNECT_MS        2000       // 连上后又断了
#define LEAVE_WINDOW_MS     2000       // 自己断开后这段时间里的“主动离开”断开事件都是自己引起的
#define HOSTNAME            "passport-alert"
#define CMD_QUEUE_DEPTH     8

// 配网热点的 DHCP 带上设置网页地址(RFC 8910 选项 114),iPhone 连上后直接弹出网页。
// DHCP 服务器只保存指针,所以必须是静态存储。
static char s_portal_url[] = "http://192.168.4.1/";

typedef enum {
    CMD_RELOAD = 0,
    CMD_DISCONNECTED,
    CMD_GOT_IP,
    CMD_SETUP_BEGIN,
    CMD_SETUP_END,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    uint16_t reason;                     // CMD_DISCONNECTED
    char ssid[WX_WIFI_SSID_MAX + 1];     // CMD_RELOAD:要优先连接的网络,空串表示无
} cmd_t;

typedef enum {
    DL_NONE = 0,
    DL_RETRY,
    DL_CONNECT_TIMEOUT,
} deadline_t;

static struct {
    wx_net_cb_t cb;
    void *ctx;
    QueueHandle_t cmds;
    SemaphoreHandle_t driver_lock;   // 扫描与改连接配置互斥(网页的扫描在 httpd 任务里)
    SemaphoreHandle_t status_lock;
    wx_net_status_t status;
    bool netif_ready;                // esp_netif 与事件循环已建好
    bool driver_ready;               // esp_wifi_init 已完成
    bool handlers_ready;             // 事件回调已注册
    bool inited;                     // 全部初始化完成(含事件回调)
    bool started;                    // esp_wifi_start 已调用
    esp_netif_t *sta;
    esp_netif_t *ap;
    TickType_t leave_until;          // 自己调用 esp_wifi_disconnect 后,忽略“主动离开”事件到这个时刻
    uint32_t attempt;                // 在扫描到的已保存网络之间轮换
    uint8_t candidates;
    uint8_t failures;                // 连续失败的轮数(每个候选网络都试过一次算一轮),决定重试间隔
    deadline_t deadline_action;
    TickType_t deadline;
    volatile bool online;
} s;

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
static void notify(void)
{
    if (s.cb) s.cb(s.ctx);
}

static void status_lock(void)
{
    xSemaphoreTake(s.status_lock, portMAX_DELAY);
}

static void status_unlock(void)
{
    xSemaphoreGive(s.status_lock);
}

static void set_state(wx_net_state_t state, const char *ssid, const char *error)
{
    status_lock();
    s.status.state = state;
    if (ssid) snprintf(s.status.ssid, sizeof(s.status.ssid), "%s", ssid);
    if (state != WX_NET_CONNECTED) {
        s.status.ip[0] = '\0';
        s.status.rssi = 0;
    }
    s.status.error = error;
    status_unlock();
    notify();
}

static void schedule(deadline_t action, uint32_t ms)
{
    s.deadline_action = action;
    s.deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
}

static const char *reason_text(uint16_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return WX_TR("Wrong password?", "密码可能不对");
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return WX_TR("Network not found nearby", "附近找不到这个网络");
    case WIFI_REASON_BEACON_TIMEOUT:
        return WX_TR("Signal too weak", "信号太弱");
    default:
        return WX_TR("Could not connect", "连接失败");
    }
}

// ---------------------------------------------------------------------------
// 驱动
// ---------------------------------------------------------------------------
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s.online = false;
        const cmd_t cmd = { .type = CMD_DISCONNECTED, .reason = d->reason };
        // 丢了这条就不会重连,所以队列满时稍等一下。
        (void)xQueueSend(s.cmds, &cmd, pdMS_TO_TICKS(100));
    } else if (id == WIFI_EVENT_AP_STACONNECTED || id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_sta_list_t list;
        const uint8_t clients = esp_wifi_ap_get_sta_list(&list) == ESP_OK ? (uint8_t)list.num : 0;
        ESP_LOGI(TAG, "配网热点上有 %u 台设备", clients);
        status_lock();
        s.status.setup_clients = clients;
        status_unlock();
        notify();
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;
    const ip_event_got_ip_t *e = data;
    status_lock();
    snprintf(s.status.ip, sizeof(s.status.ip), IPSTR, IP2STR(&e->ip_info.ip));
    status_unlock();
    const cmd_t cmd = { .type = CMD_GOT_IP };
    (void)xQueueSend(s.cmds, &cmd, pdMS_TO_TICKS(100));
}

// 分步初始化,每一步只做一次:中途失败(例如内存不够)后再调用会从失败的那一步继续,
// 不会重复创建网络接口(重复创建会触发断言重启)。
static esp_err_t ensure_wifi(void)
{
    if (s.inited) return ESP_OK;
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    esp_err_t err;

    if (!s.netif_ready) {
        err = esp_netif_init();
        if (err == ESP_OK) err = esp_event_loop_create_default();
        if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;   // 事件循环已存在
        if (err != ESP_OK) return err;
        s.netif_ready = true;
    }
    if (!s.sta) {
        s.sta = esp_netif_create_default_wifi_sta();
        (void)esp_netif_set_hostname(s.sta, HOSTNAME);
    }
    if (!s.ap) {
        s.ap = esp_netif_create_default_wifi_ap();
        err = esp_netif_dhcps_option(s.ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                     s_portal_url, strlen(s_portal_url));
        if (err != ESP_OK) ESP_LOGW(TAG, "设置 DHCP 网页地址失败: %s", esp_err_to_name(err));
    }
    if (!s.driver_ready) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) return err;
        s.driver_ready = true;
        // 网络与密码由 wx_settings 管理,驱动不必再存一份到 NVS。
        (void)esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (!s.handlers_ready) {
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL);
        if (err != ESP_OK) return err;
        s.handlers_ready = true;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL);
    if (err != ESP_OK) return err;
    (void)esp_wifi_set_mode(WIFI_MODE_STA);

    s.inited = true;
    ESP_LOGI(TAG, "Wi-Fi 已初始化,占用约 %u 字节,剩余 %u 字节",
             (unsigned)(before - heap_caps_get_free_size(MALLOC_CAP_8BIT)),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    return ESP_OK;
}

static esp_err_t ensure_started(void)
{
    if (s.started) return ESP_OK;
    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK) s.started = true;
    return err;
}

static void disconnect_quietly(void)
{
    if (!s.started) return;
    if (s.status.state == WX_NET_CONNECTING || s.status.state == WX_NET_CONNECTED) {
        s.leave_until = xTaskGetTickCount() + pdMS_TO_TICKS(LEAVE_WINDOW_MS);
        (void)esp_wifi_disconnect();
    }
    s.online = false;
}

static esp_err_t scan_into(wx_wifi_seen_t *out, size_t cap, size_t *count)
{
    *count = 0;
    xSemaphoreTake(s.driver_lock, portMAX_DELAY);
    const wifi_scan_config_t config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 60, .max = 120 },
    };
    esp_err_t err = esp_wifi_scan_start(&config, true);
    uint16_t n = 0;
    wifi_ap_record_t *records = NULL;
    if (err == ESP_OK) err = esp_wifi_scan_get_ap_num(&n);
    if (err == ESP_OK && n > 0) {
        if (n > SCAN_MAX) n = SCAN_MAX;
        records = calloc(n, sizeof(*records));
        err = records ? esp_wifi_scan_get_ap_records(&n, records) : ESP_ERR_NO_MEM;
    }
    if (!records) (void)esp_wifi_clear_ap_list();
    xSemaphoreGive(s.driver_lock);

    for (uint16_t i = 0; records && i < n && *count < cap; i++) {
        wx_wifi_seen_t *seen = &out[(*count)++];
        snprintf(seen->ssid, sizeof(seen->ssid), "%s", (const char *)records[i].ssid);
        seen->rssi = records[i].rssi;
        seen->secure = records[i].authmode != WIFI_AUTH_OPEN;
    }
    free(records);
    *count = wx_wifi_dedupe(out, *count);
    return err == ESP_ERR_WIFI_STATE ? ESP_ERR_INVALID_STATE : err;
}

// ---------------------------------------------------------------------------
// 连接
// ---------------------------------------------------------------------------
static void connection_failed(const char *why)
{
    s.online = false;
    s.attempt++;
    // 这一轮还有没试过的已保存网络:马上试下一个;都试过了才按失败轮数退避。
    const bool round_done = s.candidates <= 1 || s.attempt % s.candidates == 0;
    uint32_t delay = RETRY_NEXT_AP_MS;
    if (round_done) {
        if (s.failures < UINT8_MAX) s.failures++;
        delay = RETRY_MAX_MS;
        if (s.failures <= 7) {
            delay = RETRY_FIRST_MS << (s.failures - 1);
            if (delay > RETRY_MAX_MS) delay = RETRY_MAX_MS;
        }
    }
    ESP_LOGW(TAG, "连接 %s 失败:%s;%lu 秒后重试", s.status.ssid, why, (unsigned long)(delay / 1000));
    set_state(WX_NET_FAILED, NULL, why);
    schedule(DL_RETRY, delay);
}

static void go_off(void)
{
    s.deadline_action = DL_NONE;
    disconnect_quietly();
    if (s.started && !s.status.setup) {
        (void)esp_wifi_stop();
        s.started = false;
    }
    set_state(WX_NET_OFF, "", NULL);
}

static void connect_next(const char *prefer)
{
    s.deadline_action = DL_NONE;
    static wx_wifi_list_t list;
    wx_settings_wifi(&list);
    if (list.count == 0) {
        go_off();
        return;
    }
    esp_err_t err = ensure_wifi();
    if (err == ESP_OK) err = ensure_started();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 启动失败: %s", esp_err_to_name(err));
        connection_failed(WX_TR("Wi-Fi failed to start", "Wi-Fi 启动失败"));
        return;
    }

    int pick = prefer ? wx_wifi_list_find(&list, prefer) : -1;
    if (pick >= 0) {
        s.candidates = 1;
        disconnect_quietly();
    } else {
        static wx_wifi_seen_t seen[SCAN_MAX];
        size_t n = 0;
        disconnect_quietly();   // 正在连接时扫描不了
        err = scan_into(seen, SCAN_MAX, &n);
        uint8_t order[WX_WIFI_MAX];
        const size_t count = err == ESP_OK ? wx_wifi_rank(&list, seen, n, order) : 0;
        if (count > 0) {
            s.candidates = (uint8_t)count;
            pick = order[s.attempt % count];
        } else {
            // 一个都没扫到:可能是隐藏网络,按名字轮流直接连。
            s.candidates = list.count;
            pick = s.attempt % list.count;
        }
    }

    const wx_wifi_cred_t *cred = &list.items[pick];
    wifi_config_t config = { 0 };
    memcpy(config.sta.ssid, cred->ssid, strlen(cred->ssid));
    memcpy(config.sta.password, cred->password, strlen(cred->password));
    config.sta.threshold.authmode = cred->password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    xSemaphoreTake(s.driver_lock, portMAX_DELAY);
    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err == ESP_OK) err = esp_wifi_connect();
    xSemaphoreGive(s.driver_lock);

    set_state(WX_NET_CONNECTING, cred->ssid, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "发起连接失败: %s", esp_err_to_name(err));
        connection_failed(WX_TR("Could not connect", "连接失败"));
        return;
    }
    ESP_LOGI(TAG, "连接 %s(%u/%u)", cred->ssid, (unsigned)(s.attempt % s.candidates) + 1,
             (unsigned)s.candidates);
    schedule(DL_CONNECT_TIMEOUT, CONNECT_TIMEOUT_MS);
}

static void handle_reload(const char *prefer)
{
    if (!wx_settings_has_wifi()) {
        go_off();
        return;
    }
    if (prefer[0]) {
        s.attempt = 0;
        s.failures = 0;
        connect_next(prefer);
        return;
    }
    static wx_wifi_list_t list;
    wx_settings_wifi(&list);
    const bool current_saved = wx_wifi_list_find(&list, s.status.ssid) >= 0;
    if (s.status.state == WX_NET_OFF || s.status.state == WX_NET_FAILED || !current_saved) {
        s.failures = 0;
        connect_next(NULL);
    }
}

static void handle_disconnected(uint16_t reason)
{
    // 自己刚断开引起的事件。连上之后不会再有我们自己引起的断开,所以 CONNECTED 时不忽略。
    if (reason == WIFI_REASON_ASSOC_LEAVE && s.status.state != WX_NET_CONNECTED &&
        (int32_t)(s.leave_until - xTaskGetTickCount()) > 0) {
        return;
    }
    if (s.status.state == WX_NET_CONNECTED) {
        ESP_LOGW(TAG, "与 %s 的连接断开,原因 %u", s.status.ssid, reason);
        set_state(WX_NET_CONNECTING, NULL, NULL);
        schedule(DL_RETRY, RECONNECT_MS);
    } else if (s.status.state == WX_NET_CONNECTING) {
        connection_failed(reason_text(reason));
    }
}

static void handle_got_ip(void)
{
    // 只接受正在连接时的 IP:连接超时、我们已经断开之后才到的 IP 已经无效,重试会照常进行。
    if (s.status.state != WX_NET_CONNECTING) return;
    s.deadline_action = DL_NONE;
    s.failures = 0;
    s.online = true;
    wifi_ap_record_t info;
    const int8_t rssi = esp_wifi_sta_get_ap_info(&info) == ESP_OK ? info.rssi : 0;
    status_lock();
    s.status.rssi = rssi;
    status_unlock();
    set_state(WX_NET_CONNECTED, NULL, NULL);
    ESP_LOGI(TAG, "已连上 %s,信号 %d dBm,剩余内存 %u 字节", s.status.ssid, rssi,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

// ---------------------------------------------------------------------------
// 配网热点
// ---------------------------------------------------------------------------
static void make_setup_password(char *out)
{
    // 去掉容易认错的 0/o、1/l/i。
    static const char ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";
    for (size_t i = 0; i < WX_NET_SETUP_PASS_LEN; i++) {
        out[i] = ALPHABET[esp_random() % (sizeof(ALPHABET) - 1)];
    }
    out[WX_NET_SETUP_PASS_LEN] = '\0';
}

static void handle_setup_begin(void)
{
    esp_err_t err = ensure_wifi();
    char ssid[WX_WIFI_SSID_MAX + 1];
    char pass[WX_NET_SETUP_PASS_LEN + 1];
    uint8_t mac[6] = { 0 };
    (void)esp_read_mac(mac, ESP_MAC_BASE);    // 与蓝牙名后缀一致
    snprintf(ssid, sizeof(ssid), "Passport-%02X%02X", mac[4], mac[5]);
    make_setup_password(pass);

    wifi_config_t config = { 0 };
    memcpy(config.ap.ssid, ssid, strlen(ssid));
    config.ap.ssid_len = (uint8_t)strlen(ssid);
    memcpy(config.ap.password, pass, strlen(pass));
    config.ap.channel = 1;            // 已连上家里的网络时,驱动会改成那个网络的信道
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = 2;
    config.ap.pmf_cfg.required = false;

    if (err == ESP_OK) {
        xSemaphoreTake(s.driver_lock, portMAX_DELAY);
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &config);
        if (err == ESP_OK) err = ensure_started();
        xSemaphoreGive(s.driver_lock);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开配网热点失败: %s", esp_err_to_name(err));
        status_lock();
        s.status.setup_failed = true;
        status_unlock();
        notify();
        return;
    }

    status_lock();
    s.status.setup = true;
    s.status.setup_failed = false;
    s.status.setup_clients = 0;
    memcpy(s.status.setup_ssid, ssid, sizeof(ssid));
    memcpy(s.status.setup_pass, pass, sizeof(pass));
    status_unlock();
    ESP_LOGI(TAG, "配网热点 %s 已打开,剩余内存 %u 字节", ssid,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    notify();
}

static void handle_setup_end(void)
{
    if (!s.status.setup && !s.status.setup_failed) return;   // 已经关了(重复的关闭命令)
    if (s.inited) {
        xSemaphoreTake(s.driver_lock, portMAX_DELAY);
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
        xSemaphoreGive(s.driver_lock);
    }
    status_lock();
    s.status.setup = false;
    s.status.setup_failed = false;
    s.status.setup_clients = 0;
    s.status.setup_pass[0] = '\0';
    status_unlock();
    ESP_LOGI(TAG, "配网热点已关闭,剩余内存 %u 字节", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    if (!wx_settings_has_wifi()) {
        go_off();
    } else {
        notify();
    }
}

// ---------------------------------------------------------------------------
// 联网任务
// ---------------------------------------------------------------------------
static void on_deadline(void)
{
    const deadline_t action = s.deadline_action;
    s.deadline_action = DL_NONE;
    if (action == DL_RETRY) {
        connect_next(NULL);
    } else if (action == DL_CONNECT_TIMEOUT) {
        disconnect_quietly();
        connection_failed(WX_TR("Connection timed out", "连接超时"));
    }
}

static void net_task(void *arg)
{
    (void)arg;
    if (wx_settings_has_wifi()) connect_next(NULL);
    cmd_t cmd;
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (s.deadline_action != DL_NONE) {
            const int32_t left = (int32_t)(s.deadline - xTaskGetTickCount());
            wait = left > 0 ? (TickType_t)left : 0;
        }
        if (xQueueReceive(s.cmds, &cmd, wait) != pdTRUE) {
            on_deadline();
            continue;
        }
        switch (cmd.type) {
        case CMD_RELOAD:
            handle_reload(cmd.ssid);
            break;
        case CMD_DISCONNECTED:
            handle_disconnected(cmd.reason);
            break;
        case CMD_GOT_IP:
            handle_got_ip();
            break;
        case CMD_SETUP_BEGIN:
            handle_setup_begin();
            break;
        case CMD_SETUP_END:
            handle_setup_end();
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
esp_err_t wx_net_start(wx_net_cb_t cb, void *ctx)
{
    if (s.cmds) return ESP_ERR_INVALID_STATE;
    s.cb = cb;
    s.ctx = ctx;
    s.cmds = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(cmd_t));
    s.driver_lock = xSemaphoreCreateMutex();
    s.status_lock = xSemaphoreCreateMutex();
    if (!s.cmds || !s.driver_lock || !s.status_lock) return ESP_ERR_NO_MEM;
    if (xTaskCreate(net_task, "wx_net", 4096, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

static void post(const cmd_t *cmd)
{
    if (s.cmds && xQueueSend(s.cmds, cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "联网命令队列已满,丢弃命令 %d", cmd->type);
    }
}

void wx_net_reload(const char *prefer)
{
    cmd_t cmd = { .type = CMD_RELOAD };
    if (prefer) snprintf(cmd.ssid, sizeof(cmd.ssid), "%s", prefer);
    post(&cmd);
}

void wx_net_setup_begin(void)
{
    const cmd_t cmd = { .type = CMD_SETUP_BEGIN };
    post(&cmd);
}

void wx_net_setup_end(void)
{
    const cmd_t cmd = { .type = CMD_SETUP_END };
    post(&cmd);
}

void wx_net_status(wx_net_status_t *out)
{
    if (!s.status_lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    status_lock();
    *out = s.status;
    status_unlock();
}

bool wx_net_online(void)
{
    return s.online;
}

esp_err_t wx_net_scan(wx_wifi_seen_t *out, size_t cap, size_t *count)
{
    *count = 0;
    if (!s.inited || !s.started) return ESP_ERR_INVALID_STATE;
    if (s.status.state == WX_NET_CONNECTING) return ESP_ERR_INVALID_STATE;
    return scan_into(out, cap, count);
}
