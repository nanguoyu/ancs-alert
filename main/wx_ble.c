// main/wx_ble.c —— NimBLE 外设 + ANCS 客户端,支持多台 iPhone 同时连接。
//
// 每台手机的流程:广播(带 ANCS 服务征集 UUID 与 HID 外观,iPhone 设置 > 蓝牙里
//   才会列出本设备)→ iPhone 连接 → 等它自己用已存密钥加密,新手机则请求配对
//   (在 iPhone 输入屏幕上的 6 位配对码)→ 发现 ANCS 的 Notification Source /
//   Control Point / Data Source → 先订阅 Data Source 再订阅 Notification Source
//   (Apple 建议的顺序)→ 读设备信息服务里的型号 → 就绪,每条新增通知查询
//   AppIdentifier 与 Title(新到的通知还查 Message),是聊天应用才上报。
// 未满 WX_BLE_MAX_PHONES 台时一直广播,其他 iPhone 可以随时连进来。
//
// 除 wx_ble_start / wx_ble_forget_phones 外,所有函数都运行在 NimBLE host 任务
// (GAP/GATT 回调、callout 与事件都由 host 默认事件队列派发),因此状态无需加锁。
#include "wx_ble.h"
#include "wx_ancs_proto.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "sdkconfig.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <stdio.h>
#include <string.h>

_Static_assert(WX_BLE_MAX_PHONES == CONFIG_BT_NIMBLE_MAX_CONNECTIONS,
               "WX_BLE_MAX_PHONES must match CONFIG_BT_NIMBLE_MAX_CONNECTIONS");

static const char *TAG = "wx_ble";

#define PREFERRED_MTU       185
// 首次配对后 iPhone 会弹窗询问是否共享通知,用户点“允许”前 ANCS 不可用,
// 因此重试窗口放宽到约 1 分钟。
#define DISC_RETRY_MAX      40
#define DISC_RETRY_MS       1500
#define DS_TIMEOUT_MS       3000
// 已绑定的 iPhone 连上后会自己用保存的密钥加密(约 0.2 s)。若此时本机也发起
// Security Request,NimBLE 会挂着这个请求直到 30 s 超时,且不上报加密成功。
// 所以先等一会儿:已加密就直接发现 ANCS,未加密(新手机)才请求配对。
#define SEC_CHECK_MS        1500
// 新手机首次配对时,订阅发生在用户点“允许共享通知”之前,iPhone 之后不会主动补发。
// 所以新配对后如果迟迟收不到任何通知事件,就重新订阅 Notification Source
// (订阅时 iPhone 会把通知中心现有的通知全部发一遍)。
#define RESUB_MS            10000
#define RESUB_MAX           6
#define MAX_ANCS_CHRS       8
#define CCCD_UUID16         0x2902
#define MODEL_NUMBER_UUID16 0x2A24     // 设备信息服务的 Model Number String
#define RX_BUF_SIZE         256
// iOS“设置 > 蓝牙”只列出广播 HID 服务的 BLE 配件。与 ESP-IDF bluedroid
// ble_ancs 示例相同,只在广播里声明 HID 服务与外观,不实现 HID 本身。
#define HID_SERVICE_UUID16  0x1812
#define APPEARANCE_GENERIC_HID 0x03C0

// ESP-IDF 的 NimBLE 示例使用这个初始化函数,但公开头文件只导出了存取钩子。
void ble_store_config_init(void);

static const ble_uuid16_t s_hid_uuid = BLE_UUID16_INIT(HID_SERVICE_UUID16);
static const ble_uuid16_t s_model_uuid = BLE_UUID16_INIT(MODEL_NUMBER_UUID16);
static const ble_uuid128_t s_ancs_uuid = BLE_UUID128_INIT(WX_ANCS_SERVICE_UUID_LE);
static const ble_uuid128_t s_ns_uuid = BLE_UUID128_INIT(WX_ANCS_NOTIF_SRC_UUID_LE);
static const ble_uuid128_t s_cp_uuid = BLE_UUID128_INIT(WX_ANCS_CTRL_PT_UUID_LE);
static const ble_uuid128_t s_ds_uuid = BLE_UUID128_INIT(WX_ANCS_DATA_SRC_UUID_LE);

