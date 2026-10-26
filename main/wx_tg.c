// main/wx_tg.c —— Telegram 机器人客户端。
//
// 一个任务处理全部请求:网页的同步请求(验证 Token、绑定聊天、测试)和排队的转发。
// 每个请求新建一条 HTTPS 连接,发完就断,TLS 的大块缓冲随即释放,平时不占内存。
// 转发失败时区分两类:Telegram 明确拒收(Token 错、聊天不存在等)直接放弃;
// 网络问题或服务器繁忙则按 5 s、10 s、20 s … 最长 5 分钟重试,最多 12 次。
#include "wx_tg.h"
#include "wx_apps.h"
#include "wx_ancs_proto.h"
#include "wx_net.h"
#include "wx_phone.h"
#include "wx_settings.h"
#include "wx_tg_text.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "wx_tg";

#define TASK_STACK          8192
#define JOB_QUEUE_DEPTH     8
#define PENDING_MAX         8
#define HTTP_TIMEOUT_MS     15000
#define RESP_CAP            2048
#define UPDATES_RESP_CAP    6144
#define RETRY_FIRST_MS      5000
#define RETRY_MAX_MS        (5 * 60 * 1000)
#define RETRY_LIMIT         12
#define OFFLINE_POLL_MS     5000
#define TEXT_MAX            (WX_ANCS_TITLE_MAX + WX_ANCS_MESSAGE_MAX + 64)

typedef enum {
    JOB_FORWARD = 0,
    JOB_CHECK_TOKEN,
    JOB_FIND_CHAT,
    JOB_TEST,
} job_type_t;

typedef struct {
    job_type_t type;

    // JOB_FORWARD
    uint8_t app;
    char phone[WX_PHONE_NAME_MAX + 1];
    char title[WX_ANCS_TITLE_MAX + 1];
    char *message;
    uint8_t tries;
    TickType_t next_try;

    // 同步请求:调用者在自己的栈上建好,等 done
    const char *api_base;
    const char *token;
    char *out1;
    size_t cap1;
    char *out2;
    size_t cap2;
    const char *error;
    esp_err_t result;
    SemaphoreHandle_t done;
} job_t;

typedef enum {
    SEND_OK = 0,
    SEND_REJECTED,      // Telegram 明确拒收,重试也没用
    SEND_RETRY,         // 网络或服务器问题,稍后重试
} send_result_t;

static struct {
    wx_tg_cb_t cb;
    void *ctx;
    QueueHandle_t jobs;
    SemaphoreHandle_t stats_lock;
    wx_tg_stats_t stats;
    job_t *pending[PENDING_MAX];   // 只在 Telegram 任务里访问
    size_t pending_count;
    bool stack_reported;
} s;

