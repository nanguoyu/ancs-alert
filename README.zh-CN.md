# ANCS Alert 消息提醒器

[English](README.md)

把一块 ESP32-C3 小屏设备变成放在桌上的 iPhone 消息提醒器。**微信、信息（iMessage 和短信）、
Telegram、QQ、WhatsApp、Signal、LINE** 来新消息时，整屏按应用配色闪烁，显示是谁发来的、
还有几条没看，并用人声播报是哪个应用。其他应用的通知只计数、不提醒。连上 Wi-Fi 后，
还可以把新消息转发到你自己的 Telegram 机器人。

板子界面、设置网页、转发的消息和人声提示都支持**英文和简体中文**。



https://github.com/user-attachments/assets/9c5dc524-2a65-43ed-8f16-cf8d4075ae70

中文界面下试听提示音。

## 硬件

固件针对 **FoloToy AI Passport** 开发板：

- ESP32-C3，8 MB Flash，无 PSRAM
- 240×320 ST7789 屏幕
- 三个按键（上、确定、下）
- ES8311 音频芯片和喇叭
- CW2017 电量计

`components/bsp/` 里的板级驱动来自 FoloToy 的开源代码（MIT）。换用其他 ESP32-C3 板子，
需要按同样的 `bsp_*` 接口提供驱动。

## 工作原理

iPhone 通过 ANCS（Apple Notification Center Service）把通知推给已配对的蓝牙配件，
智能手环收通知用的就是它。设备同时订阅最多 3 台 iPhone 的 ANCS，读取每条新通知的应用标识、
标题和正文，按应用标识决定要不要提醒：

| 应用 | 应用标识 | 提醒配色 |
| --- | --- | --- |
| 微信 | `com.tencent.xin` | 绿 |
| 信息 | `com.apple.MobileSMS` | 浅绿 |
| Telegram | `ph.telegra.Telegraph` | 蓝 |
| QQ | `com.tencent.mqq`、`com.tencent.tim` | 深蓝 |
| WhatsApp | `net.whatsapp.WhatsApp`、`net.whatsapp.WhatsAppSMB` | 青绿 |
| Signal | `org.whispersystems.signal` | 蓝 |
| LINE | `jp.naver.line` | 绿 |

转发时设备直接通过 HTTPS 调用 Telegram Bot API，不需要 iOS App、电脑或中转服务器。

## 使用

### 配对 iPhone

1. 在 iPhone 上打开「设置 → 蓝牙」，点 **Passport Alert XXXX**。名字末尾的 4 位编号每台设备不同，
   等待连接时屏幕上会显示本机的完整名字。
2. 输入设备屏幕上的 6 位配对码，然后允许 iPhone 共享系统通知。
3. 设备显示“已连接 iPhone”就好了。重启后会自动重连。

最多配对 3 台 iPhone。已有手机连着时设备仍在广播，新手机随时可以配对。连着多台时，
待机页每台手机占一行，提醒页会标出消息来自哪台手机。

### 按键

| 按键 | 作用 |
| --- | --- |
| 提醒中按任意键 | 清除提醒 |
| 确定 | 打开设置菜单 |
| 上 | 提示音开/关（断电后记住） |
| 下 | 试听提示音，每按一次换一个应用 |
| Wi-Fi 设置中按任意键 | 退出 Wi-Fi 设置 |

按键都只用单击。长按没有作用，按错键也不会清掉什么。

### 设置菜单

上下键选择，确定键进入：

| 菜单项 | 作用 |
| --- | --- |
| 提示音 | 同上键 |
| 试听提示音 | 同下键 |
| Wi-Fi 与转发设置 | 打开设置热点和网页（见下文） |
| English / 中文 | 切换语言 |
| 清除 Wi-Fi 网络 | 删除全部已保存的网络（Telegram 设置保留） |
| 清除 iPhone 配对 | 删除全部配对，所有 iPhone 都要重新配对 |
| 返回 | 关闭菜单 |

