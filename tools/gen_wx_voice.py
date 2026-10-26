#!/usr/bin/env python3
"""Generate the voice prompts, one per alerting app and language.

Output: assets/music/wx_voice_<app>_<lang>_16k.pcm for app in wechat, messages,
telegram, qq, whatsapp, signal and line, and lang in en and zh. Each is raw
signed 16-bit little-endian mono PCM at 16 kHz.
Leading/trailing silence is trimmed (30 ms margins, 5 ms fades). Loudness is
normalised to the same RMS (-16 dBFS) so every app's prompt sounds equally loud,
with the peak capped at 28000 (about -1.4 dBFS).

Engines:
  edge (default)  Microsoft Edge read-aloud neural voice via the `edge-tts` CLI
                  (pip install edge-tts). Needs internet, no account. Default
                  voices en-US-JennyNeural and zh-CN-XiaoxiaoNeural. Output terms
                  are unclear for redistribution, so the files are git-ignored.
  say             macOS system voice (Samantha, Tingting); offline but robotic.

Audio is converted with macOS `afconvert` when present, otherwise `ffmpeg`.

Usage (from the project root):
    python3 tools/gen_wx_voice.py [--engine edge|say] [--lang en|zh ...] [--app wechat ...]
        [--voice NAME] [--edge-tts /path/to/edge-tts]
"""

from __future__ import annotations

import argparse
import array
import math
import shutil
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSIC = ROOT / "assets" / "music"
# 与 main/wx_apps.h 的应用一一对应;文件名要和 main/CMakeLists.txt 一致。
PROMPTS = {
    "en": {
        "wechat": "New WeChat message",
        "messages": "New text message",
        "telegram": "New Telegram message",
        "qq": "New QQ message",
        "whatsapp": "New WhatsApp message",
        "signal": "New Signal message",
        "line": "New LINE message",
    },
    "zh": {
        "wechat": "你有新的微信消息",
        "messages": "你有新的信息",
        "telegram": "你有新的 Telegram 消息",
        "qq": "你有新的 QQ 消息",
        "whatsapp": "你有新的 WhatsApp 消息",
        "signal": "你有新的 Signal 消息",
        "line": "你有新的 LINE 消息",
    },
}
APPS = list(PROMPTS["en"])
RATE = 16000
TARGET_RMS = 32768 * 10 ** (-16.0 / 20)
PEAK_CAP = 28000
MARGIN_S = 0.030
FADE_S = 0.005
GATE_DB = -45.0        # 5 ms frames quieter than this (relative to the loudest) count as silence
DEFAULT_VOICE = {
    ("edge", "en"): "en-US-JennyNeural",
    ("edge", "zh"): "zh-CN-XiaoxiaoNeural",
    ("say", "en"): "Samantha",
    ("say", "zh"): "Tingting",
}


def synthesize(engine: str, voice: str, text: str, edge_tts: str, out: Path) -> None:
    if engine == "edge":
        subprocess.run([edge_tts, "--voice", voice, "--text", text, "--write-media", str(out)],
                       check=True)
    else:
        subprocess.run(["say", "-v", voice, "-o", str(out), text], check=True)


def to_16k_mono(src: Path, dst: Path) -> array.array:
    if shutil.which("afconvert"):
        subprocess.run(["afconvert", "-f", "WAVE", "-d", f"LEI16@{RATE}", "-c", "1",
                        "--src-complexity", "bats", str(src), str(dst)], check=True)
    elif shutil.which("ffmpeg"):
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", str(src), "-ac", "1",
                        "-ar", str(RATE), "-sample_fmt", "s16", str(dst)], check=True)
    else:
        raise SystemExit("need afconvert (macOS) or ffmpeg to convert audio")
    with wave.open(str(dst), "rb") as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2 and w.getframerate() == RATE
        samples = array.array("h", w.readframes(w.getnframes()))
    if sys.byteorder != "little":
        samples.byteswap()
    return samples


def trim_normalize(x: array.array) -> array.array:
    frame = int(RATE * 0.005)
    n = len(x) // frame
    rms = [math.sqrt(sum(s * s for s in x[i * frame:(i + 1) * frame]) / frame) for i in range(n)]
    loudest = max(rms) if rms else 0.0
    if loudest <= 0:
        raise SystemExit("voice output is silent")
    threshold = loudest * 10 ** (GATE_DB / 20)
    voiced = [i for i, r in enumerate(rms) if r > threshold]
    margin = int(RATE * MARGIN_S)
    start = max(0, voiced[0] * frame - margin)
    end = min(len(x), (voiced[-1] + 1) * frame + margin)
    y = [float(s) for s in x[start:end]]

    fade = int(RATE * FADE_S)
    if len(y) > 2 * fade:
        for i in range(fade):
            g = i / fade
            y[i] *= g
            y[-1 - i] *= g
    rms_all = math.sqrt(sum(s * s for s in y) / len(y))
    peak = max(abs(s) for s in y)
    gain = min(TARGET_RMS / rms_all, PEAK_CAP / peak)
    return array.array("h", (max(-32768, min(32767, round(s * gain))) for s in y))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--engine", choices=("edge", "say"), default="edge")
    parser.add_argument("--voice", help="voice name (only useful together with a single --lang)")
    parser.add_argument("--lang", action="append", choices=sorted(PROMPTS),
                        help="only these languages (default: all)")
    parser.add_argument("--app", action="append", choices=APPS,
                        help="only these apps (default: all)")
    parser.add_argument("--edge-tts", default=shutil.which("edge-tts") or "edge-tts")
    args = parser.parse_args()

    MUSIC.mkdir(parents=True, exist_ok=True)
    for lang in args.lang or PROMPTS:
        voice = args.voice or DEFAULT_VOICE[(args.engine, lang)]
        for app in args.app or APPS:
            text = PROMPTS[lang][app]
            with tempfile.TemporaryDirectory() as tmp:
                raw = Path(tmp) / ("voice.mp3" if args.engine == "edge" else "voice.aiff")
                synthesize(args.engine, voice, text, args.edge_tts, raw)
                pcm = trim_normalize(to_16k_mono(raw, Path(tmp) / "voice16k.wav"))
            if sys.byteorder != "little":
                pcm.byteswap()
            out = MUSIC / f"wx_voice_{app}_{lang}_16k.pcm"
            out.write_bytes(pcm.tobytes())
            print(f"{out.relative_to(ROOT)}: {len(pcm) / RATE:.2f} s, {args.engine} {voice}, {text}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
