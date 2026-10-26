#!/usr/bin/env python3
"""Glyph-coverage contracts for the alert firmware's generated CJK fonts."""

from __future__ import annotations

import importlib.util
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONTS = ROOT / "assets" / "fonts"

spec = importlib.util.spec_from_file_location("gen_wx_fonts", ROOT / "tools" / "gen_wx_fonts.py")
gen = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(gen)


def symbols(name: str) -> set[str]:
    # 只去掉末尾换行:str.strip() 会把全角空格 U+3000 也当空白去掉。
    return set((FONTS / f"{name}.symbols.txt").read_text(encoding="utf-8").rstrip("\n"))


def generated_symbols(name: str) -> set[str]:
    """Characters lv_font_conv recorded in the generated font's Opts header."""
    head = (FONTS / f"{name}.c").read_text(encoding="utf-8")[:200_000]
    match = re.search(r"--symbols (.*?) --size", head, re.S)
    if not match:
        raise AssertionError(f"{name}.c has no --symbols in its Opts header")
    return set(match.group(1))


class FontSymbolsTest(unittest.TestCase):
    def test_small_font_covers_every_ui_literal(self) -> None:
        missing = set(gen.ui_literal_chars(gen.UI_SOURCES)) - symbols("wx_font_cjk_16")
        self.assertFalse(missing, "rerun tools/gen_wx_fonts.py; missing " +
                         " ".join(f"U+{ord(c):04X}" for c in sorted(missing)))

    def test_large_font_covers_gb2312_and_ui(self) -> None:
        big = symbols("wx_font_cjk_24")
        self.assertTrue(set(gen.gb2312_chars()) <= big)
        self.assertTrue(set(gen.ui_literal_chars(gen.UI_SOURCES)) <= big)
        self.assertIn("张", big)            # 张:常见姓氏,内置子集没有
        self.assertNotIn("龘", big)         # 龘:GB2312 之外,应显示占位框

    def test_generated_fonts_match_symbol_lists(self) -> None:
        for name in ("wx_font_cjk_16", "wx_font_cjk_24"):
            with self.subTest(font=name):
                self.assertEqual(generated_symbols(name), symbols(name),
                                 f"{name}.c is stale; rerun tools/gen_wx_fonts.py")


if __name__ == "__main__":
    unittest.main()
