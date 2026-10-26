// main/wx_settings.c —— 保存在 NVS 里的设置。
//
// 每组设置是一个带版本号的 blob;读出来的长度、版本或内容不对就用默认值(例如旧固件留下的数据)。
#include "wx_settings.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "wx_settings";

#define NVS_NAMESPACE "wx_alert"
#define KEY_WIFI      "wifi"
#define KEY_TG        "tg"
#define KEY_ALERTS    "alerts"
#define KEY_MUTED     "muted"      // 与旧版本相同,升级后保留静音设置
#define KEY_LANG      "lang"
#define BLOB_VERSION  1

typedef struct {
    uint8_t version;
    wx_wifi_list_t list;
} wifi_blob_t;

typedef struct {
    uint8_t version;
    wx_tg_config_t config;
} tg_blob_t;

static SemaphoreHandle_t s_lock;
static bool s_nvs_ok;
static wx_wifi_list_t s_wifi;
static wx_tg_config_t s_tg;
static uint8_t s_alerts = WX_APP_ALL_BITS;
static bool s_muted;
// 转发判断会在 BLE 回调里频繁调用:单字节缓存,读取无需加锁。语言同理。
static volatile uint8_t s_forward_bits;
static volatile uint8_t s_lang = WX_LANG_EN;

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static bool tg_ready_locked(void)
{
    return wx_tg_token_valid(s_tg.token) && s_tg.chat_id[0] != '\0';
}

static void update_forward_bits_locked(void)
{
    s_forward_bits = tg_ready_locked() ? (s_tg.forward_mask & WX_TG_FORWARD_ALLOWED) : 0;
}

static void tg_defaults(wx_tg_config_t *c)
{
    memset(c, 0, sizeof(*c));
    strcpy(c->api_base, WX_TG_API_DEFAULT);
    c->body = true;
    c->forward_mask = WX_TG_FORWARD_DEFAULT;
}

static bool terminated(const char *s, size_t cap)
{
    return memchr(s, '\0', cap) != NULL;
}

static bool tg_valid(const wx_tg_config_t *c)
{
    return terminated(c->token, sizeof(c->token)) && terminated(c->bot_name, sizeof(c->bot_name)) &&
           terminated(c->bot_user, sizeof(c->bot_user)) && terminated(c->chat_id, sizeof(c->chat_id)) &&
           terminated(c->chat_name, sizeof(c->chat_name)) &&
           terminated(c->api_base, sizeof(c->api_base)) && strncmp(c->api_base, "https://", 8) == 0;
}

static esp_err_t save_blob(const char *key, const void *data, size_t len)
{
    if (!s_nvs_ok) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, key, data, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "保存 %s 失败: %s", key, esp_err_to_name(err));
    return err;
}

static esp_err_t save_u8(const char *key, uint8_t value)
{
    if (!s_nvs_ok) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "保存 %s 失败: %s", key, esp_err_to_name(err));
    return err;
}

static bool load_blob(nvs_handle_t h, const char *key, void *data, size_t len)
{
    size_t got = len;
    return nvs_get_blob(h, key, data, &got) == ESP_OK && got == len;
}

static void load_all(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;   // 第一次开机还没有命名空间

    static wifi_blob_t wifi;   // 较大,不放栈上
    if (load_blob(h, KEY_WIFI, &wifi, sizeof(wifi))) {
        if (wifi.version == BLOB_VERSION && wx_wifi_list_valid(&wifi.list)) {
            s_wifi = wifi.list;
        } else {
            ESP_LOGW(TAG, "Wi-Fi 设置格式不对,忽略");
        }
    }
    static tg_blob_t tg;
    if (load_blob(h, KEY_TG, &tg, sizeof(tg))) {
        if (tg.version == BLOB_VERSION && tg_valid(&tg.config)) {
            s_tg = tg.config;
        } else {
            ESP_LOGW(TAG, "Telegram 设置格式不对,忽略");
        }
    }
    uint8_t value;
    if (nvs_get_u8(h, KEY_ALERTS, &value) == ESP_OK) s_alerts = value & WX_APP_ALL_BITS;
    if (nvs_get_u8(h, KEY_MUTED, &value) == ESP_OK) s_muted = value != 0;
    if (nvs_get_u8(h, KEY_LANG, &value) == ESP_OK && value < WX_LANG_COUNT) s_lang = value;
    nvs_close(h);
}