// ---------------------------------------------------------------------------
// 统计
// ---------------------------------------------------------------------------
static void update_stats(int sent, int dropped, const char *error, bool clear_error)
{
    xSemaphoreTake(s.stats_lock, portMAX_DELAY);
    s.stats.sent += (uint32_t)sent;
    s.stats.dropped += (uint32_t)dropped;
    s.stats.pending = (uint8_t)s.pending_count;
    if (error) s.stats.last_error = error;
    if (clear_error) s.stats.last_error = NULL;
    xSemaphoreGive(s.stats_lock);
    if (s.cb) s.cb(s.ctx);
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
typedef struct {
    esp_err_t err;          // 网络层结果
    int status;             // HTTP 状态码
    char *body;             // malloc 出来的响应,调用者释放
} http_resp_t;

static http_resp_t api_call(const char *api_base, const char *token, const char *method,
                            const char *json, size_t cap)
{
    http_resp_t r = { .err = ESP_ERR_NO_MEM };
    char url[WX_TG_API_MAX + WX_TG_TOKEN_MAX + 32];
    snprintf(url, sizeof(url), "%s/bot%s/%s", api_base, token, method);

    const esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return r;
    (void)esp_http_client_set_header(client, "Content-Type", "application/json");

    const size_t len = strlen(json);
    r.err = esp_http_client_open(client, (int)len);
    if (r.err == ESP_OK && esp_http_client_write(client, json, (int)len) != (int)len) r.err = ESP_FAIL;
    if (r.err == ESP_OK && esp_http_client_fetch_headers(client) < 0) r.err = ESP_FAIL;
    // 响应缓冲等握手完、拿到响应头再分配,不和 TLS 握手抢内存。
    if (r.err == ESP_OK) {
        r.body = malloc(cap);
        if (!r.body) r.err = ESP_ERR_NO_MEM;
    }
    if (r.err == ESP_OK) {
        r.status = esp_http_client_get_status_code(client);
        size_t got = 0;
        while (got + 1 < cap) {
            const int n = esp_http_client_read(client, r.body + got, (int)(cap - 1 - got));
            if (n <= 0) break;
            got += (size_t)n;
        }
        r.body[got] = '\0';
    }
    (void)esp_http_client_close(client);
    (void)esp_http_client_cleanup(client);

    if (!s.stack_reported && r.err == ESP_OK) {
        // 第一次成功的 HTTPS 请求后记一下栈余量和内存,方便调 TASK_STACK。
        s.stack_reported = true;
        ESP_LOGI(TAG, "HTTPS 完成;任务栈剩余 %u 字节,剩余内存 %u,最低时 %u",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    }
    return r;
}

// 解析 Telegram 的回复:{"ok":true,"result":...} 或 {"ok":false,"error_code":400,"description":"..."}。
// 成功时返回 result(属于 *root,调用者 cJSON_Delete(*root))。失败时 *error 为中文原因。
static cJSON *parse_result(const http_resp_t *r, cJSON **root, send_result_t *kind, const char **error)
{
    *root = NULL;
    if (r->err != ESP_OK) {
        ESP_LOGW(TAG, "连不上 Telegram: %s", esp_err_to_name(r->err));
        *kind = SEND_RETRY;
        *error = r->err == ESP_ERR_NO_MEM ? WX_TR("Out of memory", "内存不足") : WX_TR("Cannot reach Telegram: no internet, or it is blocked", "连不上 Telegram，网络不通或被拦截");
        return NULL;
    }
    *root = r->body ? cJSON_Parse(r->body) : NULL;
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(*root, "ok");
    if (cJSON_IsTrue(ok)) {
        *kind = SEND_OK;
        *error = NULL;
        return cJSON_GetObjectItemCaseSensitive(*root, "result");
    }
    const cJSON *desc = cJSON_GetObjectItemCaseSensitive(*root, "description");
    // 描述来自 Telegram,不含 Token 或消息内容,可以记日志。
    ESP_LOGW(TAG, "Telegram 返回 HTTP %d: %s", r->status,
             cJSON_IsString(desc) ? desc->valuestring : "(无说明)");
    *kind = SEND_REJECTED;
    switch (r->status) {
    case 401:
    case 404:
        *error = WX_TR("Telegram rejected this token", "Token 不对，Telegram 不认");
        break;
    case 400:
        *error = cJSON_IsString(desc) && strstr(desc->valuestring, "chat not found")
            ? WX_TR("The linked chat no longer exists. Link the chat again", "找不到绑定的聊天，请重新绑定") : WX_TR("Telegram refused the message", "Telegram 拒收了这条消息");
        break;
    case 403:
        *error = WX_TR("The bot is blocked. Tap Start in Telegram again", "机器人被屏蔽了，请在 Telegram 里重新点 Start");
        break;
    case 409:
        *error = WX_TR("This bot has a webhook set, so its messages cannot be read", "这个机器人设置了 Webhook，无法读取消息");
        break;
    case 429:
        *kind = SEND_RETRY;
        *error = WX_TR("Too many messages, will retry later", "发送太频繁，稍后重试");
        break;
    default:
        if (r->status >= 500 || r->status == 0 || !*root) {
            *kind = SEND_RETRY;
            *error = WX_TR("Telegram server error, will retry later", "Telegram 服务器出错，稍后重试");
        } else {
            *error = WX_TR("Telegram returned an error", "Telegram 返回了错误");
        }
        break;
    }
    return NULL;
}

static send_result_t send_text(const wx_tg_config_t *config, const char *chat_id, const char *text,
                               const char **error)
{
    cJSON *req = cJSON_CreateObject();
    cJSON *preview = cJSON_CreateObject();
    if (!req || !preview ||
        !cJSON_AddStringToObject(req, "chat_id", chat_id) ||
        !cJSON_AddStringToObject(req, "text", text) ||
        !cJSON_AddTrueToObject(preview, "is_disabled")) {
        cJSON_Delete(req);
        cJSON_Delete(preview);
        *error = WX_TR("Out of memory", "内存不足");
        return SEND_RETRY;
    }
    cJSON_AddItemToObject(req, "link_preview_options", preview);
    char *json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!json) {
        *error = WX_TR("Out of memory", "内存不足");
        return SEND_RETRY;
    }

    http_resp_t r = api_call(config->api_base, config->token, "sendMessage", json, RESP_CAP);
    cJSON_free(json);
    // 2xx 就是 Telegram 已收下。回复里带着整条消息,可能长到被截断、解析不了;
    // 那时若按失败重试,同一条消息会被发好几遍。
    if (r.err == ESP_OK && r.status >= 200 && r.status < 300) {
        free(r.body);
        *error = NULL;
        return SEND_OK;
    }
    cJSON *root;
    send_result_t kind;
    (void)parse_result(&r, &root, &kind, error);
    cJSON_Delete(root);
    free(r.body);
    return kind;
}

// ---------------------------------------------------------------------------
// 同步请求
// ---------------------------------------------------------------------------
static void copy_json_string(const cJSON *obj, const char *key, char *out, size_t cap)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    snprintf(out, cap, "%s", cJSON_IsString(item) ? item->valuestring : "");
    (void)wx_utf8_trim_incomplete(out, strlen(out));
}

