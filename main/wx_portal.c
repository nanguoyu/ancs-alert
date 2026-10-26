// main/wx_portal.c —— 设置网页与它的 JSON 接口。
//
// 手机连上配网热点后会检测能否上网(iPhone 访问 captive.apple.com)。DNS 把所有域名都解析成
// 192.168.4.1,HTTP 服务器对未知地址一律重定向到首页,手机就会自动弹出设置网页。
// 网页里填的 Wi-Fi 密码和 Token 只存进 NVS,不会写进日志或回传给网页(Token 只回显打码后的样子)。
// 配网时板子可能同时连着家里的 Wi-Fi:只回应从配网热点(192.168.4.1)进来的请求,
// 家里局域网里的其他设备访问不了设置接口。
#include "wx_portal.h"
#include "wx_apps.h"
#include "wx_dns.h"
#include "wx_net.h"
#include "wx_settings.h"
#include "wx_tg.h"
#include "wx_tg_text.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "wx_portal";

#define BODY_MAX        768
#define RECV_RETRIES    2           // 每次最多等 5 s(httpd 默认),手机掉线时不会一直等
#define SCAN_CAP        20
#define DNS_PORT        53
#define DNS_BUF         512
#define PORTAL_ROOT     "http://192.168.4.1/"

extern const uint8_t s_page_start[] asm("_binary_setup_html_start");
extern const uint8_t s_page_end[] asm("_binary_setup_html_end");

static const uint8_t AP_IP[4] = { 192, 168, 4, 1 };

static struct {
    httpd_handle_t server;
    TaskHandle_t dns_task;
    volatile bool dns_stop;
    wx_portal_done_cb_t done;
    void *ctx;
} s;

// ---------------------------------------------------------------------------
// DNS
// ---------------------------------------------------------------------------
static void dns_task(void *arg)
{
    (void)arg;
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    // 只在热点地址上监听,不回答家里局域网的查询。
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
    };
    memcpy(&addr.sin_addr.s_addr, AP_IP, 4);
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "DNS 无法监听 53 端口: errno %d", errno);
        if (sock >= 0) close(sock);
        s.dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    // 每秒醒一次检查是否该退出。
    const struct timeval timeout = { .tv_sec = 1 };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    static uint8_t buf[DNS_BUF];
    while (!s.dns_stop) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) continue;
        const size_t reply = wx_dns_reply(buf, (size_t)n, sizeof(buf), AP_IP);
        if (reply > 0) (void)sendto(sock, buf, reply, 0, (struct sockaddr *)&from, from_len);
    }
    close(sock);
    s.dns_task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// HTTP 工具
// ---------------------------------------------------------------------------
static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *text = root ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (!text) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

static esp_err_t send_result(httpd_req_t *req, const char *error)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", error == NULL);
    if (error) cJSON_AddStringToObject(root, "error", error);
    return send_json(req, root);
}

// 读请求体并解析成 JSON。失败时已回复错误,返回 NULL。
static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len > BODY_MAX) {
        send_result(req, WX_TR("Request too long", "请求太长"));
        return NULL;
    }
    char *body = malloc(req->content_len + 1);
    if (!body) {
        send_result(req, WX_TR("Out of memory", "内存不足"));
        return NULL;
    }
    size_t got = 0;
    int retries = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && retries++ < RECV_RETRIES) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    body[got] = '\0';
    cJSON *root = got == req->content_len ? cJSON_Parse(body) : NULL;
    // 请求体里可能有密码或 Token:用完立即清掉。
    memset(body, 0, got);
    free(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        send_result(req, WX_TR("Malformed request", "请求格式不对"));
        return NULL;
    }
    return root;
}

static const char *json_str(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

// 清掉 JSON 里的敏感字符串再释放。
static void delete_secret_json(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item)) memset(item->valuestring, 0, strlen(item->valuestring));
    cJSON_Delete(root);
}