typedef struct {
    uint16_t def_handle;
    uint16_t val_handle;
    uint16_t end_handle;
    uint16_t cccd_handle;
} ancs_chr_t;

// 一台手机(一条连接)的全部状态。conn 为 BLE_HS_CONN_HANDLE_NONE 表示槽位空闲。
typedef struct {
    uint8_t index;
    uint16_t conn;

    // 发现结果
    uint16_t svc_start;
    uint16_t svc_end;
    ancs_chr_t found[MAX_ANCS_CHRS];
    const ble_uuid128_t *found_uuid[MAX_ANCS_CHRS];
    size_t found_count;
    ancs_chr_t ns;
    ancs_chr_t cp;
    ancs_chr_t ds;
    ancs_chr_t *dsc_target;      // 正在发现描述符的特征
    uint8_t disc_attempts;
    bool security_requested;
    bool discovery_started;
    bool ready;
    char model[WX_PHONE_NAME_MAX + 1];

    // 通知属性查询:每台手机同一时刻只有一条在途
    wx_ancs_queue_t queue;
    wx_ancs_ds_parser_t parser;
    bool request_in_flight;
    wx_ancs_pending_t request;
    uint32_t request_seq;

    wx_ancs_counts_t counts;     // 通知中心各类别的通知数

    struct ble_npl_callout retry_callout;
    struct ble_npl_callout ds_timeout_callout;
    struct ble_npl_callout sec_check_callout;
    struct ble_npl_callout resub_callout;
    uint16_t ns_events;          // 本次连接收到的 Notification Source 事件数
    uint8_t resub_attempts;
} phone_t;

static struct {
    wx_ble_event_cb_t cb;
    void *ctx;
    bool started;
    uint8_t own_addr_type;
    phone_t phones[WX_BLE_MAX_PHONES];
} s;

static struct ble_npl_event s_forget_event;

static int gap_event(struct ble_gap_event *event, void *arg);
static void start_discovery(phone_t *p);
static void pump_requests(phone_t *p);

static void emit(const wx_ble_event_t *event)
{
    if (s.cb) s.cb(event, s.ctx);
}

static void emit_stack(wx_ble_stack_t stack)
{
    const wx_ble_event_t event = { .type = WX_BLE_EV_STACK, .data.stack = stack };
    emit(&event);
}

static void emit_state(const phone_t *p, wx_phone_state_t state)
{
    const wx_ble_event_t event = {
        .type = WX_BLE_EV_PHONE_STATE, .phone = p->index, .data.phone_state = state,
    };
    emit(&event);
}

static phone_t *phone_by_conn(uint16_t conn)
{
    if (conn == BLE_HS_CONN_HANDLE_NONE) return NULL;
    for (size_t i = 0; i < WX_BLE_MAX_PHONES; i++) {
        if (s.phones[i].conn == conn) return &s.phones[i];
    }
    return NULL;
}

static phone_t *free_phone(void)
{
    for (size_t i = 0; i < WX_BLE_MAX_PHONES; i++) {
        if (s.phones[i].conn == BLE_HS_CONN_HANDLE_NONE) return &s.phones[i];
    }
    return NULL;
}

static size_t connected_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < WX_BLE_MAX_PHONES; i++) {
        if (s.phones[i].conn != BLE_HS_CONN_HANDLE_NONE) n++;
    }
    return n;
}

static void reset_phone(phone_t *p)
{
    ble_npl_callout_stop(&p->retry_callout);
    ble_npl_callout_stop(&p->ds_timeout_callout);
    ble_npl_callout_stop(&p->sec_check_callout);
    ble_npl_callout_stop(&p->resub_callout);
    p->ns_events = 0;
    p->resub_attempts = 0;
    p->svc_start = 0;
    p->svc_end = 0;
    p->found_count = 0;
    memset(&p->ns, 0, sizeof(p->ns));
    memset(&p->cp, 0, sizeof(p->cp));
    memset(&p->ds, 0, sizeof(p->ds));
    p->dsc_target = NULL;
    p->disc_attempts = 0;
    p->security_requested = false;
    p->discovery_started = false;
    p->ready = false;
    p->model[0] = '\0';
    wx_ancs_queue_init(&p->queue);
    p->request_in_flight = false;
    wx_ancs_counts_reset(&p->counts);
}

