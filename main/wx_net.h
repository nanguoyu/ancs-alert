// main/wx_net.h —— Wi-Fi:连接已保存网络中信号最强的一个,断了自动重连;配网时另开一个
// 热点(AP+STA 同时工作),让手机连上来打开设置网页。
// 没有保存网络、也没在配网时不初始化 Wi-Fi,不占内存和电。
#pragma once

#include "esp_err.h"
#include "wx_wifi_list.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    WX_NET_OFF = 0,         // 没有保存的网络
    WX_NET_CONNECTING,
    WX_NET_CONNECTED,
    WX_NET_FAILED,          // 连不上(原因见 error);过一会儿自动重试
} wx_net_state_t;

#define WX_NET_SETUP_PASS_LEN 8

typedef struct {
    wx_net_state_t state;
    char ssid[WX_WIFI_SSID_MAX + 1];         // 正在连或已连上的网络
    char ip[16];
    int8_t rssi;
    const char *error;                       // FAILED 时的原因(中文短句),否则 NULL
    bool setup;                              // 配网热点已开
    bool setup_failed;                       // 最近一次打开热点失败
    char setup_ssid[WX_WIFI_SSID_MAX + 1];
    char setup_pass[WX_NET_SETUP_PASS_LEN + 1];
    uint8_t setup_clients;                   // 连在热点上的手机数
} wx_net_status_t;

// 状态有变化。运行在联网任务或系统事件任务:只能入队,读状态请调用 wx_net_status()。
typedef void (*wx_net_cb_t)(void *ctx);

// 需在 nvs_flash_init() 和 wx_settings_init() 之后调用。
esp_err_t wx_net_start(wx_net_cb_t cb, void *ctx);

// 保存的网络有变化。prefer 非 NULL 时马上连这个网络(刚在网页上添加或改了密码的)。
void wx_net_reload(const char *prefer);

void wx_net_status(wx_net_status_t *out);
bool wx_net_online(void);

// 扫描附近的网络(阻塞约 2~4 秒),结果已去重并按信号排序。不能在联网任务里调用。
// 正在连接网络时扫描不了,返回 ESP_ERR_INVALID_STATE。
esp_err_t wx_net_scan(wx_wifi_seen_t *out, size_t cap, size_t *count);

// 打开 / 关闭配网热点,异步完成,完成后通过回调通知(status.setup)。
// 热点名形如 "Passport-1A2B"(后缀与蓝牙名相同),每次打开都换一个随机的 8 位密码。
void wx_net_setup_begin(void);
void wx_net_setup_end(void);
