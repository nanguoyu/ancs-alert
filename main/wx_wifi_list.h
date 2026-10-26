// main/wx_wifi_list.h —— 保存的 Wi-Fi 网络:增加、替换(同名改密码)、删除,以及按信号强弱
// 挑选要连接的网络。纯逻辑,不依赖 ESP-IDF,由 tests/test_wx_wifi_list.c 覆盖。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WX_WIFI_MAX      5
#define WX_WIFI_SSID_MAX 32     // 802.11 的 SSID 最长 32 字节
#define WX_WIFI_PASS_MAX 64     // WPA2 口令 8..63 个字符,或 64 位十六进制 PSK

typedef struct {
    char ssid[WX_WIFI_SSID_MAX + 1];
    char password[WX_WIFI_PASS_MAX + 1];   // 空串表示开放网络
} wx_wifi_cred_t;

// 最近添加或修改的排在最前。
typedef struct {
    wx_wifi_cred_t items[WX_WIFI_MAX];
    uint8_t count;
} wx_wifi_list_t;

typedef enum {
    WX_WIFI_ADDED = 0,
    WX_WIFI_REPLACED,       // 已有同名网络:换成新密码
    WX_WIFI_FULL,           // 已存满 WX_WIFI_MAX 个,要先删一个
    WX_WIFI_BAD_SSID,
    WX_WIFI_BAD_PASSWORD,
} wx_wifi_put_t;

void wx_wifi_list_init(wx_wifi_list_t *list);
// 列表内容是否合法(从 NVS 读出后检查,防止旧版本或损坏的数据)。
bool wx_wifi_list_valid(const wx_wifi_list_t *list);

wx_wifi_put_t wx_wifi_list_put(wx_wifi_list_t *list, const char *ssid, const char *password);
bool wx_wifi_list_remove(wx_wifi_list_t *list, const char *ssid);
// 返回下标,没有返回 -1。
int wx_wifi_list_find(const wx_wifi_list_t *list, const char *ssid);

bool wx_wifi_ssid_valid(const char *ssid);
bool wx_wifi_password_valid(const char *password);

// 一次扫描里看到的网络。
typedef struct {
    char ssid[WX_WIFI_SSID_MAX + 1];
    int8_t rssi;
    bool secure;
} wx_wifi_seen_t;

// 整理扫描结果:去掉隐藏网络,同名的只留信号最强的一个,按信号从强到弱排序。返回剩下的个数。
size_t wx_wifi_dedupe(wx_wifi_seen_t *aps, size_t n);

// 扫描到的已保存网络,按信号从强到弱把它们在 list 里的下标写进 order。返回个数。
size_t wx_wifi_rank(const wx_wifi_list_t *list, const wx_wifi_seen_t *aps, size_t n,
                    uint8_t order[WX_WIFI_MAX]);