// ---------------------------------------------------------------------------
// 广播
// ---------------------------------------------------------------------------
static void advertise(void)
{
    if (connected_count() >= WX_BLE_MAX_PHONES || ble_gap_adv_active()) return;

    // 3 (flags) + 4 (HID UUID) + 4 (外观) + 18 (ANCS 征集) = 29 字节,不超过 31。
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids16 = &s_hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.appearance = APPEARANCE_GENERIC_HID;
    fields.appearance_is_present = 1;
    fields.sol_uuids128 = &s_ancs_uuid;
    fields.sol_num_uuids128 = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播数据失败: %d", rc);
        emit_stack(WX_BLE_STACK_FAILED);
        return;
    }

    char name[WX_BLE_DEVICE_NAME_MAX];
    wx_ble_device_name(name, sizeof(name));
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (const uint8_t *)name;
    rsp.name_len = strlen(name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置扫描响应失败: %d", rc);
        emit_stack(WX_BLE_STACK_FAILED);
        return;
    }

    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s.own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "开始广播失败: %d", rc);
        emit_stack(WX_BLE_STACK_FAILED);
        return;
    }
    ESP_LOGI(TAG, "广播中(%s),已连接 %u 台,等待 iPhone 连接", name, (unsigned)connected_count());
}

// ---------------------------------------------------------------------------
// 通知属性查询
// ---------------------------------------------------------------------------
static void finish_request(phone_t *p)
{
    p->request_in_flight = false;
    ble_npl_callout_stop(&p->ds_timeout_callout);
}

static int on_cp_written(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    phone_t *p = phone_by_conn(conn_handle);
    const uint32_t seq = (uint32_t)(uintptr_t)arg;
    if (p && error->status != 0 && p->request_in_flight && seq == p->request_seq) {
        // 0xA2 等错误:通常是通知在查询前已被移除。跳过,继续下一条。
        ESP_LOGW(TAG, "[%u] 查询通知 %lu 失败: 0x%x", p->index,
                 (unsigned long)p->request.uid, error->status);
        finish_request(p);
        pump_requests(p);
    }
    return 0;
}

static void pump_requests(phone_t *p)
{
    while (p->ready && !p->request_in_flight) {
        wx_ancs_pending_t next;
        if (!wx_ancs_queue_pop(&p->queue, &next)) return;

        // 正文只用于转发新消息;连上时补发的已有通知不查正文,省流量也少碰隐私。
        const bool with_message = !next.pre_existing;
        uint8_t cmd[WX_ANCS_ATTRS_REQUEST_MAX_LEN];
        size_t len = wx_ancs_build_attrs_request(next.uid, with_message, cmd, sizeof(cmd));
        p->request = next;
        p->request_seq++;
        wx_ancs_ds_reset(&p->parser, with_message);
        p->request_in_flight = true;
        int rc = ble_gattc_write_flat(p->conn, p->cp.val_handle, cmd, (uint16_t)len,
                                      on_cp_written, (void *)(uintptr_t)p->request_seq);
        if (rc != 0) {
            ESP_LOGW(TAG, "[%u] 写 Control Point 失败: %d", p->index, rc);
            p->request_in_flight = false;
            continue;
        }
        ble_npl_callout_reset(&p->ds_timeout_callout, ble_npl_time_ms_to_ticks32(DS_TIMEOUT_MS));
    }
}

static void on_ds_timeout(struct ble_npl_event *ev)
{
    phone_t *p = ble_npl_event_get_arg(ev);
    if (p->conn == BLE_HS_CONN_HANDLE_NONE || !p->request_in_flight) return;
    ESP_LOGW(TAG, "[%u] 通知 %lu 的查询超时", p->index, (unsigned long)p->request.uid);
    p->request_in_flight = false;
    pump_requests(p);
}

