// main/wx_portal.h —— 配网热点上的设置网页:DNS 把所有域名指向板子,HTTP 服务器提供网页和 JSON 接口
// (Wi-Fi 增删改与扫描、Telegram Token 与聊天绑定、转发与提醒选项、界面语言)。
#pragma once

#include "esp_err.h"

// 网页上点了“完成”。运行在 httpd 任务:只能入队,不要在回调里停止网页。
typedef void (*wx_portal_done_cb_t)(void *ctx);

// 需在配网热点打开之后调用(见 wx_net_setup_begin)。
esp_err_t wx_portal_start(wx_portal_done_cb_t done, void *ctx);
void wx_portal_stop(void);