static cJSON *app_mask_array(uint8_t mask)
{
    cJSON *array = cJSON_CreateArray();
    for (uint8_t app = 0; app < WX_APP_COUNT; app++) {
        if (mask & WX_APP_BIT(app)) cJSON_AddItemToArray(array, cJSON_CreateNumber(app));
    }
    return array;
}

static uint8_t app_mask_from_json(const cJSON *array)
{
    uint8_t mask = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, array) {
        if (cJSON_IsNumber(item) && item->valueint >= 0 && item->valueint < WX_APP_COUNT) {
            mask |= WX_APP_BIT(item->valueint);
        }
    }
    return mask;
}

// 请求是不是从配网热点进来的:看这条连接在板子这端的地址。httpd 用的是 IPv6 套接字
// (IPv4 连接表现为 ::ffff:a.b.c.d),两种都认。
static bool from_setup_hotspot(httpd_req_t *req)
{
    struct sockaddr_storage local;
    socklen_t len = sizeof(local);
    const int fd = httpd_req_to_sockfd(req);
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&local, &len) != 0) return false;
    if (local.ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)&local;
        return memcmp(&a->sin_addr.s_addr, AP_IP, 4) == 0;
    }
#if CONFIG_LWIP_IPV6
    if (local.ss_family == AF_INET6) {
        static const uint8_t V4_MAPPED[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
        const uint8_t *b = ((const struct sockaddr_in6 *)&local)->sin6_addr.s6_addr;
        return memcmp(b, V4_MAPPED, 12) == 0 && memcmp(b + 12, AP_IP, 4) == 0;
    }
#endif
    return false;
}

// ---------------------------------------------------------------------------
// 接口
// ---------------------------------------------------------------------------
static const char *net_state_name(wx_net_state_t state)
{
    switch (state) {
    case WX_NET_CONNECTING: return "connecting";
    case WX_NET_CONNECTED: return "connected";
    case WX_NET_FAILED: return "failed";
    case WX_NET_OFF:
    default: return "off";
    }
}

static esp_err_t get_state(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return send_json(req, NULL);
    cJSON_AddStringToObject(root, "lang", wx_lang_code(wx_lang()));

    wx_net_status_t net;
    wx_net_status(&net);
    static wx_wifi_list_t list;   // 网页请求在 httpd 任务里串行处理
    wx_settings_wifi(&list);
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "state", net_state_name(net.state));
    cJSON_AddStringToObject(wifi, "ssid", net.ssid);
    cJSON_AddStringToObject(wifi, "ip", net.ip);
    cJSON_AddNumberToObject(wifi, "rssi", net.rssi);
    if (net.error) cJSON_AddStringToObject(wifi, "error", net.error);
    cJSON_AddNumberToObject(wifi, "max", WX_WIFI_MAX);
    cJSON *saved = cJSON_AddArrayToObject(wifi, "saved");
    for (size_t i = 0; i < list.count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", list.items[i].ssid);
        cJSON_AddBoolToObject(item, "open", list.items[i].password[0] == '\0');
        cJSON_AddItemToArray(saved, item);
    }

    wx_tg_config_t config;
    wx_settings_tg(&config);
    wx_tg_stats_t stats;
    wx_tg_stats(&stats);
    char hint[WX_TG_TOKEN_MAX + 8];
    wx_tg_token_hint(config.token, hint, sizeof(hint));
    cJSON *tg = cJSON_AddObjectToObject(root, "tg");
    cJSON_AddStringToObject(tg, "token", hint);
    cJSON_AddStringToObject(tg, "bot", config.bot_name);
    cJSON_AddStringToObject(tg, "user", config.bot_user);
    cJSON_AddStringToObject(tg, "chat", config.chat_name);
    cJSON_AddStringToObject(tg, "chat_id", config.chat_id);
    cJSON_AddStringToObject(tg, "api", config.api_base);
    cJSON_AddBoolToObject(tg, "body", config.body);
    cJSON_AddItemToObject(tg, "fwd", app_mask_array(config.forward_mask));
    cJSON_AddItemToObject(tg, "fwd_allowed", app_mask_array(WX_TG_FORWARD_ALLOWED));
    cJSON_AddNumberToObject(tg, "sent", stats.sent);
    cJSON_AddNumberToObject(tg, "pending", stats.pending);
    if (stats.last_error) cJSON_AddStringToObject(tg, "error", stats.last_error);

    cJSON_AddItemToObject(root, "alert", app_mask_array(wx_settings_alert_mask()));
    cJSON *apps = cJSON_AddArrayToObject(root, "apps");
    for (uint8_t app = 0; app < WX_APP_COUNT; app++) {
        cJSON_AddItemToArray(apps, cJSON_CreateString(wx_app_name(app, wx_lang())));
    }
    return send_json(req, root);
}

