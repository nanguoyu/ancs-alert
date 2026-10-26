# ANCS Alert

[中文说明](README.zh-CN.md)

Firmware that turns a small ESP32-C3 gadget into a desk-side message alert for your iPhone.
When a new message arrives in **WeChat, Messages (iMessage and SMS), Telegram, QQ, WhatsApp,
Signal or LINE**, the whole screen flashes in the app's colour, shows who sent it and how many
messages are waiting, and a voice says which app it came from. Other apps are counted but do not
alert. With Wi-Fi set up, new messages can also be forwarded to your own Telegram bot.

The interface, the setup page, forwarded messages and the voice prompts are available in
**English and Simplified Chinese**.



https://github.com/user-attachments/assets/2c8bdd9c-9177-4dc3-9fe9-2b273651dbca

Test alerts with the Chinese interface.

## Hardware

The firmware targets the **FoloToy AI Passport** board:

- ESP32-C3 with 8 MB flash, no PSRAM
- 240×320 ST7789 display
- three buttons (Up, OK, Down)
- ES8311 audio codec with a speaker
- CW2017 battery gauge

The board drivers in `components/bsp/` come from FoloToy's open-source code (MIT). Other ESP32-C3
boards need their own drivers behind the same `bsp_*` functions.

## How it works

iPhones share their notifications with paired Bluetooth accessories through the Apple
Notification Center Service (ANCS), the same way fitness bands receive them. The device subscribes
to ANCS on up to three iPhones at once, reads each new notification's app identifier, title and
text, and decides from the app identifier whether to alert:

| App | App identifier | Alert colour |
| --- | --- | --- |
| WeChat | `com.tencent.xin` | green |
| Messages | `com.apple.MobileSMS` | light green |
| Telegram | `ph.telegra.Telegraph` | blue |
| QQ | `com.tencent.mqq`, `com.tencent.tim` | dark blue |
| WhatsApp | `net.whatsapp.WhatsApp`, `net.whatsapp.WhatsAppSMB` | teal |
| Signal | `org.whispersystems.signal` | blue |
| LINE | `jp.naver.line` | green |

Forwarding calls the Telegram Bot API over HTTPS directly from the device. No iOS app, computer or
relay server is involved.

## Using it

### Pairing an iPhone

1. On the iPhone, open **Settings → Bluetooth** and tap **Passport Alert XXXX**. The four
   characters at the end are different on every device; the screen shows the full name while it
   waits for a phone.
2. Enter the six-digit code shown on the device, then allow the iPhone to share system
   notifications.
3. The device shows "iPhone connected". It reconnects by itself after a restart.

Up to three iPhones can be paired. The device keeps advertising while phones are connected, so
another phone can pair at any time. With several phones connected, the home screen shows one line
per phone, and the alert screen says which phone a message came from.

### Buttons

| Button | Action |
| --- | --- |
| Any button during an alert | Clear the alert |
| OK | Open the settings menu |
| Up | Sound on or off (remembered) |
| Down | Play a test alert; each press tries the next app |
| Any button in Wi-Fi setup | Leave Wi-Fi setup |

Only single presses do anything. Long presses do nothing, so a wrong button never erases anything.

### Settings menu

Use Up and Down to choose and OK to open:

| Item | Action |
| --- | --- |
| Sound | Same as the Up button |
| Test sound | Same as the Down button |
| Wi-Fi setup | Opens the setup hotspot and web page (see below) |
| English / 中文 | Switches the language |
| Clear Wi-Fi | Deletes all saved networks (Telegram settings are kept) |
| Forget iPhones | Deletes all pairings; every iPhone has to pair again |
| Back | Closes the menu |

Both clearing items ask for confirmation. The confirmation page starts on "Cancel"; move to
"Clear" with Up or Down and press OK to go ahead. The menu closes after 30 seconds without a
button press, or when a new message arrives.

When you read a message on the iPhone, its notification leaves Notification Center and the alert on
the device ends by itself. If several apps have new messages, the alert uses the colour of the most
recent one and lists the count per app.

## Wi-Fi and Telegram forwarding

1. Press **OK** to open the menu and choose **Wi-Fi setup**. The screen shows a QR code, the
   hotspot name (such as `Passport-1A2B`, ending with the same characters as the Bluetooth name) and
   a random password that changes every time.
2. Scan the QR code with the iPhone camera to join the hotspot. The setup page opens by itself; if
   it does not, open `http://192.168.4.1` in a browser.
3. **Language**: switch between English and 中文 at the top of the page. This also changes the
   device.
4. **Wi-Fi**: tap "Scan for networks", pick one, enter its password and tap "Save and connect". Up
   to five networks are saved; saving a network again changes its password. The device joins the
   strongest saved network at start-up and reconnects when the connection drops.
5. **Telegram**:
   1. In Telegram, open @BotFather, send `/newbot` and copy the token it gives you.
   2. Paste the token into the page and tap "Verify and save" (the device must be on Wi-Fi).
   3. Send your bot any message (for example `/start`), then tap "Link chat". The bot replies with
      a confirmation. "Send a test message" checks it again at any time.
6. Choose which apps to forward, whether to include the message text, and which apps alert on the
   device. Changes are saved immediately.
7. Tap "Done" or press any button on the device to leave setup. The hotspot also closes after 15
   minutes.

A forwarded message looks like this (the phone is named when several are connected):

