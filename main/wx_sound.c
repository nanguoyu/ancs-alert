// main/wx_sound.c —— 提示音播放任务。
#include "wx_sound.h"
#include "wx_apps.h"
#include "wx_chime.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stddef.h>

static const char *TAG = "wx_sound";

#define SAMPLE_RATE   16000
#define CHUNK_SAMPLES 256
#define VOLUME        75
#define VOICE_CHUNK_BYTES 1024

// 各应用的人声,英文和中文各一段:16 kHz / 16 bit / 单声道 PCM,见 tools/gen_wx_voice.py。
// 文件在本地生成、不入库;main/CMakeLists.txt 只嵌入存在的文件并定义 WX_HAVE_VOICE_<应用>_<语言>。
// 缺了哪个,那个应用在那种语言下就只播“叮咚”。
#define VOICE(app, lang)                                                                     \
    extern const uint8_t s_voice_##app##_##lang##_start[] asm("_binary_wx_voice_" #app "_" #lang "_16k_pcm_start"); \
    extern const uint8_t s_voice_##app##_##lang##_end[] asm("_binary_wx_voice_" #app "_" #lang "_16k_pcm_end");
#define VOICE_ENTRY(app, lang) { s_voice_##app##_##lang##_start, s_voice_##app##_##lang##_end }

#ifdef WX_HAVE_VOICE_WECHAT_EN
VOICE(wechat, en)
#endif
#ifdef WX_HAVE_VOICE_MESSAGES_EN
VOICE(messages, en)
#endif
#ifdef WX_HAVE_VOICE_TELEGRAM_EN
VOICE(telegram, en)
#endif
#ifdef WX_HAVE_VOICE_QQ_EN
VOICE(qq, en)
#endif
#ifdef WX_HAVE_VOICE_WHATSAPP_EN
VOICE(whatsapp, en)
#endif
#ifdef WX_HAVE_VOICE_SIGNAL_EN
VOICE(signal, en)
#endif
#ifdef WX_HAVE_VOICE_LINE_EN
VOICE(line, en)
#endif
#ifdef WX_HAVE_VOICE_WECHAT_ZH
VOICE(wechat, zh)
#endif
#ifdef WX_HAVE_VOICE_MESSAGES_ZH
VOICE(messages, zh)
#endif
#ifdef WX_HAVE_VOICE_TELEGRAM_ZH
VOICE(telegram, zh)
#endif
#ifdef WX_HAVE_VOICE_QQ_ZH
VOICE(qq, zh)
#endif
#ifdef WX_HAVE_VOICE_WHATSAPP_ZH
VOICE(whatsapp, zh)
#endif
#ifdef WX_HAVE_VOICE_SIGNAL_ZH
VOICE(signal, zh)
#endif
#ifdef WX_HAVE_VOICE_LINE_ZH
VOICE(line, zh)
#endif

typedef struct {
    const uint8_t *start;
    const uint8_t *end;
} voice_t;

static const voice_t VOICES[WX_LANG_COUNT][WX_APP_COUNT] = {
    [WX_LANG_EN] = {
#ifdef WX_HAVE_VOICE_WECHAT_EN
        [WX_APP_WECHAT] = VOICE_ENTRY(wechat, en),
#endif
#ifdef WX_HAVE_VOICE_MESSAGES_EN
        [WX_APP_MESSAGES] = VOICE_ENTRY(messages, en),
#endif
#ifdef WX_HAVE_VOICE_TELEGRAM_EN
        [WX_APP_TELEGRAM] = VOICE_ENTRY(telegram, en),
#endif
#ifdef WX_HAVE_VOICE_QQ_EN
        [WX_APP_QQ] = VOICE_ENTRY(qq, en),
#endif
#ifdef WX_HAVE_VOICE_WHATSAPP_EN
        [WX_APP_WHATSAPP] = VOICE_ENTRY(whatsapp, en),
#endif
#ifdef WX_HAVE_VOICE_SIGNAL_EN
        [WX_APP_SIGNAL] = VOICE_ENTRY(signal, en),
#endif
#ifdef WX_HAVE_VOICE_LINE_EN
        [WX_APP_LINE] = VOICE_ENTRY(line, en),
#endif
    },
    [WX_LANG_ZH] = {
#ifdef WX_HAVE_VOICE_WECHAT_ZH
        [WX_APP_WECHAT] = VOICE_ENTRY(wechat, zh),
#endif
#ifdef WX_HAVE_VOICE_MESSAGES_ZH
        [WX_APP_MESSAGES] = VOICE_ENTRY(messages, zh),
#endif
#ifdef WX_HAVE_VOICE_TELEGRAM_ZH
        [WX_APP_TELEGRAM] = VOICE_ENTRY(telegram, zh),
#endif
#ifdef WX_HAVE_VOICE_QQ_ZH
        [WX_APP_QQ] = VOICE_ENTRY(qq, zh),
#endif
#ifdef WX_HAVE_VOICE_WHATSAPP_ZH
        [WX_APP_WHATSAPP] = VOICE_ENTRY(whatsapp, zh),
#endif
#ifdef WX_HAVE_VOICE_SIGNAL_ZH
        [WX_APP_SIGNAL] = VOICE_ENTRY(signal, zh),
#endif
#ifdef WX_HAVE_VOICE_LINE_ZH
        [WX_APP_LINE] = VOICE_ENTRY(line, zh),
#endif
    },
};