static esp_err_t get_scan(httpd_req_t *req)
{
    wx_wifi_seen_t *aps = calloc(SCAN_CAP, sizeof(*aps));
    if (!aps) return send_result(req, WX_TR("Out of memory", "内存不足"));
    size_t n = 0;
    const esp_err_t err = wx_net_scan(aps, SCAN_CAP, &n);
    if (err != ESP_OK) {
        free(aps);
        return send_result(req, err == ESP_ERR_INVALID_STATE ? WX_TR("The device is connecting. Scan again in a few seconds", "板子正在连接网络，请过几秒再扫描") : WX_TR("Scan failed", "扫描失败"));
    }
    static wx_wifi_list_t list;
    wx_settings_wifi(&list);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(aps);
        return send_json(req, NULL);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *array = cJSON_AddArrayToObject(root, "aps");
    for (size_t i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(item, "secure", aps[i].secure);
        cJSON_AddBoolToObject(item, "saved", wx_wifi_list_find(&list, aps[i].ssid) >= 0);
        cJSON_AddItemToArray(array, item);
    }
    free(aps);
    return send_json(req, root);
}

static esp_err_t post_wifi(httpd_req_t *req)
{
    cJSON *body = read_json(req);
    if (!body) return ESP_OK;
    const char *ssid = json_str(body, "ssid");
    const char *password = json_str(body, "password");
    esp_err_t saved = ESP_OK;
    const wx_wifi_put_t result = wx_settings_wifi_put(ssid, password ? password : "", &saved);
    const char *error = NULL;
    switch (result) {
    case WX_WIFI_ADDED:
    case WX_WIFI_REPLACED:
        if (saved != ESP_OK) error = WX_TR("Could not save", "保存失败");
        break;
    case WX_WIFI_FULL: error = WX_TR("5 networks saved already. Delete one first", "已经存了 5 个网络，请先删掉一个"); break;
    case WX_WIFI_BAD_SSID: error = WX_TR("Network name must be 1 to 32 bytes", "网络名称要 1 到 32 个字节"); break;
    case WX_WIFI_BAD_PASSWORD: error = WX_TR("Password must be 8 to 63 ASCII characters (leave empty for open networks)", "密码要 8 到 63 个英文字符（开放网络留空）"); break;
    }
    if (!error) {
        ESP_LOGI(TAG, "%s Wi-Fi 网络 %s", result == WX_WIFI_ADDED ? "添加" : "更新", ssid);
        wx_net_reload(ssid);
    }
    delete_secret_json(body, "password");
    return send_result(req, error);
}

static esp_err_t post_wifi_delete(httpd_req_t *req)
{
    cJSON *body = read_json(req);
    if (!body) return ESP_OK;
    const esp_err_t err = wx_settings_wifi_remove(json_str(body, "ssid"));
    if (err == ESP_OK) wx_net_reload(NULL);
    cJSON_Delete(body);
    return send_result(req, err == ESP_OK ? NULL : err == ESP_ERR_NOT_FOUND ? WX_TR("No such network", "没有这个网络") : WX_TR("Could not save", "保存失败"));
}

