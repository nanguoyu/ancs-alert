// main/wx_dns.h —— 配网热点上的 DNS:任何域名的 A 记录都回答板子自己的地址,
// 手机的联网检测(如 captive.apple.com)因此打开设置网页。纯逻辑,由 tests/test_wx_dns.c 覆盖。
#pragma once

#include <stddef.h>
#include <stdint.h>

// 把 msg 里的一条 DNS 查询原地改写成回答:只保留第一个问题;A/IN 回答 ip,
// 其他类型回答“没有这种记录”。msg 的容量为 cap。返回回复长度;不是合法查询时返回 0(不回)。
size_t wx_dns_reply(uint8_t *msg, size_t len, size_t cap, const uint8_t ip[4]);
