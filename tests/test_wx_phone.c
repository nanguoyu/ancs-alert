// iPhone 型号名称的 host 测试:不依赖 SDK,直接验证 wx_phone.c。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wx_phone.h"

int main(void) {
    char name[WX_PHONE_NAME_MAX + 1];

    wx_phone_model_name("iPhone11,8", name, sizeof(name));
    assert(strcmp(name, "iPhone XR") == 0);
    wx_phone_model_name("iPhone17,1", name, sizeof(name));
    assert(strcmp(name, "iPhone 16 Pro") == 0);

    // 未知型号原样显示,空串与 NULL 显示通用名。
    wx_phone_model_name("iPhone99,9", name, sizeof(name));
    assert(strcmp(name, "iPhone99,9") == 0);
    wx_phone_model_name("", name, sizeof(name));
    assert(strcmp(name, "iPhone") == 0);
    wx_phone_model_name(NULL, name, sizeof(name));
    assert(strcmp(name, "iPhone") == 0);

    // 输出缓冲小也不会越界。
    char tiny[6];
    wx_phone_model_name("iPhone12,5", tiny, sizeof(tiny));
    assert(strlen(tiny) == sizeof(tiny) - 1);

    // 短名。
    assert(strcmp(wx_phone_short_name("iPhone XR"), "XR") == 0);
    assert(strcmp(wx_phone_short_name("iPhone 16 Pro"), "16 Pro") == 0);
    assert(strcmp(wx_phone_short_name("iPhone"), "iPhone") == 0);
    assert(strcmp(wx_phone_short_name("iPhone "), "iPhone ") == 0);
    assert(strcmp(wx_phone_short_name("iPhone99,9"), "iPhone99,9") == 0);
    assert(strcmp(wx_phone_short_name(NULL), "") == 0);

    printf("test_wx_phone: PASS\n");
    return 0;
}
