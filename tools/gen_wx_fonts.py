#!/usr/bin/env python3
"""Generate the alert firmware's CJK LVGL fonts with lv_font_conv.

wx_font_cjk_24: printable ASCII, GB2312 row 1/3 punctuation and all 6763 GB2312
hanzi, so friend/group names in that range render. Characters outside it show
LVGL's placeholder box.
wx_font_cjk_16: printable ASCII plus only the non-ASCII characters that appear in
string literals of the app UI sources.

Usage (from the project root; needs Node.js for npx):
    python3 tools/gen_wx_fonts.py --font /path/to/LXGWWenKai-Regular.ttf
    python3 tools/gen_wx_fonts.py --symbols-only   # refresh symbol lists only

The symbol lists are written next to the fonts so tests can check coverage
without Node.js.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONT_DIR = ROOT / "assets" / "fonts"
UI_SOURCES = [ROOT / "main" / "wx_ui.c", ROOT / "main" / "main.c", ROOT / "main" / "wx_apps.c"]
LV_FONT_CONV = "lv_font_conv@1.5.3"
STRING_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def gb2312_chars() -> list[str]:
    chars: list[str] = []
    rows = [0xA1, 0xA3] + list(range(0xB0, 0xF8))   # 标点、全角 ASCII、一二级汉字
    for hi in rows:
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            chars.append(ch)
    return chars


def ui_literal_chars(paths: list[Path]) -> list[str]:
    """Non-ASCII characters of string literals, skipping log calls and comments."""
    seen: dict[str, None] = {}
    for path in paths:
        in_log = False
        for line in path.read_text(encoding="utf-8").splitlines():
            code = line.split("//", 1)[0]
            if "ESP_LOG" in code:
                in_log = True
            if not in_log:
                for literal in STRING_LITERAL.findall(code):
                    for ch in literal:
                        if ord(ch) > 0x7E:
                            seen[ch] = None
            if in_log and code.rstrip().endswith(";"):
                in_log = False
    return sorted(seen)


def write_symbols(name: str, chars: list[str]) -> Path:
    path = FONT_DIR / f"{name}.symbols.txt"
    path.write_text("".join(chars) + "\n", encoding="utf-8")
    return path


def convert(font: Path, name: str, size: int, chars: list[str]) -> None:
    out = FONT_DIR / f"{name}.c"
    # 用相对仓库根目录的路径:lv_font_conv 会把参数写进生成文件头,避免留下本机用户目录。
    cmd = [
        "npx", "--yes", LV_FONT_CONV,
        "--font", os.path.relpath(font.resolve(), ROOT),
        "-r", "0x20-0x7E",
        "--symbols", "".join(chars),
        "--size", str(size), "--bpp", "4", "--format", "lvgl", "--no-compress",
        "--lv-font-name", name, "--lv-include", "lvgl.h",
        "-o", str(out.relative_to(ROOT)),
    ]
    subprocess.run(cmd, check=True, cwd=ROOT)
    print(f"{out.relative_to(ROOT)}: {len(chars)} non-ASCII symbols")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path, help="TTF/OTF covering GB2312 (LXGW WenKai Regular)")
    parser.add_argument("--symbols-only", action="store_true")
    args = parser.parse_args()

    FONT_DIR.mkdir(parents=True, exist_ok=True)
    ui = ui_literal_chars(UI_SOURCES)
    big = list(dict.fromkeys(gb2312_chars() + ui))
    write_symbols("wx_font_cjk_24", big)
    write_symbols("wx_font_cjk_16", ui)
    if args.symbols_only:
        return 0
    if not args.font or not args.font.is_file():
        print("--font is required unless --symbols-only", file=sys.stderr)
        return 2
    convert(args.font, "wx_font_cjk_24", 24, big)
    convert(args.font, "wx_font_cjk_16", 16, ui)
    return 0


if __name__ == "__main__":
    sys.exit(main())
