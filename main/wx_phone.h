// main/wx_phone.h —— 把 iPhone 设备信息服务里的型号代码(如 "iPhone11,8")转成可读名称。
// 纯逻辑,由 tests/test_wx_phone.c 覆盖。
#pragma once

#include <stddef.h>

#define WX_PHONE_NAME_MAX 23

// 已知型号输出如 "iPhone XR";未知型号原样输出代码;空串输出 "iPhone"。
// out 至少 WX_PHONE_NAME_MAX + 1 字节。
void wx_phone_model_name(const char *model, char *out, size_t cap);

// 界面上的短名:去掉开头的 "iPhone "(“iPhone XR” → “XR”),去掉后为空则保留原名。
const char *wx_phone_short_name(const char *name);
