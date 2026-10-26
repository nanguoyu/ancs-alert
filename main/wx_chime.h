// main/wx_chime.h —— 新消息提示音:两段正弦“叮—咚”,重复 WX_CHIME_REPEAT 次,
// 之后由 wx_sound 接着播人声提示。
// 纯整数合成(ESP32-C3 无 FPU),不依赖 ESP-IDF,由 tests/test_wx_chime.c 覆盖。
#pragma once

#include <stddef.h>
#include <stdint.h>

#define WX_CHIME_REPEAT 1

typedef struct {
    uint32_t rate;      // 采样率 Hz
    size_t pos;         // 已输出采样数
    size_t total;       // 整段采样数
    uint32_t phase;     // 32-bit 相位累加器
} wx_chime_t;

void wx_chime_init(wx_chime_t *chime, uint32_t rate);

// 写出最多 max_samples 个 16-bit 单声道采样,返回实际数量;全部播完后返回 0。
size_t wx_chime_render(wx_chime_t *chime, int16_t *out, size_t max_samples);