// 一次播放请求。
typedef struct {
    uint8_t app;
    uint8_t lang;
} request_t;

#define REQUEST_DEPTH 2

static TaskHandle_t s_task;
static QueueHandle_t s_requests;
static int16_t s_buf[CHUNK_SAMPLES];

// 本板没有功放使能脚(BSP_I2S_PA_CTRL = -1),ES8311 一直工作时喇叭会持续出底噪,
// 所以待机时让 codec 休眠,只在播放提示音期间唤醒。
static void audio_idle(void)
{
    esp_err_t err = bsp_audio_sleep();
    if (err != ESP_OK) ESP_LOGW(TAG, "ES8311 休眠失败: %s", esp_err_to_name(err));
}

static void play_once(uint8_t app, wx_lang_t lang)
{
    esp_err_t err = bsp_audio_wake();
    if (err == ESP_OK) err = bsp_audio_set_format(SAMPLE_RATE, 16, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开音频失败: %s", esp_err_to_name(err));
        audio_idle();
        return;
    }
    bsp_audio_set_volume(VOLUME);

    wx_chime_t chime;
    wx_chime_init(&chime, SAMPLE_RATE);
    size_t n;
    while (err == ESP_OK && (n = wx_chime_render(&chime, s_buf, CHUNK_SAMPLES)) > 0) {
        err = bsp_audio_write(s_buf, n * sizeof(s_buf[0]));
    }
    const voice_t *voice = app < WX_APP_COUNT && lang < WX_LANG_COUNT ? &VOICES[lang][app] : NULL;
    if (voice && voice->start) {
        // I2S 写入会把数据拷进 DMA 缓冲,可直接从 Flash 读。
        const size_t voice_bytes = (size_t)(voice->end - voice->start);
        for (size_t off = 0; err == ESP_OK && off < voice_bytes; off += VOICE_CHUNK_BYTES) {
            size_t left = voice_bytes - off;
            err = bsp_audio_write(voice->start + off, left < VOICE_CHUNK_BYTES ? left : VOICE_CHUNK_BYTES);
        }
        // 人声末尾只留了约 30 ms 静音:补一小段零,避免休眠截掉最后一个字。
        for (size_t i = 0; i < CHUNK_SAMPLES; i++) s_buf[i] = 0;
        for (int i = 0; err == ESP_OK && i < 8; i++) err = bsp_audio_write(s_buf, sizeof(s_buf));
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "播放失败: %s", esp_err_to_name(err));
    // 合成提示音以 160 ms 静音结尾,人声后也补了静音,休眠截掉 DMA 尾巴听不出来。
    audio_idle();
}

static void sound_task(void *arg)
{
    (void)arg;
    request_t req;
    for (;;) {
        if (xQueueReceive(s_requests, &req, portMAX_DELAY) == pdTRUE) play_once(req.app, (wx_lang_t)req.lang);
    }
}

esp_err_t wx_sound_start(void)
{
    if (s_task) return ESP_OK;
    // 之后所有音频操作都只在 sound_task 中串行进行。
    audio_idle();
    s_requests = xQueueCreate(REQUEST_DEPTH, sizeof(request_t));
    if (!s_requests) return ESP_ERR_NO_MEM;
    if (xTaskCreate(sound_task, "wx_sound", 3072, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void wx_sound_play(uint8_t app, wx_lang_t lang)
{
    const request_t req = { .app = app, .lang = (uint8_t)lang };
    if (s_task) (void)xQueueSend(s_requests, &req, 0);
}
