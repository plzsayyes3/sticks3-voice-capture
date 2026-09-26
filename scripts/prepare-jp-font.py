#!/usr/bin/env python3
"""Generate the Japanese LVGL fonts for the device screen.

Only the characters the firmware actually shows are converted: every
non-ASCII character inside a string literal in SOURCES, plus printable
ASCII. Rerun this after adding or changing on-screen Japanese text;
tests/test_jp_font.py fails when a string uses a character the fonts lack.

The M PLUS Rounded 1c TTFs (SIL OFL 1.1) are downloaded from a pinned
google/fonts commit into scripts/.fonts/ (gitignored) and checked by
SHA-256. Requires Node.js (npx runs lv_font_conv).
"""
import hashlib
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = (
    ROOT / "firmware/main/main.c",
    ROOT / "firmware/components/ui_status/ui_status.c",
)
OUT_DIR = ROOT / "firmware/components/ui_status/fonts"
CACHE_DIR = Path(__file__).resolve().parent / ".fonts"

FONTS_COMMIT = "7d1c962b6fbc8987b59ed229bbe5c12764d3a624"
FONT_URL = ("https://raw.githubusercontent.com/google/fonts/"
            f"{FONTS_COMMIT}/ofl/mplusrounded1c/{{name}}")
TTF_SHA256 = {
    "MPLUSRounded1c-Medium.ttf":
        "adfde1b6bae58719c4e0144612a94232e72fc5ca655c4722165fe88d06521a70",
    "MPLUSRounded1c-Bold.ttf":
        "c358630584e8e2d8fbd6121d0f4693255ffef6d1e6d4f3441fd6e5a963a11f9e",
}

# (lvgl symbol, pixel size, ttf, montserrat fallback for anything missing)
FONTS = (
    ("ui_font_jp_12", 12, "MPLUSRounded1c-Medium.ttf", "lv_font_montserrat_12"),
    ("ui_font_jp_16", 16, "MPLUSRounded1c-Bold.ttf", "lv_font_montserrat_16"),
)
LV_FONT_CONV = "lv_font_conv@1.5.3"

STRING_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def screen_symbols(sources=SOURCES) -> str:
    chars = set()
    for source in sources:
        for literal in STRING_LITERAL.findall(source.read_text(encoding="utf-8")):
            chars.update(ch for ch in literal if ord(ch) > 0x7E)
    return "".join(sorted(chars))


def fetch_ttf(name: str) -> Path:
    CACHE_DIR.mkdir(exist_ok=True)
    path = CACHE_DIR / name
    if not path.exists():
        urllib.request.urlretrieve(FONT_URL.format(name=name), path)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != TTF_SHA256[name]:
        path.unlink()
        raise SystemExit(f"{name}: unexpected SHA-256 {digest}")
    return path


def generate(symbol: str, size: int, ttf: Path, fallback: str, chars: str) -> Path:
    output = OUT_DIR / f"{symbol}.c"
    subprocess.run(
        ["npx", "--yes", LV_FONT_CONV,
         "--font", str(ttf), "--size", str(size), "--bpp", "4",
         "--format", "lvgl", "--no-compress",
         "--lv-font-name", symbol, "--lv-fallback", fallback,
         "-r", "0x20-0x7E", "--symbols", chars,
         "-o", str(output)],
        check=True,
    )
    # lv_font_conv's include guard assumes an lvgl/ include root; the ESP-IDF
    # component exposes plain "lvgl.h". Also drop the absolute local paths it
    # records in the header comment.
    text = output.read_text(encoding="utf-8")
    text = text.replace('#include "lvgl/lvgl.h"', '#include "lvgl.h"')
    text = text.replace(str(ttf), ttf.name)
    text = text.replace(str(output), str(output.relative_to(ROOT)))
    output.write_text(text, encoding="utf-8")
    return output


def main() -> None:
    chars = screen_symbols()
    OUT_DIR.mkdir(exist_ok=True)
    for symbol, size, name, fallback in FONTS:
        output = generate(symbol, size, fetch_ttf(name), fallback, chars)
        print(f"wrote {output.relative_to(ROOT)} ({len(chars)} non-ASCII glyphs)")


if __name__ == "__main__":
    sys.exit(main())