// URL 里允许的字符。HTTP 客户端解析失败时会把整个 URL(含 Token)打进错误日志,
// 所以只放行普通网址用得到的字符。
static bool url_char_ok(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           strchr(".-_~:/%", c) != NULL;
}

// API 地址:去掉首尾空白和末尾的 "/";空串表示用默认地址。只接受 https,Token 和消息不走明文。
// 返回 false 表示格式不对。
static bool normalize_api(const char *in, char *out, size_t cap)
{
    if (!in) in = "";
    while (*in == ' ') in++;
    if (strlen(in) > WX_TG_API_MAX) return false;
    snprintf(out, cap, "%s", in[0] ? in : WX_TG_API_DEFAULT);
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '/' || out[len - 1] == ' ')) out[--len] = '\0';
    if (strncmp(out, "https://", 8) != 0 || len <= 8) return false;
    for (size_t i = 0; i < len; i++) {
        if (!url_char_ok(out[i])) return false;
    }
    return true;
}

static esp_err_t post_tg_token(httpd_req_t *req)
{
    cJSON *body = read_json(req);
    if (!body) return ESP_OK;
    static wx_tg_config_t config;
    wx_settings_tg(&config);

    const char *token = json_str(body, "token");
    const bool new_token = token && token[0];
    if (!new_token) token = config.token;   // 只改 API 地址:用已保存的 Token 验证
    char api[WX_TG_API_MAX + 1];
    const char *error = NULL;
    if (!normalize_api(json_str(body, "api"), api, sizeof(api))) {
        error = WX_TR("The API address must start with https:// and contain only letters, digits and . - _ ~ : / %", "API 地址要以 https:// 开头，只能含字母、数字和 . - _ ~ : / %");
    } else if (!wx_tg_token_valid(token)) {
        error = new_token ? WX_TR("That does not look like a token. It should look like 123456789:AAH…", "Token 格式不对：应形如 123456789:AAH…") : WX_TR("Enter the bot token", "请填写机器人 Token");
    }

    char bot_name[WX_TG_NAME_MAX + 1] = "";
    char bot_user[WX_TG_NAME_MAX + 1] = "";
    if (!error && wx_tg_check_token(api, token, bot_name, sizeof(bot_name),
                                    bot_user, sizeof(bot_user), &error) == ESP_OK) {
        if (new_token && strcmp(token, config.token) != 0) {
            // 换了机器人:原来绑定的聊天属于旧机器人,作废。
            config.chat_id[0] = '\0';
            config.chat_name[0] = '\0';
            snprintf(config.token, sizeof(config.token), "%s", token);
        }
        snprintf(config.api_base, sizeof(config.api_base), "%s", api);
        snprintf(config.bot_name, sizeof(config.bot_name), "%s", bot_name);
        snprintf(config.bot_user, sizeof(config.bot_user), "%s", bot_user);
        if (wx_settings_set_tg(&config) != ESP_OK) error = WX_TR("Could not save", "保存失败");
        ESP_LOGI(TAG, "已保存机器人 @%s", bot_user);
    }
    memset(config.token, 0, sizeof(config.token));
    delete_secret_json(body, "token");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", error == NULL);
    if (error) {
        cJSON_AddStringToObject(root, "error", error);
    } else {
        cJSON_AddStringToObject(root, "bot", bot_name);
        cJSON_AddStringToObject(root, "user", bot_user);
    }
    return send_json(req, root);
}

static bool chat_id_valid(const char *id)
{
    if (!id || !id[0] || strlen(id) > WX_TG_CHAT_ID_MAX) return false;
    for (const char *p = id[0] == '-' ? id + 1 : id; *p; p++) {
        if (*p < '0' || *p > '9') return false;
    }
    return id[0] != '-' || id[1] != '\0';
}