static void handle_data_source(phone_t *p, const uint8_t *data, size_t len)
{
    if (!p->request_in_flight) return;
    wx_ancs_ds_status_t status = wx_ancs_ds_feed(&p->parser, data, len);
    if (status == WX_ANCS_DS_MORE) return;
    if (status == WX_ANCS_DS_ERROR || p->parser.uid != p->request.uid) {
        // 上一条查询超时后才到的回复(或它的后半截):丢掉,继续等当前这条的回复,
        // 等不到由超时处理。若在这里结束当前查询,它真正的回复又会被下一条当成过期回复丢掉,
        // 一连串通知都会因此漏掉。每条回复都从一个新的 Data Source 通知开始。
        ESP_LOGW(TAG, "[%u] 丢弃一段不属于通知 %lu 的 Data Source 数据", p->index,
                 (unsigned long)p->request.uid);
        wx_ancs_ds_reset(&p->parser, !p->request.pre_existing);
        return;
    }

    // 只记来源应用的标识,不记标题,便于排查 iOS 转发了哪些应用的通知。
    ESP_LOGI(TAG, "[%u] 通知 %lu 来自 %s", p->index, (unsigned long)p->parser.uid,
             p->parser.app_id[0] ? p->parser.app_id : "(空)");
    const uint8_t app = wx_app_from_bundle(p->parser.app_id, p->parser.app_id_truncated);
    if (app != WX_APP_UNKNOWN) {
        wx_ble_event_t event = {
            .type = WX_BLE_EV_NOTIF_ADDED,
            .phone = p->index,
            .data.added = {
                .uid = p->request.uid,
                .app = app,
                .pre_existing = p->request.pre_existing,
                .message = p->parser.message,
            },
        };
        memcpy(event.data.added.title, p->parser.title, sizeof(event.data.added.title));
        // 标题是好友名、正文是聊天内容,都属于个人数据,日志里只记长度。
        ESP_LOGI(TAG, "[%u] %s通知 %lu%s,标题 %u 字节,正文 %u 字节", p->index, wx_app_name(app, WX_LANG_ZH),
                 (unsigned long)p->request.uid, p->request.pre_existing ? "(已有)" : "",
                 (unsigned)strlen(p->parser.title), (unsigned)strlen(p->parser.message));
        emit(&event);
    }
    finish_request(p);
    pump_requests(p);
}

static void handle_notification_source(phone_t *p, const uint8_t *data, size_t len)
{
    wx_ancs_ns_event_t ns;
    if (!wx_ancs_parse_ns(data, len, &ns)) return;
    if (p->ns_events < UINT16_MAX) p->ns_events++;

    const uint16_t before = wx_ancs_counts_total(&p->counts);
    const uint16_t after = wx_ancs_counts_update(&p->counts, &ns);
    // 只记元信息(事件类型、类别、UID、总数),不记通知内容。
    ESP_LOGI(TAG, "[%u] 通知事件 %s 类别 %u UID %lu%s,共 %u 条", p->index,
             ns.event_id == WX_ANCS_EVENT_ADDED ? "新增" :
             ns.event_id == WX_ANCS_EVENT_REMOVED ? "移除" : "修改",
             ns.category_id, (unsigned long)ns.uid,
             (ns.flags & WX_ANCS_FLAG_PRE_EXISTING) ? "(已有)" : "", after);
    if (after != before) {
        const wx_ble_event_t total = {
            .type = WX_BLE_EV_NOTIF_TOTAL, .phone = p->index, .data.total = after,
        };
        emit(&total);
    }

    if (ns.event_id == WX_ANCS_EVENT_ADDED) {
        const bool pre_existing = (ns.flags & WX_ANCS_FLAG_PRE_EXISTING) != 0;
        if (!wx_ancs_queue_push(&p->queue, ns.uid, pre_existing)) {
            ESP_LOGD(TAG, "[%u] 查询队列已满,跳过已有通知 %lu", p->index, (unsigned long)ns.uid);
        }
        pump_requests(p);
    } else if (ns.event_id == WX_ANCS_EVENT_REMOVED) {
        wx_ancs_queue_remove(&p->queue, ns.uid);
        const wx_ble_event_t event = {
            .type = WX_BLE_EV_NOTIF_REMOVED, .phone = p->index, .data.removed_uid = ns.uid,
        };
        emit(&event);
    }
}

// ---------------------------------------------------------------------------
// 发现与订阅
// ---------------------------------------------------------------------------
static void schedule_retry(phone_t *p, const char *why)
{
    if (p->conn == BLE_HS_CONN_HANDLE_NONE) return;
    if (++p->disc_attempts > DISC_RETRY_MAX) {
        ESP_LOGW(TAG, "[%u] %s;已重试 %d 次,放弃。请在 iPhone 上允许共享系统通知",
                 p->index, why, DISC_RETRY_MAX);
        emit_state(p, WX_PHONE_NO_ANCS);
        return;
    }
    ESP_LOGW(TAG, "[%u] %s;%d ms 后重试(%d/%d)", p->index, why, DISC_RETRY_MS,
             p->disc_attempts, DISC_RETRY_MAX);
    ble_npl_callout_reset(&p->retry_callout, ble_npl_time_ms_to_ticks32(DISC_RETRY_MS));
}

