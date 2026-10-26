// 提示音合成的 host 测试:不依赖 SDK,直接验证 wx_chime.c。
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "wx_chime.h"

int main(void) {
    wx_chime_t c;
    int16_t buf[300];

    // 16 kHz:一遍 110+20+260+160 ms = 8800 个采样,重复 WX_CHIME_REPEAT 遍。
    wx_chime_init(&c, 16000);
    assert(c.total == 8800u * WX_CHIME_REPEAT);

    size_t total = 0;
    int peak = 0;
    int first = -1;
    size_t n;
    while ((n = wx_chime_render(&c, buf, sizeof(buf) / sizeof(buf[0]))) > 0) {
        for (size_t i = 0; i < n; i++) {
            int v = abs(buf[i]);
            if (v > peak) peak = v;
            if (total + i < 10 && first < v) first = v;   // 起音段开头必须很轻
        }
        total += n;
    }
    assert(total == c.total);
    assert(peak > 15000 && peak <= 19660);                 // 有声音,且不超过设定峰值
    assert(first < 3000);
    assert(wx_chime_render(&c, buf, 10) == 0);             // 播完后不再输出

    // 两遍之间的静音段确实是静音:第一遍最后 100 ms 落在 160 ms 间隔内。
    wx_chime_init(&c, 16000);
    int16_t *all = malloc(c.total * sizeof(int16_t));
    assert(all);
    assert(wx_chime_render(&c, all, c.total) == c.total);
    for (size_t i = 8800 - 1600; i < 8800; i++) assert(all[i] == 0);
    free(all);

    wx_chime_init(&c, 0);
    assert(wx_chime_render(&c, buf, 10) == 0);

    printf("test_wx_chime: PASS\n");
    return 0;
}
