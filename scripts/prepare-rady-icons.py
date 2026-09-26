#!/usr/bin/env python3
"""Extract the selected Rady poses from the supplied transparent sprite sheet.

The source image is kept in firmware/components/ui_status/assets so the
remaining color rows (pink, sky blue, etc.) can be used for later UI states.
The generated files are raw BGRA bytes for LVGL ARGB8888 on this target.
Requires Pillow.
"""
from pathlib import Path
import sys

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
ASSET_DIR = ROOT / "firmware/components/ui_status/assets"
SOURCE = ASSET_DIR / "rady_sprite_sheet_source.png"
ICON_SIZE = 112

# Pixel bounds in the 1774x887 source sheet. The chosen color rows are orange
# for standby/supporting scenes, green for recording to internal storage and
# pink for recording to the SD card.
POSES = {
    "rady_pairing": (158, 705, 337, 886),  # orange, sparkle
    "rady_ready": (514, 705, 682, 886),  # orange, neutral
    "rady_listening": (696, 188, 910, 372),  # green, music notes
    "rady_listening_sd": (696, 0, 910, 187),  # pink, music notes
    "rady_thinking": (1524, 705, 1713, 886),  # orange, thought bubble
    "rady_resting": (1110, 705, 1345, 886),  # orange, sleeping
    "rady_error": (1346, 705, 1526, 886),  # orange, puzzled
}


def render_icon(sheet: Image.Image, bounds: tuple[int, int, int, int]) -> bytes:
    pose = sheet.crop(bounds).convert("RGBA")
    alpha_bounds = pose.getchannel("A").getbbox()
    if alpha_bounds is None:
        raise ValueError(f"sprite crop {bounds} is fully transparent")
    pose = pose.crop(alpha_bounds)

    scale = min(ICON_SIZE / pose.width, ICON_SIZE / pose.height)
    size = (max(1, round(pose.width * scale)), max(1, round(pose.height * scale)))
    pose = pose.resize(size, Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (ICON_SIZE, ICON_SIZE), (0, 0, 0, 0))
    canvas.paste(pose, ((ICON_SIZE - size[0]) // 2, (ICON_SIZE - size[1]) // 2), pose)

    red, green, blue, alpha = canvas.split()
    return Image.merge("RGBA", (blue, green, red, alpha)).tobytes()


def prepare_icons(source: Path = SOURCE) -> list[Path]:
    sheet = Image.open(source).convert("RGBA")
    if sheet.size != (1774, 887):
        raise ValueError(f"unexpected Rady sprite sheet dimensions: {sheet.size}")

    outputs = []
    for name, bounds in POSES.items():
        data = render_icon(sheet, bounds)
        if len(data) != ICON_SIZE * ICON_SIZE * 4:
            raise ValueError(f"unexpected output size for {name}: {len(data)}")
        output = ASSET_DIR / f"{name}_argb8888.bin"
        output.write_bytes(data)
        outputs.append(output)
    return outputs


if __name__ == "__main__":
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else SOURCE
    for output in prepare_icons(source):
        print(f"wrote {output} ({output.stat().st_size} bytes)")
