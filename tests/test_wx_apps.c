// 应用对照表的 host 测试:不依赖 SDK,直接验证 wx_apps.c。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wx_apps.h"

int main(void) {
    assert(wx_app_from_bundle("com.tencent.xin", false) == WX_APP_WECHAT);
    assert(wx_app_from_bundle("com.apple.MobileSMS", false) == WX_APP_MESSAGES);
    assert(wx_app_from_bundle("ph.telegra.Telegraph", false) == WX_APP_TELEGRAM);
    assert(wx_app_from_bundle("com.tencent.mqq", false) == WX_APP_QQ);
    assert(wx_app_from_bundle("com.tencent.tim", false) == WX_APP_QQ);
    assert(wx_app_from_bundle("net.whatsapp.WhatsApp", false) == WX_APP_WHATSAPP);
    assert(wx_app_from_bundle("net.whatsapp.WhatsAppSMB", false) == WX_APP_WHATSAPP);
    assert(wx_app_from_bundle("org.whispersystems.signal", false) == WX_APP_SIGNAL);
    assert(wx_app_from_bundle("jp.naver.line", false) == WX_APP_LINE);

    // 其他应用、前缀相同的标识、被截断的标识、空指针都不提醒。
    assert(wx_app_from_bundle("com.reddit.Reddit", false) == WX_APP_UNKNOWN);
    assert(wx_app_from_bundle("com.tencent.xinx", false) == WX_APP_UNKNOWN);
    assert(wx_app_from_bundle("com.tencent.xin", true) == WX_APP_UNKNOWN);
    assert(wx_app_from_bundle(NULL, false) == WX_APP_UNKNOWN);

    assert(strcmp(wx_app_name(WX_APP_WECHAT, WX_LANG_ZH), "微信") == 0);
    assert(strcmp(wx_app_name(WX_APP_WECHAT, WX_LANG_EN), "WeChat") == 0);
    assert(strcmp(wx_app_name(WX_APP_MESSAGES, WX_LANG_EN), "Messages") == 0);
    assert(strcmp(wx_app_name(WX_APP_QQ, WX_LANG_ZH), "QQ") == 0);
    assert(strcmp(wx_app_name(WX_APP_LINE, WX_LANG_EN), "LINE") == 0);
    assert(strcmp(wx_app_name(WX_APP_UNKNOWN, WX_LANG_EN), "") == 0);
    assert(strcmp(wx_app_name(WX_APP_WECHAT, WX_LANG_COUNT), "WeChat") == 0);
    for (uint8_t app = 0; app < WX_APP_COUNT; app++) {
        assert(wx_app_name(app, WX_LANG_EN)[0] && wx_app_name(app, WX_LANG_ZH)[0]);
        assert(wx_app_title_is_generic(app, wx_app_name(app, WX_LANG_EN)));
        assert(wx_app_title_is_generic(app, wx_app_name(app, WX_LANG_ZH)));
    }
    assert(wx_app_color(WX_APP_WECHAT) == 0x07C160);
    assert(wx_app_color(WX_APP_UNKNOWN) == wx_app_color(WX_APP_WECHAT));

    assert(WX_APP_BIT(WX_APP_MESSAGES) == 0x02);
    assert(WX_APP_ALL_BITS == 0x7F);

    // 通用标题:不当作联系人显示。
    assert(wx_app_title_is_generic(WX_APP_WECHAT, "微信"));
    assert(wx_app_title_is_generic(WX_APP_WECHAT, "Weixin"));
    assert(wx_app_title_is_generic(WX_APP_QQ, "TIM"));
    assert(wx_app_title_is_generic(WX_APP_WHATSAPP, "WhatsApp Business"));
    assert(wx_app_title_is_generic(WX_APP_MESSAGES, ""));
    assert(wx_app_title_is_generic(WX_APP_MESSAGES, NULL));
    assert(!wx_app_title_is_generic(WX_APP_WECHAT, "\xE5\xBC\xA0\xE4\xB8\x89"));   // 张三
    assert(!wx_app_title_is_generic(WX_APP_TELEGRAM, "Alice"));

    printf("test_wx_apps: PASS\n");
    return 0;
}