static void on_retry(struct ble_npl_event *ev)
{
    phone_t *p = ble_npl_event_get_arg(ev);
    if (p->conn != BLE_HS_CONN_HANDLE_NONE && !p->ready) start_discovery(p);
}

static int on_resub_on(uint16_t conn_handle, const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    phone_t *p = arg;
    if (p->conn != conn_handle) return 0;
    if (error->status != 0) ESP_LOGW(TAG, "[%u] 重新订阅失败: 0x%x", p->index, error->status);
    ble_npl_callout_reset(&p->resub_callout, ble_npl_time_ms_to_ticks32(RESUB_MS));
    return 0;
}

static int on_resub_off(uint16_t conn_handle, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    (void)error;
    phone_t *p = arg;
    if (p->conn != conn_handle) return 0;
    static const uint8_t enable[2] = { 0x01, 0x00 };
    if (ble_gattc_write_flat(conn_handle, p->ns.cccd_handle, enable, sizeof(enable),
                             on_resub_on, p) != 0) {
        ble_npl_callout_reset(&p->resub_callout, ble_npl_time_ms_to_ticks32(RESUB_MS));
    }
    return 0;
}

static void on_resub(struct ble_npl_event *ev)
{
    phone_t *p = ble_npl_event_get_arg(ev);
    if (p->conn == BLE_HS_CONN_HANDLE_NONE || !p->ready || p->ns_events > 0) return;
    if (++p->resub_attempts > RESUB_MAX) {
        ESP_LOGW(TAG, "[%u] 一直收不到通知;请在 iPhone 蓝牙设置里确认已打开“共享系统通知”", p->index);
        return;
    }
    ESP_LOGI(TAG, "[%u] 新配对后还没收到通知,重新订阅(%u/%d)", p->index, p->resub_attempts, RESUB_MAX);
    static const uint8_t disable[2] = { 0x00, 0x00 };
    if (ble_gattc_write_flat(p->conn, p->ns.cccd_handle, disable, sizeof(disable),
                             on_resub_off, p) != 0) {
        ble_npl_callout_reset(&p->resub_callout, ble_npl_time_ms_to_ticks32(RESUB_MS));
    }
}

static void become_ready(phone_t *p)
{
    p->ready = true;
    p->disc_attempts = 0;
    // 这次连接走了配对流程(新手机):等不到通知就重新订阅。
    if (p->security_requested) {
        ble_npl_callout_reset(&p->resub_callout, ble_npl_time_ms_to_ticks32(RESUB_MS));
    }

    wx_ble_event_t name = { .type = WX_BLE_EV_PHONE_NAME, .phone = p->index };
    wx_phone_model_name(p->model, name.data.name, sizeof(name.data.name));
    emit(&name);

    ESP_LOGI(TAG, "[%u] %s 的 ANCS 已就绪,开始接收通知", p->index, name.data.name);
    emit_state(p, WX_PHONE_READY);
    pump_requests(p);   // 订阅时手机已补发的已有通知
}

static int on_model(uint16_t conn_handle, const struct ble_gatt_error *error,
                    struct ble_gatt_attr *attr, void *arg)
{
    phone_t *p = arg;
    if (p->conn != conn_handle || p->ready) return 0;
    if (error->status == 0 && attr && attr->om) {
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(attr->om, p->model, sizeof(p->model) - 1, &len) != 0) {
            len = sizeof(p->model) - 1;
        }
        p->model[len] = '\0';
        return 0;
    }
    // 读完(EDONE)或手机不提供型号:都进入就绪,型号未知时显示 "iPhone"。
    become_ready(p);
    return 0;
}

static int on_ns_subscribed(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    phone_t *p = arg;
    if (p->conn != conn_handle) return 0;
    if (error->status != 0) {
        schedule_retry(p, "订阅 Notification Source 失败");
        return 0;
    }
    // 订阅完成后读型号,串行进行,避免和其他 GATT 请求交叉。
    int rc = ble_gattc_read_by_uuid(conn_handle, 1, 0xFFFF, &s_model_uuid.u, on_model, p);
    if (rc != 0) become_ready(p);
    return 0;
}