两个“清除”都会弹出确认页。确认页默认停在“取消”，要先按上下键移到“确定清除”再按确定才会执行。
菜单 30 秒没操作会自动关闭，来新消息时也会关闭。

在 iPhone 上读掉新消息后，通知会从通知中心消失，设备上的提醒也会自动结束。
几个应用同时有新消息时，提醒页按最近一条的应用配色，下方小字分别列出各应用的条数。

## Wi-Fi 与 Telegram 转发

1. 按**确定键**打开设置菜单，选 **Wi-Fi 与转发设置**。屏幕显示一个二维码、热点名
   （如 `Passport-1A2B`，编号与蓝牙名相同）和随机密码，每次都不同。
2. 用 iPhone 相机扫码加入热点。手机会自动弹出设置网页；没弹出就用浏览器打开 `http://192.168.4.1`。
3. **语言**：在网页顶部切换 English 和中文，设备的语言也会一起改。
4. **Wi-Fi**：点“扫描附近的网络”，选一个，填密码，点“保存并连接”。最多保存 5 个网络；
   同名的网络再保存一次就是改密码。设备开机后自动连信号最强的已保存网络，断了会自动重连。
5. **Telegram**：
   1. 在 Telegram 里找 @BotFather，发送 `/newbot` 创建机器人，复制它给的 Token；
   2. 粘贴到网页上，点“验证并保存”（设备要先连上 Wi-Fi）；
   3. 给你的机器人发一句话（比如 `/start`），再点“绑定聊天”。机器人会回一条确认消息，
      之后也可以随时点“发送测试消息”试一下。
6. 选择要转发的应用、是否转发正文，以及设备要提醒哪些应用。设置随改随存。
7. 点“完成”或按设备任意键退出设置。设置热点 15 分钟后也会自动关闭。

转发的消息长这样（连着多台手机时会标出是哪台）：

```
【微信，XR】张三
今晚吃什么
```

### 直连不了 Telegram 时

在连不上 `api.telegram.org` 的地方，可以在网页的“手动填聊天 ID、修改 API 地址”里填自己的反向代理，
路径与官方相同（`<地址>/bot<Token>/sendMessage`）。例如一个最简单的 Cloudflare Worker：

```js
export default {
  fetch(request) {
    const url = new URL(request.url);
    url.hostname = "api.telegram.org";
    return fetch(new Request(url, request));
  },
};
```

把 Worker 绑定到自己的域名（`workers.dev` 在国内通常也连不上），然后在网页里填 `https://你的域名`。

## 已知限制

- **通知不显示预览时，iPhone 不会转给设备。** 某个应用的「设置 → 通知 → 显示预览」不是“始终”时，
  iOS 通过 ANCS 一条都不发。Apple Watch 不受这个设置影响，所以手表能收到、设备收不到时，先检查这里。
- 第一次配对的新手机，要在弹窗里允许“共享系统通知”。设备会在配对后自动重新订阅几次，
  弥补点“允许”之前那段时间漏掉的通知。
- 应用开在前台时不会产生系统通知，设备也收不到。
- 设备重连手机时，手机会把通知中心里已有的通知再发一遍：这些只计数，不提醒也不转发。
- 转发的正文最多约 240 字节，这是 iOS 通过 ANCS 给的上限。Telegram 的消息不转发；
  你的机器人发来的通知不会在设备上再提醒一次。
- 连着 Wi-Fi 比只用蓝牙更耗电。没有保存的网络时，Wi-Fi 不会打开。

## 编译与刷入