static void run_check_token(job_t *job)
{
    http_resp_t r = api_call(job->api_base, job->token, "getMe", "{}", RESP_CAP);
    cJSON *root;
    send_result_t kind;
    const cJSON *me = parse_result(&r, &root, &kind, &job->error);
    if (me) {
        copy_json_string(me, "first_name", job->out1, job->cap1);
        copy_json_string(me, "username", job->out2, job->cap2);
        job->result = ESP_OK;
    } else {
        job->result = ESP_FAIL;
    }
    cJSON_Delete(root);
    free(r.body);
}

// 一条 update 里的聊天:私聊、群聊或频道都行。
static const cJSON *update_chat(const cJSON *update)
{
    static const char *const KINDS[] = { "message", "edited_message", "channel_post", "my_chat_member" };
    for (size_t i = 0; i < sizeof(KINDS) / sizeof(KINDS[0]); i++) {
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(update, KINDS[i]);
        const cJSON *chat = cJSON_GetObjectItemCaseSensitive(m, "chat");
        if (cJSON_IsObject(chat)) return chat;
    }
    return NULL;
}

static void chat_display_name(const cJSON *chat, char *out, size_t cap)
{
    copy_json_string(chat, "title", out, cap);
    if (out[0]) return;
    copy_json_string(chat, "first_name", out, cap);
    char last[WX_TG_NAME_MAX + 1];
    copy_json_string(chat, "last_name", last, sizeof(last));
    if (last[0] && strlen(out) + 1 + strlen(last) < cap) {
        strcat(out, " ");
        strcat(out, last);
    }
    if (!out[0]) copy_json_string(chat, "username", out, cap);
}

static void run_find_chat(job_t *job)
{
    wx_tg_config_t config;
    wx_settings_tg(&config);
    job->result = ESP_FAIL;
    if (!wx_tg_token_valid(config.token)) {
        job->error = WX_TR("Save the bot token first", "请先保存机器人 Token");
        return;
    }

    // offset 为负数:只取最近的几条,不影响别的程序读取。
    http_resp_t r = api_call(config.api_base, config.token, "getUpdates",
                             "{\"offset\":-5,\"limit\":5,\"timeout\":0}", UPDATES_RESP_CAP);
    cJSON *root;
    send_result_t kind;
    const cJSON *updates = parse_result(&r, &root, &kind, &job->error);
    free(r.body);
    if (!updates && r.err == ESP_OK && !root) job->error = WX_TR("The latest message is too long. Send the bot something short (like /start) and try again", "最近的消息太长，请给机器人发一句短的（如 /start）再试");

    const cJSON *chat = NULL;
    const int n = cJSON_GetArraySize(updates);
    for (int i = n - 1; i >= 0 && !chat; i--) chat = update_chat(cJSON_GetArrayItem(updates, i));
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(chat, "id");
    if (updates && !cJSON_IsNumber(id)) {
        job->error = WX_TR("No message found. Send your bot a message in Telegram first (like /start)", "没找到消息：请先在 Telegram 里给机器人发一条消息（如 /start）");
    } else if (cJSON_IsNumber(id)) {
        snprintf(job->out1, job->cap1, "%lld", (long long)id->valuedouble);
        chat_display_name(chat, job->out2, job->cap2);
        job->result = ESP_OK;
        job->error = NULL;
    }
    cJSON_Delete(root);
    if (job->result != ESP_OK) return;

    // 在找到的聊天里打个招呼,用户马上能确认绑对了。失败不影响绑定结果。
    const char *ignored;
    (void)send_text(&config, job->out1, WX_TR("✅ Linked to Passport Alert. New messages from your phone will be forwarded here.", "✅ 已绑定 Passport Alert。之后手机上的新消息会转发到这里。"), &ignored);
}

