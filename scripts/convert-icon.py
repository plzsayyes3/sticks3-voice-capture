#!/usr/bin/env python3
"""Convert an image into the raw ARGB8888 (BGRA byte order) .bin format
used by firmware/components/ui_status/assets/.

The UI icons are fixed at 112x112 pixels, LVGL LV_COLOR_FORMAT_ARGB8888,
which in memory is [blue, green, red, alpha] per pixel, row-major, no
padding (see lv_color32_t in LVGL's lv_color.h). This script resizes/pads
any input image to 112x112, converts it to that exact byte layout, and
writes a .bin file. Requires Pillow (pip install Pillow); run inside
scripts/.icon-venv if you don't want to touch your system Python:

    source scripts/.icon-venv/bin/activate
    python3 scripts/convert-icon.py my_icon.png cat_ready

Valid names (must match firmware/components/ui_status/CMakeLists.txt's
EMBED_FILES list): cat_pairing, cat_ready, cat_listening, cat_thinking,
cat_resting, cat_error.
"""
import sys
from pathlib import Path

from PIL import Image

ICON_SIZE = 112
VALID_NAMES = {
    "cat_pairing", "cat_ready", "cat_listening",
    "cat_thinking", "cat_resting", "cat_error",
}
ASSETS_DIR = Path(__file__).resolve().parent.parent / "firmware/components/ui_status/assets"


def convert(src_path: str, name: str) -> Path:
    if name not in VALID_NAMES:
        raise SystemExit(f"unknown icon name {name!r}; must be one of {sorted(VALID_NAMES)}")

    img = Image.open(src_path).convert("RGBA")
    if img.size != (ICON_SIZE, ICON_SIZE):
        img = img.resize((ICON_SIZE, ICON_SIZE), Image.LANCZOS)

    r, g, b, a = img.split()
    bgra = Image.merge("RGBA", (b, g, r, a))
    raw = bgra.tobytes()

    expected = ICON_SIZE * ICON_SIZE * 4
    if len(raw) != expected:
        raise SystemExit(f"unexpected output size {len(raw)}, expected {expected}")

    out_path = ASSETS_DIR / f"{name}_argb8888.bin"
    out_path.write_bytes(raw)
    return out_path


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        raise SystemExit(1)
    result = convert(sys.argv[1], sys.argv[2])
    print(f"wrote {result} ({result.stat().st_size} bytes)")