```
[WhatsApp, XR] Alice
See you at 7
```

### When Telegram is blocked

Where `api.telegram.org` cannot be reached, enter your own reverse proxy under "Enter a chat ID or
change the API address". It must use the same paths as the official API
(`<address>/bot<token>/sendMessage`). A minimal Cloudflare Worker:

```js
export default {
  fetch(request) {
    const url = new URL(request.url);
    url.hostname = "api.telegram.org";
    return fetch(new Request(url, request));
  },
};
```

Put the Worker on your own domain (`workers.dev` is often blocked as well) and enter
`https://your-domain` on the page.

## Known limitations

- **The iPhone only sends notifications that show previews.** If an app's
  **Settings → Notifications → Show Previews** is not set to "Always", iOS sends nothing for it over
  ANCS. Apple Watch is not affected by this setting, so if the watch gets a message and the device
  does not, check this first.
- A newly paired iPhone has to allow "Share System Notifications" in the pop-up. The device
  re-subscribes a few times after pairing to catch messages that arrived before you tapped Allow.
- Apps open in the foreground do not create notifications, so the device does not see them.
- When the device reconnects, the iPhone re-sends the notifications already in Notification Center.
  Those are counted but do not alert and are not forwarded.
- Forwarded text is cut at about 240 bytes, the most iOS sends over ANCS. Telegram messages are not
  forwarded, and notifications from your own bot do not alert.
- Wi-Fi uses more battery than Bluetooth alone. Without saved networks, Wi-Fi stays off.

## Building and flashing

You need [ESP-IDF v5.5.3](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/get-started/)
with the ESP32-C3 tools installed.

### 1. Generate the voice prompts (optional)

The voice prompts are not in the repository because the text-to-speech service gives no clear terms
for redistributing its output. Without them, each alert plays a two-tone chime only.

```bash
pip install edge-tts
python3 tools/gen_wx_voice.py
```

This writes 14 files to `assets/music/` (seven apps, two languages). It needs internet access, plus
`afconvert` (macOS) or `ffmpeg`. Add `--lang en` or `--app whatsapp` to generate only some of them,
or `--engine say` to use the offline macOS voices.

### 2. Build

```bash
. <path-to>/esp-idf-v5.5.3/export.sh
./tools/validate.sh
```

This runs the host unit tests, builds the firmware and writes `build/ancs-alert.bin` (app only) and
`build/ancs-alert-full.bin` (bootloader, partition table and app).

### 3. Flash

Connect the board over USB and find its port (`/dev/cu.usbmodem*` on macOS, `/dev/ttyACM*` on
Linux). Before the first install, you may want to keep a copy of the original firmware:

```bash
python -m esptool --chip esp32c3 -p PORT -b 921600 read_flash 0 0x800000 original-firmware.bin
```

First install, which erases the board:

```bash
python -m esptool --chip esp32c3 -p PORT erase_flash
python -m esptool --chip esp32c3 -p PORT -b 921600 write_flash 0x0 build/ancs-alert-full.bin
```

Later updates only need the app image, which keeps pairings and settings:

```bash
python -m esptool --chip esp32c3 -p PORT -b 921600 write_flash 0x10000 build/ancs-alert.bin
```

Writing the full image again clears pairings and settings: forget the device on each iPhone, pair
again and redo Wi-Fi and Telegram setup.

### Changing interface text

The Chinese text uses bitmap fonts generated from
[LXGW WenKai](https://github.com/lxgw/LxgwWenKai). After changing Chinese text in `main/wx_ui.c`,
`main/main.c` or `main/wx_apps.c`, regenerate the fonts (needs Node.js):

```bash
python3 tools/gen_wx_fonts.py --font /path/to/LXGWWenKai-Regular.ttf
```

`tests/test_wx_font_symbols.py` fails when a character is missing from the fonts. English text uses
the printable ASCII range, which is always included. The screen is 240 px wide with rounded corners,
so check that new text fits.

## Project layout

| Path | Contents |
| --- | --- |
| `main/` | The application: Bluetooth and ANCS (`wx_ble`, `wx_ancs_proto`), app table (`wx_apps`), language (`wx_lang`), iPhone models (`wx_phone`), alert state (`wx_alert`), screen (`wx_ui`), sound (`wx_sound`, `wx_chime`), settings (`wx_settings`, `wx_wifi_list`), Wi-Fi (`wx_net`), setup page and DNS (`wx_portal`, `wx_dns`), Telegram (`wx_tg`, `wx_tg_text`) |
| `assets/web/` | The setup page, embedded into the firmware |
| `assets/fonts/` | Bitmap fonts generated by `tools/gen_wx_fonts.py`. The 24 px font covers all GB2312 characters; the 16 px font only has the characters the interface uses |
| `assets/music/` | Voice prompts generated by `tools/gen_wx_voice.py` (not in the repository) |
| `components/bsp/` | Board drivers from FoloToy's AI Passport code, used unchanged |
| `tools/` | Font and voice generators, build checks |
| `tests/` | Host unit tests |

## Licence

The firmware is released under the [MIT licence](LICENSE). The board drivers in `components/bsp/`
are MIT-licensed by FoloToy (see `components/bsp/LICENSE`). The fonts in `assets/fonts/` are
derived from LXGW WenKai and are licensed under the SIL Open Font License 1.1
(see `assets/fonts/OFL.txt`).
