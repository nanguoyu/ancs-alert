// 转发文字的 host 测试:不依赖 SDK,直接验证 wx_tg_text.c。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wx_tg_text.h"

#define ZHANGSAN "\xE5\xBC\xA0\xE4\xB8\x89"   // 张三
#define NIHAO    "\xE4\xBD\xA0\xE5\xA5\xBD"   // 你好

static void test_format(void) {
    char out[256];

    const wx_lang_t zh = WX_LANG_ZH;
    assert(wx_tg_format(out, sizeof(out), zh, "微信", NULL, ZHANGSAN, false, NIHAO) == strlen(out));
    assert(strcmp(out, "【微信】" ZHANGSAN "\n" NIHAO) == 0);

    wx_tg_format(out, sizeof(out), zh, "QQ", "XR", "Bob", false, "hi");
    assert(strcmp(out, "【QQ，XR】Bob\nhi") == 0);

    // 不转发正文。
    wx_tg_format(out, sizeof(out), zh, "信息", "", ZHANGSAN, false, NULL);
    assert(strcmp(out, "【信息】" ZHANGSAN " 发来新消息") == 0);

    // 标题只是应用名(关闭了消息详情):不写联系人。
    wx_tg_format(out, sizeof(out), zh, "微信", NULL, "微信", true, "你收到了一条消息");
    assert(strcmp(out, "【微信】你收到了一条消息") == 0);
    wx_tg_format(out, sizeof(out), zh, "微信", NULL, "微信", true, "");
    assert(strcmp(out, "【微信】有新消息") == 0);
    wx_tg_format(out, sizeof(out), zh, "微信", NULL, "", false, NULL);
    assert(strcmp(out, "【微信】有新消息") == 0);
}

static void test_format_english(void) {
    char out[256];
    const wx_lang_t en = WX_LANG_EN;
    wx_tg_format(out, sizeof(out), en, "WhatsApp", "XR", "Alice", false, "See you at 7");
    assert(strcmp(out, "[WhatsApp, XR] Alice\nSee you at 7") == 0);
    wx_tg_format(out, sizeof(out), en, "Messages", NULL, "Bob", false, NULL);
    assert(strcmp(out, "[Messages] Bob sent a new message") == 0);
    wx_tg_format(out, sizeof(out), en, "WeChat", "", "WeChat", true, "");
    assert(strcmp(out, "[WeChat] New message") == 0);
    wx_tg_format(out, sizeof(out), en, "Signal", NULL, "Signal", true, "You have a new message");
    assert(strcmp(out, "[Signal] You have a new message") == 0);
}

static void test_format_truncates_on_char_boundary(void) {
    char out[16];
    // "【微信】" 是 12 字节,只剩 3 字节:放得下一个汉字,放不下第二个的一半。
    size_t n = wx_tg_format(out, sizeof(out), WX_LANG_ZH, "微信", NULL, NIHAO, false, NULL);
    assert(n == 15 && strcmp(out, "【微信】\xE4\xBD\xA0") == 0);
    char tiny[14];
    n = wx_tg_format(tiny, sizeof(tiny), WX_LANG_ZH, "微信", NULL, NIHAO, false, NULL);
    assert(n == 12 && strcmp(tiny, "【微信】") == 0);
    char one[1];
    assert(wx_tg_format(one, sizeof(one), WX_LANG_ZH, "微信", NULL, "x", false, "y") == 0 && one[0] == '\0');
}

static void test_token(void) {
    const char *good = "123456789:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsaw";
    assert(wx_tg_token_valid(good));
    assert(wx_tg_token_valid("12345:abcdefghijklmnopqrstuvwxyz_-0123"));
    assert(!wx_tg_token_valid(""));
    assert(!wx_tg_token_valid(NULL));
    assert(!wx_tg_token_valid("123456789"));
    assert(!wx_tg_token_valid("1234:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsaw"));          // 编号太短
    assert(!wx_tg_token_valid("123456789:short"));
    assert(!wx_tg_token_valid("123456789:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsaw "));   // 末尾空格
    assert(!wx_tg_token_valid("123456789:AAHdqTcvCH1vGWJxfSeofSAs0K5PALD/saw"));
    assert(!wx_tg_token_valid("abc:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsaw"));

    char hint[48];
    wx_tg_token_hint(good, hint, sizeof(hint));
    assert(strcmp(hint, "123456789:••••Dsaw") == 0);
    assert(strstr(hint, "AAHdq") == NULL);
    wx_tg_token_hint("bad", hint, sizeof(hint));
    assert(hint[0] == '\0');
}

int main(void) {
    test_format();
    test_format_english();
    test_format_truncates_on_char_boundary();
    test_token();
    printf("test_wx_tg_text: PASS\n");
    return 0;
}
