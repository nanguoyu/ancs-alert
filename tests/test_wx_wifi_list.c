// 保存的 Wi-Fi 网络的 host 测试:不依赖 SDK,直接验证 wx_wifi_list.c。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wx_wifi_list.h"

static void test_put_replace_remove(void) {
    wx_wifi_list_t l;
    wx_wifi_list_init(&l);
    assert(wx_wifi_list_valid(&l));

    assert(wx_wifi_list_put(&l, "Home", "password1") == WX_WIFI_ADDED);
    assert(wx_wifi_list_put(&l, "Office", "") == WX_WIFI_ADDED);          // 开放网络
    assert(l.count == 2 && strcmp(l.items[0].ssid, "Office") == 0);       // 新加的排最前

    // 同名:换密码并移到最前,不新增。
    assert(wx_wifi_list_put(&l, "Home", "password2") == WX_WIFI_REPLACED);
    assert(l.count == 2 && strcmp(l.items[0].ssid, "Home") == 0);
    assert(strcmp(l.items[0].password, "password2") == 0);
    assert(wx_wifi_list_find(&l, "Office") == 1);

    assert(wx_wifi_list_remove(&l, "Home"));
    assert(!wx_wifi_list_remove(&l, "Home"));
    assert(l.count == 1 && wx_wifi_list_find(&l, "Home") == -1);
    assert(wx_wifi_list_valid(&l));
}

static void test_full_list(void) {
    wx_wifi_list_t l;
    wx_wifi_list_init(&l);
    char ssid[8];
    for (int i = 0; i < WX_WIFI_MAX; i++) {
        snprintf(ssid, sizeof(ssid), "net%d", i);
        assert(wx_wifi_list_put(&l, ssid, "12345678") == WX_WIFI_ADDED);
    }
    assert(wx_wifi_list_put(&l, "extra", "12345678") == WX_WIFI_FULL);
    assert(l.count == WX_WIFI_MAX);
    // 存满时仍能替换已有网络的密码。
    assert(wx_wifi_list_put(&l, "net0", "abcdefgh") == WX_WIFI_REPLACED);
    assert(l.count == WX_WIFI_MAX && strcmp(l.items[0].ssid, "net0") == 0);
    // 删一个之后就能加。
    assert(wx_wifi_list_remove(&l, "net3"));
    assert(wx_wifi_list_put(&l, "extra", "12345678") == WX_WIFI_ADDED);
}

static void test_validation(void) {
    wx_wifi_list_t l;
    wx_wifi_list_init(&l);
    char long_ssid[40];
    memset(long_ssid, 's', 33);
    long_ssid[33] = '\0';
    assert(wx_wifi_list_put(&l, "", "12345678") == WX_WIFI_BAD_SSID);
    assert(wx_wifi_list_put(&l, long_ssid, "12345678") == WX_WIFI_BAD_SSID);
    long_ssid[32] = '\0';
    assert(wx_wifi_list_put(&l, long_ssid, "12345678") == WX_WIFI_ADDED);  // 正好 32 字节
    assert(wx_wifi_list_put(&l, "\xE5\xAE\xB6", "12345678") == WX_WIFI_ADDED);  // 中文 SSID“家”

    assert(wx_wifi_password_valid(""));
    assert(!wx_wifi_password_valid("1234567"));                          // 少于 8 个字符
    assert(wx_wifi_password_valid("12345678"));
    char pw[70];
    memset(pw, 'a', 63);
    pw[63] = '\0';
    assert(wx_wifi_password_valid(pw));
    memset(pw, 'f', 64);
    pw[64] = '\0';
    assert(wx_wifi_password_valid(pw));                                  // 64 位十六进制 PSK
    pw[10] = 'z';
    assert(!wx_wifi_password_valid(pw));
    assert(!wx_wifi_password_valid("\xE5\xAF\x86\xE7\xA0\x81\xE5\xAF\x86"));  // 非 ASCII
    assert(!wx_wifi_password_valid(NULL));
    assert(wx_wifi_list_put(&l, "x", "short") == WX_WIFI_BAD_PASSWORD);

    // 损坏的数据。
    l.count = WX_WIFI_MAX + 1;
    assert(!wx_wifi_list_valid(&l));
    l.count = 1;
    l.items[0].password[0] = 'a';
    l.items[0].password[1] = '\0';
    assert(!wx_wifi_list_valid(&l));
}

static wx_wifi_seen_t ap(const char *ssid, int rssi) {
    wx_wifi_seen_t s = { .rssi = (int8_t)rssi, .secure = true };
    strcpy(s.ssid, ssid);
    return s;
}

static void test_dedupe_and_rank(void) {
    wx_wifi_seen_t aps[] = {
        ap("Cafe", -70), ap("Home", -60), ap("", -30), ap("Home", -45), ap("Office", -80),
        ap("Cafe", -75),
    };
    size_t n = wx_wifi_dedupe(aps, sizeof(aps) / sizeof(aps[0]));
    assert(n == 3);
    assert(strcmp(aps[0].ssid, "Home") == 0 && aps[0].rssi == -45);
    assert(strcmp(aps[1].ssid, "Cafe") == 0 && aps[1].rssi == -70);
    assert(strcmp(aps[2].ssid, "Office") == 0);

    wx_wifi_list_t l;
    wx_wifi_list_init(&l);
    assert(wx_wifi_list_put(&l, "Office", "12345678") == WX_WIFI_ADDED);  // 下标 2
    assert(wx_wifi_list_put(&l, "Away", "12345678") == WX_WIFI_ADDED);    // 下标 1,没扫到
    assert(wx_wifi_list_put(&l, "Home", "12345678") == WX_WIFI_ADDED);    // 下标 0
    uint8_t order[WX_WIFI_MAX];
    assert(wx_wifi_rank(&l, aps, n, order) == 2);
    assert(order[0] == 0 && order[1] == 2);                              // Home 比 Office 强

    // 不去重也行:同名取最强的那个。
    wx_wifi_seen_t raw[] = { ap("Office", -40), ap("Home", -50), ap("Office", -90) };
    assert(wx_wifi_rank(&l, raw, 3, order) == 2 && order[0] == 2 && order[1] == 0);
    assert(wx_wifi_rank(&l, raw, 0, order) == 0);
}

int main(void) {
    test_put_replace_remove();
    test_full_list();
    test_validation();
    test_dedupe_and_rank();
    printf("test_wx_wifi_list: PASS\n");
    return 0;
}