static esp_err_t post_tg_bind(httpd_req_t *req)
{
    cJSON *body = read_json(req);
    if (!body) return ESP_OK;
    const char *manual = json_str(body, "chat_id");
    char chat_id[WX_TG_CHAT_ID_MAX + 1] = "";
    char chat_name[WX_TG_NAME_MAX + 1] = "";
    const char *error = NULL;
    if (manual && manual[0]) {
        if (chat_id_valid(manual)) {
            snprintf(chat_id, sizeof(chat_id), "%s", manual);
            snprintf(chat_name, sizeof(chat_name), WX_TR("Chat %s", "聊天 %s"), manual);
        } else {
            error = WX_TR("A chat ID is digits only (groups start with -)", "聊天 ID 只能是数字（群组以 - 开头）");
        }
    } else {
        (void)wx_tg_find_chat(chat_id, sizeof(chat_id), chat_name, sizeof(chat_name), &error);
    }
    cJSON_Delete(body);

    if (!error) {
        static wx_tg_config_t config;
        wx_settings_tg(&config);
        snprintf(config.chat_id, sizeof(config.chat_id), "%s", chat_id);
        snprintf(config.chat_name, sizeof(config.chat_name), "%s", chat_name);
        if (wx_settings_set_tg(&config) != ESP_OK) error = WX_TR("Could not save", "保存失败");
        memset(config.token, 0, sizeof(config.token));
        ESP_LOGI(TAG, "已绑定 Telegram 聊天 %s", chat_id);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", error == NULL);
    if (error) {
        cJSON_AddStringToObject(root, "error", error);
    } else {
        cJSON_AddStringToObject(root, "chat", chat_name);
    }
    return send_json(req, root);
}

static esp_err_t post_tg_options(httpd_req_t *req)
{
    cJSON *body = read_json(req);
    if (!body) return ESP_OK;
    static wx_tg_config_t config;
    wx_settings_tg(&config);
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(body, "body");
    if (cJSON_IsBool(b)) config.body = cJSON_IsTrue(b);
    const cJSON *fwd = cJSON_GetObjectItemCaseSensitive(body, "fwd");
    if (cJSON_IsArray(fwd)) config.forward_mask = app_mask_from_json(fwd);
    cJSON_Delete(body);
    const esp_err_t err = wx_settings_set_tg(&config);
    memset(config.token, 0, sizeof(config.token));
    return send_result(req, err == ESP_OK ? NULL : WX_TR("Could not save", "保存失败"));
}

static esp_err_t post_tg_test(httpd_req_t *req)
{
    const char *error = NULL;
    (void)wx_tg_send_test(&error);
    return send_result(req, error);
}

static esp_err_t post_tg_clear(httpd_req_t *req)
{
    static wx_tg_config_t config;
    wx_settings_tg(&config);
    const bool body = config.body;
    const uint8_t fwd = config.forward_mask;
    char api[WX_TG_API_MAX + 1];
    snprintf(api, sizeof(api), "%s", config.api_base);
    memset(&config, 0, sizeof(config));
    config.body = body;
    config.forward_mask = fwd;
    snprintf(config.api_base, sizeof(config.api_base), "%s", api);
    const esp_err_t err = wx_settings_set_tg(&config);
    ESP_LOGI(TAG, "已清除 Telegram 设置");
    return send_result(req, err == ESP_OK ? NULL : WX_TR("Could not save", "保存失败"));
}

static esp_err_t post_alert(httpd_req_t *req)
{
    cJSON *body = read_json(req);
    if (!body) return ESP_OK;
    const cJSON *apps = cJSON_GetObjectItemCaseSensitive(body, "apps");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsArray(apps)) err = wx_settings_set_alert_mask(app_mask_from_json(apps));
    cJSON_Delete(body);
    return send_result(req, err == ESP_OK ? NULL : WX_TR("Could not save", "保存失败"));
}

