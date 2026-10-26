// main/wx_sound.h —— 提示音播放任务。PCM 写入会阻塞,所以放在独立任务里;
// 应用只需投递请求。待机时 ES8311 保持休眠,避免喇叭底噪。
#pragma once

#include "esp_err.h"
#include "wx_lang.h"
#include <stdint.h>

// 需先 bsp_audio_init() 成功。
esp_err_t wx_sound_start(void);

// 非阻塞:请求播放一次某个应用(wx_app_t)的提示音:“叮咚”加这种语言的人声,
// 如 "New QQ message" / “你有新的 QQ 消息”。未启动时忽略;播放中最多再排一条,多出的请求丢弃。
void wx_sound_play(uint8_t app, wx_lang_t lang);