需要 [ESP-IDF v5.5.3](https://docs.espressif.com/projects/esp-idf/zh_CN/v5.5.3/esp32c3/get-started/)，
并装好 ESP32-C3 的工具链。

### 1. 生成人声提示（可选）

人声提示不放在仓库里，因为语音服务对生成结果的再分发没有明确条款。没有这些文件时，提醒只播合成的“叮咚”。

```bash
pip install edge-tts
python3 tools/gen_wx_voice.py
```

脚本会在 `assets/music/` 写出 14 个文件（7 个应用，2 种语言）。需要联网，还需要 `afconvert`（macOS）
或 `ffmpeg`。加 `--lang zh` 或 `--app whatsapp` 只生成一部分，加 `--engine say` 改用 macOS 自带的离线语音。

### 2. 编译

```bash
. <ESP-IDF 路径>/esp-idf-v5.5.3/export.sh
./tools/validate.sh
```

这会跑主机单元测试、编译固件，并生成 `build/ancs-alert.bin`（应用镜像）和
`build/ancs-alert-full.bin`（引导程序、分区表和应用）。

### 3. 刷入

用 USB 连上板子，找到串口（macOS 上是 `/dev/cu.usbmodem*`，Linux 上是 `/dev/ttyACM*`）。
第一次刷之前，可以先备份原来的固件：

```bash
python -m esptool --chip esp32c3 -p 串口 -b 921600 read_flash 0 0x800000 original-firmware.bin
```

第一次安装会擦掉整片 Flash：

```bash
python -m esptool --chip esp32c3 -p 串口 erase_flash
python -m esptool --chip esp32c3 -p 串口 -b 921600 write_flash 0x0 build/ancs-alert-full.bin
```

之后更新只需要写应用镜像，配对和设置都会保留：

```bash
python -m esptool --chip esp32c3 -p 串口 -b 921600 write_flash 0x10000 build/ancs-alert.bin
```

再次写入完整镜像会清空配对和设置：要在每台 iPhone 上忽略设备后重新配对，并重新设置 Wi-Fi 和 Telegram。

### 修改界面文字

中文用的是由[霞鹜文楷](https://github.com/lxgw/LxgwWenKai)生成的位图字库。改了 `main/wx_ui.c`、
`main/main.c` 或 `main/wx_apps.c` 里的中文后，要重新生成字库（需要 Node.js）：

```bash
python3 tools/gen_wx_fonts.py --font /path/to/LXGWWenKai-Regular.ttf
```

字库缺字时 `tests/test_wx_font_symbols.py` 会失败。英文用的是可打印 ASCII，字库里总是有。
屏幕宽 240 px 且四角是圆的，新文字要确认放得下。

## 目录

| 路径 | 内容 |
| --- | --- |
| `main/` | 应用：蓝牙与 ANCS（`wx_ble`、`wx_ancs_proto`）、应用对照表（`wx_apps`）、语言（`wx_lang`）、手机型号（`wx_phone`）、提醒状态（`wx_alert`）、界面（`wx_ui`）、提示音（`wx_sound`、`wx_chime`）、设置（`wx_settings`、`wx_wifi_list`）、Wi-Fi（`wx_net`）、设置网页与 DNS（`wx_portal`、`wx_dns`）、Telegram（`wx_tg`、`wx_tg_text`） |
| `assets/web/` | 设置网页（编译时嵌入固件） |
| `assets/fonts/` | 由 `tools/gen_wx_fonts.py` 生成的位图字库。24 px 覆盖 GB2312 全部汉字，16 px 只含界面用到的字 |
| `assets/music/` | 由 `tools/gen_wx_voice.py` 生成的人声提示（不入库） |
| `components/bsp/` | FoloToy AI Passport 的板级驱动，原样使用 |
| `tools/` | 字库与人声生成脚本、编译检查 |
| `tests/` | 主机单元测试 |

## 许可

固件以 [MIT 许可](LICENSE)发布。`components/bsp/` 里的板级驱动由 FoloToy 以 MIT 许可发布
（见 `components/bsp/LICENSE`）。`assets/fonts/` 里的字库由霞鹜文楷生成，按 SIL Open Font License 1.1
授权（见 `assets/fonts/OFL.txt`）。