static void run_test(job_t *job)
{
    wx_tg_config_t config;
    wx_settings_tg(&config);
    if (!wx_tg_token_valid(config.token) || !config.chat_id[0]) {
        job->result = ESP_FAIL;
        job->error = WX_TR("Save the token and link a chat first", "请先保存 Token 并绑定聊天");
        return;
    }
    const send_result_t kind = send_text(&config, config.chat_id,
                                         WX_TR("🔔 Test message from Passport Alert. Forwarding works.", "🔔 这是 Passport Alert 的测试消息，转发设置正常。"), &job->error);
    job->result = kind == SEND_OK ? ESP_OK : ESP_FAIL;
}

// ---------------------------------------------------------------------------
// 转发队列
// ---------------------------------------------------------------------------
static void free_job(job_t *job)
{
    free(job->message);
    free(job);
}

static void drop_first_pending(void)
{
    free_job(s.pending[0]);
    memmove(&s.pending[0], &s.pending[1], (s.pending_count - 1) * sizeof(s.pending[0]));
    s.pending_count--;
}

static void add_pending(job_t *job)
{
    if (s.pending_count == PENDING_MAX) {
        ESP_LOGW(TAG, "待转发的消息太多,丢弃最早的一条");
        drop_first_pending();
        update_stats(0, 1, WX_TR("Queue full, one message was dropped", "排队太多，丢了一条"), false);
    }
    job->next_try = xTaskGetTickCount();
    s.pending[s.pending_count++] = job;
    update_stats(0, 0, NULL, false);
}

static uint32_t retry_delay_ms(uint8_t tries)
{
    uint32_t ms = RETRY_FIRST_MS;
    for (uint8_t i = 1; i < tries && ms < RETRY_MAX_MS; i++) ms *= 2;
    return ms > RETRY_MAX_MS ? RETRY_MAX_MS : ms;
}

// 发送排在最前、已到时间的转发。返回下次需要醒来的等待时间。
static TickType_t flush_pending(void)
{
    while (s.pending_count > 0) {
        job_t *job = s.pending[0];
        const int32_t wait = (int32_t)(job->next_try - xTaskGetTickCount());
        if (wait > 0) return (TickType_t)wait;
        if (!wx_net_online()) return pdMS_TO_TICKS(OFFLINE_POLL_MS);

        wx_tg_config_t config;
        wx_settings_tg(&config);
        if (!wx_settings_forwards(job->app)) {
            drop_first_pending();   // 已取消转发这个应用,或 Telegram 设置被清除
            update_stats(0, 0, NULL, false);
            continue;
        }
        char text[TEXT_MAX];
        const wx_lang_t lang = wx_lang();
        wx_tg_format(text, sizeof(text), lang, wx_app_name(job->app, lang), job->phone, job->title,
                     wx_app_title_is_generic(job->app, job->title), config.body ? job->message : NULL);

        const char *error = NULL;
        const send_result_t kind = send_text(&config, config.chat_id, text, &error);
        if (kind == SEND_OK) {
            ESP_LOGI(TAG, "已转发一条%s消息(%u 字节)", wx_app_name(job->app, WX_LANG_ZH), (unsigned)strlen(text));
            drop_first_pending();
            update_stats(1, 0, NULL, true);
        } else if (kind == SEND_REJECTED || ++job->tries >= RETRY_LIMIT) {
            ESP_LOGW(TAG, "放弃转发一条%s消息:%s", wx_app_name(job->app, WX_LANG_ZH), error);
            drop_first_pending();
            update_stats(0, 1, error, false);
        } else {
            const uint32_t delay = retry_delay_ms(job->tries);
            ESP_LOGW(TAG, "转发失败:%s;%lu 秒后重试(%u/%d)", error, (unsigned long)(delay / 1000),
                     job->tries, RETRY_LIMIT);
            job->next_try = xTaskGetTickCount() + pdMS_TO_TICKS(delay);
            update_stats(0, 0, error, false);
            return pdMS_TO_TICKS(delay);
        }
    }
    return portMAX_DELAY;
}

