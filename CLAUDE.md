# CLAUDE.md

Firmware for an ESP32-C3 message alert (FoloToy AI Passport board). Up to three iPhones connect over BLE ANCS. The board alerts for WeChat, Messages, Telegram, QQ, WhatsApp, Signal and LINE, and can forward new messages to a Telegram bot over Wi-Fi. The device UI, the setup page, forwarded messages and voice prompts come in English and Simplified Chinese.

## Layout and boundaries
- `main/`: the application.
  - Pure logic stays free of ESP-IDF and LVGL and is covered by host tests: `wx_ancs_proto`, `wx_alert`, `wx_apps`, `wx_chime`, `wx_dns`, `wx_phone`, `wx_tg_text`, `wx_wifi_list`. Register new ones in `tools/validate.sh`.
  - Integration code: `wx_ble`, `wx_net` (Wi-Fi), `wx_portal` (setup page + DNS), `wx_tg` (Telegram), `wx_settings` (NVS), `wx_ui`, `wx_sound`, `main.c`.
- `assets/web/setup.html`: the setup page, embedded into the firmware.
- `components/bsp/`: an unmodified copy of the FoloToy AI Passport board drivers (MIT, keep its `LICENSE`). Use its `bsp_*` APIs and do not edit it unless a driver bug forces it.

## Rules
- Languages: every user-visible string exists in English and Chinese. In firmware use `WX_TR("English", "中文")` (`wx_settings.h`); in `wx_ui.c` use `TR(...)`, which follows `model.lang`; pure modules take a `wx_lang_t` argument. The setup page keeps its strings in the `T` table. English is the default.
- Screen space: the display is 240 px wide with 30 px rounded corners. Check new text against the glyph widths of the generated fonts before relying on it.
- Changing Chinese UI text: rerun `tools/gen_wx_fonts.py`. `tests/test_wx_font_symbols.py` fails if a glyph is missing or the fonts are stale. The 16 px font only has characters from `wx_ui.c`, `main.c` and `wx_apps.c`, so do not show SSIDs or other free text in it.
- Do not join pieces of text with a middle dot; use a comma or separate lines.
- LVGL is not thread-safe. Outside the LVGL task, hold `bsp_lvgl_lock()`.
- Button, BLE, Wi-Fi, Telegram and portal callbacks only enqueue to the app task. Only the app task changes UI state.
- Keys: no key press or long press on its own may destroy anything. Clearing pairings, Wi-Fi or other settings lives in the on-device settings menu (OK key) behind a confirmation page that defaults to Cancel. Long presses do nothing.
- Personal data and secrets: never log notification titles or bodies (only lengths and app IDs), Wi-Fi passwords or the bot token. The setup page only ever gets the token back masked (`wx_tg_token_hint`).
- Reconnecting: the bonded iPhone encrypts the link by itself. Do not send a Security Request at connect time; `wx_ble.c` waits 1.5 s and checks `sec_state.encrypted` first.
- The speaker amp is always powered. Keep the ES8311 asleep (`bsp_audio_sleep`) except while playing.
- RAM is the tight resource (no PSRAM). Free heap is about 88 KB at boot; Wi-Fi takes about 38 KB when it starts, and an HTTPS request needs about 35 KB more for a moment. Check `内存:` and `LVGL 内存` in the boot log after changes. `CONFIG_LV_MEM_SIZE` is in bytes (LVGL 9.5 ignores the old `_KILOBYTES` option).
- Forwarding: a notification the iPhone re-sends on reconnect (`pre_existing`) is never forwarded, Telegram notifications are never forwarded, and Telegram notifications titled with the bot's name are not alerted.

## Verify and flash
- Run `. <esp-idf-v5.5.3>/export.sh && ./tools/validate.sh`. Report host tests, build and on-device results separately.
- Write only `build/ancs-alert.bin` at `0x10000` so the bonds and settings in NVS survive. A full flash from `0x0` needs re-pairing and re-entering Wi-Fi/Telegram; say so when it is unavoidable, for example when the partition table changes.