// 网页切换语言:板子界面、之后的转发文字和人声都跟着变。
static esp_err_t post_lang(httpd_req_t *req)
{
    cJSON *body = read_json(req);
    if (!body) return ESP_OK;
    const char *code = json_str(body, "lang");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    for (int lang = 0; code && lang < WX_LANG_COUNT; lang++) {
        if (strcmp(code, wx_lang_code((wx_lang_t)lang)) == 0) err = wx_settings_set_lang((wx_lang_t)lang);
    }
    cJSON_Delete(body);
    return send_result(req, err == ESP_OK ? NULL : WX_TR("Could not save", "保存失败"));
}

static esp_err_t post_done(httpd_req_t *req)
{
    const esp_err_t err = send_result(req, NULL);
    if (s.done) s.done(s.ctx);
    return err;
}

// 首页;其他地址(手机的联网检测等)一律重定向到首页,手机就会弹出设置网页。
static esp_err_t get_any(httpd_req_t *req)
{
    if (strcmp(req->uri, "/") == 0 || strncmp(req->uri, "/?", 2) == 0 ||
        strcmp(req->uri, "/index.html") == 0) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, (const char *)s_page_start, s_page_end - s_page_start);
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", PORTAL_ROOT);
    return httpd_resp_send(req, NULL, 0);
}

// ---------------------------------------------------------------------------
// 启停
// ---------------------------------------------------------------------------
typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *req);
} route_t;

static esp_err_t dispatch(httpd_req_t *req)
{
    const route_t *route = req->user_ctx;
    if (!from_setup_hotspot(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "setup is only available on the Passport hotspot");
        return ESP_OK;
    }
    return route->handler(req);
}

static const route_t ROUTES[] = {
    { "/api/state", HTTP_GET, get_state },
    { "/api/scan", HTTP_GET, get_scan },
    { "/api/wifi", HTTP_POST, post_wifi },
    { "/api/wifi/delete", HTTP_POST, post_wifi_delete },
    { "/api/tg/token", HTTP_POST, post_tg_token },
    { "/api/tg/bind", HTTP_POST, post_tg_bind },
    { "/api/tg/options", HTTP_POST, post_tg_options },
    { "/api/tg/test", HTTP_POST, post_tg_test },
    { "/api/tg/clear", HTTP_POST, post_tg_clear },
    { "/api/alert", HTTP_POST, post_alert },
    { "/api/lang", HTTP_POST, post_lang },
    { "/api/done", HTTP_POST, post_done },
    { "/*", HTTP_GET, get_any },              // 必须最后注册:通配符会匹配所有地址
};

esp_err_t wx_portal_start(wx_portal_done_cb_t done, void *ctx)
{
    if (s.server) return ESP_OK;
    s.done = done;
    s.ctx = ctx;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.max_uri_handlers = sizeof(ROUTES) / sizeof(ROUTES[0]);
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&s.server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "网页服务启动失败: %s", esp_err_to_name(err));
        s.server = NULL;
        return err;
    }
    for (size_t i = 0; i < sizeof(ROUTES) / sizeof(ROUTES[0]); i++) {
        const httpd_uri_t uri = {
            .uri = ROUTES[i].uri, .method = ROUTES[i].method, .handler = dispatch,
            .user_ctx = (void *)&ROUTES[i],
        };
        (void)httpd_register_uri_handler(s.server, &uri);
    }

    s.dns_stop = false;
    if (xTaskCreate(dns_task, "wx_dns", 3072, NULL, 4, &s.dns_task) != pdPASS) {
        s.dns_task = NULL;
        ESP_LOGW(TAG, "DNS 任务创建失败;手机不会自动弹出网页,需要手动打开 %s", PORTAL_ROOT);
    }
    ESP_LOGI(TAG, "设置网页已启动:%s", PORTAL_ROOT);
    return ESP_OK;
}

void wx_portal_stop(void)
{
    if (s.server) {
        (void)httpd_stop(s.server);
        s.server = NULL;
    }
    if (s.dns_task) {
        s.dns_stop = true;
        // DNS 任务最多 1 秒后醒来并退出。
        for (int i = 0; i < 30 && s.dns_task; i++) vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "设置网页已关闭");
}
