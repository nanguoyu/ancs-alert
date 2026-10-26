// main/wx_wifi_list.c —— 保存的 Wi-Fi 网络(纯逻辑)。
#include "wx_wifi_list.h"

#include <string.h>

void wx_wifi_list_init(wx_wifi_list_t *list)
{
    memset(list, 0, sizeof(*list));
}

// 逐字节找结尾,不读到字符串之外。
static bool bounded(const char *s, size_t max)
{
    for (size_t i = 0; i <= max; i++) {
        if (s[i] == '\0') return true;
    }
    return false;
}

bool wx_wifi_ssid_valid(const char *ssid)
{
    return ssid && ssid[0] != '\0' && bounded(ssid, WX_WIFI_SSID_MAX);
}

static bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool wx_wifi_password_valid(const char *password)
{
    if (!password || !bounded(password, WX_WIFI_PASS_MAX)) return false;
    const size_t len = strlen(password);
    if (len == 0) return true;
    if (len == 64) {
        for (size_t i = 0; i < len; i++) {
            if (!is_hex(password[i])) return false;
        }
        return true;
    }
    if (len < 8 || len > 63) return false;
    for (size_t i = 0; i < len; i++) {
        // WPA2 口令只允许可打印 ASCII。
        if (password[i] < 0x20 || password[i] > 0x7E) return false;
    }
    return true;
}

bool wx_wifi_list_valid(const wx_wifi_list_t *list)
{
    if (list->count > WX_WIFI_MAX) return false;
    for (size_t i = 0; i < list->count; i++) {
        if (!wx_wifi_ssid_valid(list->items[i].ssid) ||
            !wx_wifi_password_valid(list->items[i].password)) {
            return false;
        }
    }
    return true;
}

int wx_wifi_list_find(const wx_wifi_list_t *list, const char *ssid)
{
    if (!ssid) return -1;
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].ssid, ssid) == 0) return (int)i;
    }
    return -1;
}

static void delete_at(wx_wifi_list_t *list, size_t index)
{
    memmove(&list->items[index], &list->items[index + 1],
            (list->count - index - 1) * sizeof(list->items[0]));
    list->count--;
    memset(&list->items[list->count], 0, sizeof(list->items[0]));
}

wx_wifi_put_t wx_wifi_list_put(wx_wifi_list_t *list, const char *ssid, const char *password)
{
    if (!wx_wifi_ssid_valid(ssid)) return WX_WIFI_BAD_SSID;
    if (!wx_wifi_password_valid(password)) return WX_WIFI_BAD_PASSWORD;

    const int existing = wx_wifi_list_find(list, ssid);
    if (existing < 0 && list->count == WX_WIFI_MAX) return WX_WIFI_FULL;
    if (existing >= 0) delete_at(list, (size_t)existing);

    memmove(&list->items[1], &list->items[0], list->count * sizeof(list->items[0]));
    wx_wifi_cred_t *item = &list->items[0];
    memset(item, 0, sizeof(*item));
    strcpy(item->ssid, ssid);
    strcpy(item->password, password);
    list->count++;
    return existing >= 0 ? WX_WIFI_REPLACED : WX_WIFI_ADDED;
}

bool wx_wifi_list_remove(wx_wifi_list_t *list, const char *ssid)
{
    const int index = wx_wifi_list_find(list, ssid);
    if (index < 0) return false;
    delete_at(list, (size_t)index);
    return true;
}

size_t wx_wifi_dedupe(wx_wifi_seen_t *aps, size_t n)
{
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        if (aps[i].ssid[0] == '\0') continue;
        size_t j = 0;
        while (j < kept && strcmp(aps[j].ssid, aps[i].ssid) != 0) j++;
        if (j == kept) {
            aps[kept++] = aps[i];
        } else if (aps[i].rssi > aps[j].rssi) {
            aps[j] = aps[i];
        }
    }
    // 插入排序:最多几十个。
    for (size_t i = 1; i < kept; i++) {
        wx_wifi_seen_t cur = aps[i];
        size_t j = i;
        while (j > 0 && aps[j - 1].rssi < cur.rssi) {
            aps[j] = aps[j - 1];
            j--;
        }
        aps[j] = cur;
    }
    return kept;
}

size_t wx_wifi_rank(const wx_wifi_list_t *list, const wx_wifi_seen_t *aps, size_t n,
                    uint8_t order[WX_WIFI_MAX])
{
    int8_t best[WX_WIFI_MAX];
    bool seen[WX_WIFI_MAX] = { false };
    for (size_t i = 0; i < n; i++) {
        const int index = wx_wifi_list_find(list, aps[i].ssid);
        if (index < 0) continue;
        if (!seen[index] || aps[i].rssi > best[index]) best[index] = aps[i].rssi;
        seen[index] = true;
    }

    size_t count = 0;
    for (uint8_t index = 0; index < list->count; index++) {
        if (!seen[index]) continue;
        size_t j = count;
        while (j > 0 && best[order[j - 1]] < best[index]) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = index;
        count++;
    }
    return count;
}