static void tg_task(void *arg)
{
    (void)arg;
    TickType_t wait = portMAX_DELAY;
    for (;;) {
        job_t *job;
        if (xQueueReceive(s.jobs, &job, wait) == pdTRUE) {
            switch (job->type) {
            case JOB_FORWARD:
                add_pending(job);
                break;
            case JOB_CHECK_TOKEN:
                run_check_token(job);
                xSemaphoreGive(job->done);
                break;
            case JOB_FIND_CHAT:
                run_find_chat(job);
                xSemaphoreGive(job->done);
                break;
            case JOB_TEST:
                run_test(job);
                if (job->result == ESP_OK) update_stats(0, 0, NULL, true);
                xSemaphoreGive(job->done);
                break;
            }
        }
        wait = flush_pending();
    }
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
esp_err_t wx_tg_start(wx_tg_cb_t cb, void *ctx)
{
    if (s.jobs) return ESP_ERR_INVALID_STATE;
    s.cb = cb;
    s.ctx = ctx;
    s.jobs = xQueueCreate(JOB_QUEUE_DEPTH, sizeof(job_t *));
    s.stats_lock = xSemaphoreCreateMutex();
    if (!s.jobs || !s.stats_lock) return ESP_ERR_NO_MEM;
    if (xTaskCreate(tg_task, "wx_tg", TASK_STACK, NULL, 3, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void wx_tg_forward(uint8_t app, const char *phone, const char *title, char *message)
{
    job_t *job = s.jobs ? calloc(1, sizeof(*job)) : NULL;
    if (!job) {
        free(message);
        return;
    }
    job->type = JOB_FORWARD;
    job->app = app;
    snprintf(job->phone, sizeof(job->phone), "%s", phone ? phone : "");
    snprintf(job->title, sizeof(job->title), "%s", title ? title : "");
    job->message = message;
    if (xQueueSend(s.jobs, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Telegram 队列已满,丢弃一条转发");
        free_job(job);
    }
}

static esp_err_t run_sync(job_t *job)
{
    if (!s.jobs) {
        job->error = WX_TR("Telegram is not running", "Telegram 模块没有启动");
        return ESP_ERR_INVALID_STATE;
    }
    if (!wx_net_online()) {
        job->error = WX_TR("The device is not on Wi-Fi yet", "板子还没连上 Wi-Fi");
        return ESP_ERR_INVALID_STATE;
    }
    job->done = xSemaphoreCreateBinary();
    if (!job->done) {
        job->error = WX_TR("Out of memory", "内存不足");
        return ESP_ERR_NO_MEM;
    }
    job->result = ESP_FAIL;
    // 网页在等:排到转发前面。请求本身有超时,所以这里可以一直等。
    job_t *ptr = job;
    if (xQueueSendToFront(s.jobs, &ptr, pdMS_TO_TICKS(1000)) != pdTRUE) {
        vSemaphoreDelete(job->done);
        job->error = WX_TR("Telegram is busy, try again shortly", "Telegram 忙，请稍后再试");
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreTake(job->done, portMAX_DELAY);
    vSemaphoreDelete(job->done);
    return job->result;
}

esp_err_t wx_tg_check_token(const char *api_base, const char *token, char *bot_name, size_t name_cap,
                            char *bot_user, size_t user_cap, const char **error)
{
    wx_tg_config_t config;
    wx_settings_tg(&config);
    job_t job = {
        .type = JOB_CHECK_TOKEN,
        .api_base = api_base ? api_base : config.api_base,
        .token = token,
        .out1 = bot_name, .cap1 = name_cap,
        .out2 = bot_user, .cap2 = user_cap,
    };
    if (!wx_tg_token_valid(token)) {
        *error = WX_TR("That does not look like a token. It should look like 123456789:AAH…", "Token 格式不对：应形如 123456789:AAH…");
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = run_sync(&job);
    *error = job.error;
    return err;
}

esp_err_t wx_tg_find_chat(char *chat_id, size_t id_cap, char *chat_name, size_t name_cap,
                          const char **error)
{
    job_t job = {
        .type = JOB_FIND_CHAT,
        .out1 = chat_id, .cap1 = id_cap,
        .out2 = chat_name, .cap2 = name_cap,
    };
    const esp_err_t err = run_sync(&job);
    *error = job.error;
    return err;
}

esp_err_t wx_tg_send_test(const char **error)
{
    job_t job = { .type = JOB_TEST };
    const esp_err_t err = run_sync(&job);
    *error = job.error;
    return err;
}

void wx_tg_stats(wx_tg_stats_t *out)
{
    if (!s.stats_lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s.stats_lock, portMAX_DELAY);
    *out = s.stats;
    xSemaphoreGive(s.stats_lock);
}