static int on_ds_subscribed(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    phone_t *p = arg;
    if (p->conn != conn_handle) return 0;
    if (error->status != 0) {
        schedule_retry(p, "订阅 Data Source 失败");
        return 0;
    }
    static const uint8_t enable[2] = { 0x01, 0x00 };
    int rc = ble_gattc_write_flat(conn_handle, p->ns.cccd_handle, enable, sizeof(enable),
                                  on_ns_subscribed, p);
    if (rc != 0) schedule_retry(p, "写 Notification Source CCCD 失败");
    return 0;
}

static void subscribe(phone_t *p)
{
    static const uint8_t enable[2] = { 0x01, 0x00 };
    int rc = ble_gattc_write_flat(p->conn, p->ds.cccd_handle, enable, sizeof(enable),
                                  on_ds_subscribed, p);
    if (rc != 0) schedule_retry(p, "写 Data Source CCCD 失败");
}

static void discover_descriptors(phone_t *p, ancs_chr_t *chr);

static int on_dsc(uint16_t conn_handle, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    phone_t *p = arg;
    if (p->conn != conn_handle || !p->dsc_target) return 0;
    ancs_chr_t *chr = p->dsc_target;
    if (error->status == 0) {
        if (ble_uuid_u16(&dsc->uuid.u) == CCCD_UUID16) chr->cccd_handle = dsc->handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        schedule_retry(p, "发现描述符失败");
        return 0;
    }
    if (chr->cccd_handle == 0) chr->cccd_handle = chr_val_handle + 1;  // 兜底:紧随值句柄

    if (chr == &p->ds) {
        discover_descriptors(p, &p->ns);
    } else {
        p->dsc_target = NULL;
        subscribe(p);
    }
    return 0;
}

static void discover_descriptors(phone_t *p, ancs_chr_t *chr)
{
    p->dsc_target = chr;
    int rc = ble_gattc_disc_all_dscs(p->conn, chr->val_handle, chr->end_handle, on_dsc, p);
    if (rc != 0) schedule_retry(p, "发现 ANCS 描述符失败");
}

static int on_chr(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg)
{
    phone_t *p = arg;
    if (p->conn != conn_handle) return 0;
    if (error->status == 0) {
        if (p->found_count < MAX_ANCS_CHRS) {
            p->found[p->found_count] = (ancs_chr_t){ .def_handle = chr->def_handle,
                                                     .val_handle = chr->val_handle };
            p->found_uuid[p->found_count] = NULL;
            if (ble_uuid_cmp(&chr->uuid.u, &s_ns_uuid.u) == 0) p->found_uuid[p->found_count] = &s_ns_uuid;
            if (ble_uuid_cmp(&chr->uuid.u, &s_cp_uuid.u) == 0) p->found_uuid[p->found_count] = &s_cp_uuid;
            if (ble_uuid_cmp(&chr->uuid.u, &s_ds_uuid.u) == 0) p->found_uuid[p->found_count] = &s_ds_uuid;
            p->found_count++;
        }
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        schedule_retry(p, "发现 ANCS 特征失败");
        return 0;
    }

    // 特征按句柄升序返回;每个特征的描述符范围到下一个特征声明之前为止。
    for (size_t i = 0; i < p->found_count; i++) {
        ancs_chr_t c = p->found[i];
        c.end_handle = (i + 1 < p->found_count) ? p->found[i + 1].def_handle - 1 : p->svc_end;
        if (p->found_uuid[i] == &s_ns_uuid) p->ns = c;
        if (p->found_uuid[i] == &s_cp_uuid) p->cp = c;
        if (p->found_uuid[i] == &s_ds_uuid) p->ds = c;
    }
    if (!p->ns.val_handle || !p->cp.val_handle || !p->ds.val_handle) {
        schedule_retry(p, "ANCS 特征不完整");
        return 0;
    }
    discover_descriptors(p, &p->ds);
    return 0;
}

