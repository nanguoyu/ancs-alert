// main/wx_chime.c —— 新消息提示音合成(纯逻辑)。
#include "wx_chime.h"

// sin(i * pi / 128) * 32767, i = 0..64:四分之一周期,其余象限由对称得到。
static const int16_t QUARTER_SINE[65] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602,
    6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767,
};

typedef struct {
    uint32_t hz;
    uint32_t ms;
} chime_note_t;

// 一遍:叮(短) — 咚(长) — 间隔。频率为 E6 / A6。
static const chime_note_t NOTES[] = {
    { 1319, 110 },
    { 0, 20 },
    { 1760, 260 },
    { 0, 160 },
};
#define NOTE_COUNT (sizeof(NOTES) / sizeof(NOTES[0]))
#define ATTACK_MS 4
// 峰值约为满幅的 60%,给 codec 和小喇叭留余量。
#define PEAK_Q15 19660

static int32_t sine(uint32_t phase)
{
    uint32_t index = phase >> 24;           // 0..255
    uint32_t k = index & 63;
    switch (index >> 6) {
    case 0: return QUARTER_SINE[k];
    case 1: return QUARTER_SINE[64 - k];
    case 2: return -QUARTER_SINE[k];
    default: return -QUARTER_SINE[64 - k];
    }
}

static size_t samples_for(uint32_t rate, uint32_t ms)
{
    return (size_t)((uint64_t)rate * ms / 1000);
}

static size_t pass_samples(uint32_t rate)
{
    size_t total = 0;
    for (size_t i = 0; i < NOTE_COUNT; i++) total += samples_for(rate, NOTES[i].ms);
    return total;
}

void wx_chime_init(wx_chime_t *chime, uint32_t rate)
{
    chime->rate = rate;
    chime->pos = 0;
    chime->phase = 0;
    chime->total = rate ? pass_samples(rate) * WX_CHIME_REPEAT : 0;
}

static int16_t sample_at(wx_chime_t *chime, size_t pos_in_pass)
{
    size_t start = 0;
    for (size_t i = 0; i < NOTE_COUNT; i++) {
        size_t len = samples_for(chime->rate, NOTES[i].ms);
        if (pos_in_pass < start + len) {
            if (NOTES[i].hz == 0 || len == 0) return 0;
            size_t t = pos_in_pass - start;
            size_t attack = samples_for(chime->rate, ATTACK_MS);
            // 包络:4 ms 线性起音,之后线性衰减到 0,避免喀哒声。
            int64_t env;
            if (t < attack && attack > 0) {
                env = (int64_t)PEAK_Q15 * (int64_t)t / (int64_t)attack;
            } else {
                env = (int64_t)PEAK_Q15 * (int64_t)(len - t) / (int64_t)(len - attack);
            }
            chime->phase += (uint32_t)(((uint64_t)NOTES[i].hz << 32) / chime->rate);
            return (int16_t)((sine(chime->phase) * env) >> 15);
        }
        start += len;
    }
    return 0;
}

size_t wx_chime_render(wx_chime_t *chime, int16_t *out, size_t max_samples)
{
    if (!chime->total) return 0;
    size_t pass = chime->total / WX_CHIME_REPEAT;
    size_t n = 0;
    while (n < max_samples && chime->pos < chime->total) {
        out[n++] = sample_at(chime, chime->pos % pass);
        chime->pos++;
    }
    return n;
}