esp_err_t wx_settings_init(bool nvs_ok)
{
    if (s_lock) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    s_nvs_ok = nvs_ok;
    wx_wifi_list_init(&s_wifi);
    tg_defaults(&s_tg);
    if (nvs_ok) load_all();
    update_forward_bits_locked();
    ESP_LOGI(TAG, "已保存 %u 个 Wi-Fi;Telegram %s;提醒应用 0x%02x;提示音%s;语言 %s",
             s_wifi.count, tg_ready_locked() ? "已设置" : "未设置", s_alerts, s_muted ? "关" : "开",
             wx_lang_code((wx_lang_t)s_lang));
    return ESP_OK;
}

void wx_settings_wifi(wx_wifi_list_t *out)
{
    lock();
    *out = s_wifi;
    unlock();
}

bool wx_settings_has_wifi(void)
{
    lock();
    const bool any = s_wifi.count > 0;
    unlock();
    return any;
}

static esp_err_t save_wifi_locked(void)
{
    static wifi_blob_t blob;
    blob.version = BLOB_VERSION;
    blob.list = s_wifi;
    return save_blob(KEY_WIFI, &blob, sizeof(blob));
}

wx_wifi_put_t wx_settings_wifi_put(const char *ssid, const char *password, esp_err_t *saved)
{
    lock();
    const wx_wifi_put_t result = wx_wifi_list_put(&s_wifi, ssid, password);
    esp_err_t err = ESP_OK;
    if (result == WX_WIFI_ADDED || result == WX_WIFI_REPLACED) err = save_wifi_locked();
    unlock();
    if (saved) *saved = err;
    return result;
}

esp_err_t wx_settings_wifi_remove(const char *ssid)
{
    lock();
    esp_err_t err = wx_wifi_list_remove(&s_wifi, ssid) ? save_wifi_locked() : ESP_ERR_NOT_FOUND;
    unlock();
    return err;
}

esp_err_t wx_settings_wifi_clear(void)
{
    lock();
    wx_wifi_list_init(&s_wifi);
    esp_err_t err = save_wifi_locked();
    unlock();
    return err;
}

void wx_settings_tg(wx_tg_config_t *out)
{
    lock();
    *out = s_tg;
    unlock();
}

esp_err_t wx_settings_set_tg(const wx_tg_config_t *config)
{
    if (!tg_valid(config)) return ESP_ERR_INVALID_ARG;
    lock();
    s_tg = *config;
    s_tg.forward_mask &= WX_TG_FORWARD_ALLOWED;
    update_forward_bits_locked();
    static tg_blob_t blob;
    blob.version = BLOB_VERSION;
    blob.config = s_tg;
    esp_err_t err = save_blob(KEY_TG, &blob, sizeof(blob));
    unlock();
    return err;
}

bool wx_settings_tg_ready(void)
{
    lock();
    const bool ready = tg_ready_locked();
    unlock();
    return ready;
}

bool wx_settings_forwards(uint8_t app)
{
    return app < WX_APP_COUNT && (s_forward_bits & WX_APP_BIT(app)) != 0;
}

uint8_t wx_settings_alert_mask(void)
{
    lock();
    const uint8_t mask = s_alerts;
    unlock();
    return mask;
}

esp_err_t wx_settings_set_alert_mask(uint8_t mask)
{
    lock();
    s_alerts = mask & WX_APP_ALL_BITS;
    esp_err_t err = save_u8(KEY_ALERTS, s_alerts);
    unlock();
    return err;
}

bool wx_settings_muted(void)
{
    lock();
    const bool muted = s_muted;
    unlock();
    return muted;
}

esp_err_t wx_settings_set_muted(bool muted)
{
    lock();
    s_muted = muted;
    esp_err_t err = save_u8(KEY_MUTED, muted ? 1 : 0);
    unlock();
    return err;
}

wx_lang_t wx_lang(void)
{
    return (wx_lang_t)s_lang;
}

esp_err_t wx_settings_set_lang(wx_lang_t lang)
{
    if (lang >= WX_LANG_COUNT) return ESP_ERR_INVALID_ARG;
    lock();
    s_lang = (uint8_t)lang;
    esp_err_t err = save_u8(KEY_LANG, s_lang);
    unlock();
    return err;
}