static int on_svc(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *service, void *arg)
{
    phone_t *p = arg;
    if (p->conn != conn_handle) return 0;
    if (error->status == 0) {
        p->svc_start = service->start_handle;
        p->svc_end = service->end_handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE || p->svc_start == 0) {
        // iPhone 在用户允许“共享系统通知”之前不开放 ANCS。
        schedule_retry(p, "未找到 ANCS 服务");
        return 0;
    }
    p->found_count = 0;
    int rc = ble_gattc_disc_all_chrs(conn_handle, p->svc_start, p->svc_end, on_chr, p);
    if (rc != 0) schedule_retry(p, "发现 ANCS 特征失败");
    return 0;
}

static void start_discovery(phone_t *p)
{
    p->svc_start = 0;
    p->svc_end = 0;
    emit_state(p, WX_PHONE_DISCOVERING);
    int rc = ble_gattc_disc_svc_by_uuid(p->conn, &s_ancs_uuid.u, on_svc, p);
    if (rc != 0) schedule_retry(p, "发现 ANCS 服务失败");
}

static bool link_encrypted(const phone_t *p)
{
    struct ble_gap_conn_desc desc;
    return p->conn != BLE_HS_CONN_HANDLE_NONE &&
           ble_gap_conn_find(p->conn, &desc) == 0 && desc.sec_state.encrypted;
}

static void begin_discovery_once(phone_t *p)
{
    if (p->discovery_started || p->ready) return;
    p->discovery_started = true;
    ESP_LOGI(TAG, "[%u] 链路已加密", p->index);
    start_discovery(p);
}

static void on_sec_check(struct ble_npl_event *ev)
{
    phone_t *p = ble_npl_event_get_arg(ev);
    if (p->conn == BLE_HS_CONN_HANDLE_NONE) return;
    if (link_encrypted(p)) {
        begin_discovery_once(p);
        return;
    }
    if (!p->security_requested) {
        // 新手机:请求配对,iPhone 会弹出配对码输入框。
        p->security_requested = true;
        ESP_LOGI(TAG, "[%u] 链路未加密,请求配对", p->index);
        int rc = ble_gap_security_initiate(p->conn);
        if (rc != 0) ESP_LOGW(TAG, "[%u] 请求加密失败: %d,等待手机发起", p->index, rc);
    }
}

// ---------------------------------------------------------------------------
// GAP 事件
// ---------------------------------------------------------------------------
static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "连接失败: %d", event->connect.status);
            advertise();
            return 0;
        }
        phone_t *p = free_phone();
        if (!p) {
            // 不应发生:满员时已停止广播。
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_CONN_LIMIT);
            return 0;
        }
        reset_phone(p);
        p->conn = event->connect.conn_handle;
        ESP_LOGI(TAG, "[%u] iPhone 已连接(共 %u 台)", p->index, (unsigned)connected_count());
        emit_state(p, WX_PHONE_CONNECTED);
        ble_npl_callout_reset(&p->sec_check_callout, ble_npl_time_ms_to_ticks32(SEC_CHECK_MS));
        advertise();   // 连接建立后广播会自动停止;未满员则继续等待其他手机
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        phone_t *p = phone_by_conn(event->disconnect.conn.conn_handle);
        ESP_LOGI(TAG, "[%d] 连接断开,原因 0x%x", p ? p->index : -1, event->disconnect.reason);
        if (p) {
            reset_phone(p);
            p->conn = BLE_HS_CONN_HANDLE_NONE;
            emit_state(p, WX_PHONE_OFF);
        }
        advertise();
        return 0;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        phone_t *p = phone_by_conn(event->enc_change.conn_handle);
        if (!p) return 0;
        if (event->enc_change.status != 0 && !link_encrypted(p)) {
            // 常见原因:手机端已“忽略此设备”而本机仍保留旧密钥,或反之。
            ESP_LOGW(TAG, "[%u] 加密失败: %d。如反复出现,请在 iPhone 上忽略此设备,"
                          "并在板子的设置菜单里清除 iPhone 配对", p->index, event->enc_change.status);
            return 0;
        }
        // 状态非 0 但链路已加密:是本机的配对请求超时,而手机已自行加密,照常继续。
        begin_discovery_once(p);
        return 0;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        phone_t *p = phone_by_conn(event->passkey.conn_handle);
        if (p && event->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io io = { .action = BLE_SM_IOACT_DISP, .passkey = esp_random() % 1000000 };
            const wx_ble_event_t passkey = {
                .type = WX_BLE_EV_PASSKEY, .phone = p->index, .data.passkey = io.passkey,
            };
            emit(&passkey);
            int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
            if (rc != 0) ESP_LOGW(TAG, "[%u] 提交配对码失败: %d", p->index, rc);
        }
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // 手机删除了配对、重新配对:丢掉旧密钥再继续。
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
        phone_t *p = phone_by_conn(event->notify_rx.conn_handle);
        if (!p) return 0;
        uint8_t buf[RX_BUF_SIZE];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof(buf), &len) != 0) {
            // 超长时 len 为已复制部分;ANCS 只取开头即可。
            len = sizeof(buf);
        }
        if (event->notify_rx.attr_handle == p->ns.val_handle) {
            handle_notification_source(p, buf, len);
        } else if (event->notify_rx.attr_handle == p->ds.val_handle) {
            handle_data_source(p, buf, len);
        }
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU = %d", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// 协议栈生命周期
// ---------------------------------------------------------------------------
static void on_forget(struct ble_npl_event *ev)
{
    (void)ev;
    int rc = ble_store_clear();
    ESP_LOGI(TAG, "已清除配对: %d", rc);
    const wx_ble_event_t event = { .type = WX_BLE_EV_BONDS_CLEARED, .data.status = rc };
    emit(&event);
    bool any = false;
    for (size_t i = 0; i < WX_BLE_MAX_PHONES; i++) {
        if (s.phones[i].conn != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(s.phones[i].conn, BLE_ERR_REM_USER_CONN_TERM);
            any = true;
        }
    }
    if (!any) advertise();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE 复位,原因 %d", reason);
    for (size_t i = 0; i < WX_BLE_MAX_PHONES; i++) {
        phone_t *p = &s.phones[i];
        if (p->conn != BLE_HS_CONN_HANDLE_NONE) {
            reset_phone(p);
            p->conn = BLE_HS_CONN_HANDLE_NONE;
            emit_state(p, WX_PHONE_OFF);
        }
    }
    emit_stack(WX_BLE_STACK_FAILED);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s.own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "无法确定蓝牙地址: %d", rc);
        emit_stack(WX_BLE_STACK_FAILED);
        return;
    }
    emit_stack(WX_BLE_STACK_RUNNING);
    advertise();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void wx_ble_device_name(char *buf, size_t cap)
{
    uint8_t mac[6] = { 0 };
    (void)esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(buf, cap, "Passport Alert %02X%02X", mac[4], mac[5]);
}

esp_err_t wx_ble_start(wx_ble_event_cb_t cb, void *ctx)
{
    if (s.started) return ESP_ERR_INVALID_STATE;
    s.cb = cb;
    s.ctx = ctx;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // 屏幕只能显示配对码:iPhone 输入后才能绑定,防止旁人随意配对。
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    char name[WX_BLE_DEVICE_NAME_MAX];
    wx_ble_device_name(name, sizeof(name));
    int rc = ble_svc_gap_device_name_set(name);
    if (rc == 0) rc = ble_svc_gap_device_appearance_set(APPEARANCE_GENERIC_HID);
    if (rc == 0) rc = ble_att_set_preferred_mtu(PREFERRED_MTU);
    if (rc != 0) {
        ESP_LOGE(TAG, "GAP/ATT 配置失败: %d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }
    ble_store_config_init();

    struct ble_npl_eventq *q = nimble_port_get_dflt_eventq();
    for (size_t i = 0; i < WX_BLE_MAX_PHONES; i++) {
        phone_t *p = &s.phones[i];
        p->index = (uint8_t)i;
        p->conn = BLE_HS_CONN_HANDLE_NONE;
        ble_npl_callout_init(&p->retry_callout, q, on_retry, p);
        ble_npl_callout_init(&p->ds_timeout_callout, q, on_ds_timeout, p);
        ble_npl_callout_init(&p->sec_check_callout, q, on_sec_check, p);
        ble_npl_callout_init(&p->resub_callout, q, on_resub, p);
        reset_phone(p);
    }
    ble_npl_event_init(&s_forget_event, on_forget, NULL);

    s.started = true;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

void wx_ble_forget_phones(void)
{
    if (!s.started || ble_npl_event_is_queued(&s_forget_event)) return;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_forget_event);
}
