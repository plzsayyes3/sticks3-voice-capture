import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ASSET_DIR = ROOT / "firmware/components/ui_status/assets"
ICON_NAMES = (
    "rady_pairing",
    "rady_ready",
    "rady_listening",
    "rady_thinking",
    "rady_resting",
    "rady_error",
)
ICON_BYTES = 112 * 112 * 4


def average_visible_channel(data: bytes, channel: int) -> float:
    pixels = [data[offset + channel] for offset in range(0, len(data), 4)
              if data[offset + 3] >= 224]
    if not pixels:
        return 0.0
    return sum(pixels) / len(pixels)


class RadyAssetTests(unittest.TestCase):
    def test_all_status_assets_are_embedded_argb8888_icons(self):
        for name in ICON_NAMES:
            with self.subTest(name=name):
                path = ASSET_DIR / f"{name}_argb8888.bin"
                self.assertTrue(path.is_file(), f"missing {path.name}")
                self.assertEqual(path.stat().st_size, ICON_BYTES)

        cmake = (ASSET_DIR.parent / "CMakeLists.txt").read_text()
        for name in ICON_NAMES:
            self.assertIn(f'"assets/{name}_argb8888.bin"', cmake)

    def test_ready_icon_uses_the_orange_row(self):
        data = (ASSET_DIR / "rady_ready_argb8888.bin").read_bytes()
        red = average_visible_channel(data, 2)
        green = average_visible_channel(data, 1)
        blue = average_visible_channel(data, 0)
        self.assertGreater(red, green)
        self.assertGreater(green, blue)

    def test_recording_icon_uses_the_green_row(self):
        data = (ASSET_DIR / "rady_listening_argb8888.bin").read_bytes()
        red = average_visible_channel(data, 2)
        green = average_visible_channel(data, 1)
        blue = average_visible_channel(data, 0)
        self.assertGreater(green, red)
        self.assertGreater(green, blue)

    def test_ready_and_recording_scenes_reference_rady_assets(self):
        source = (ASSET_DIR.parent / "ui_status_icons.c").read_text()
        self.assertIn("_binary_rady_ready_argb8888_bin_start", source)
        self.assertIn("_binary_rady_listening_argb8888_bin_start", source)
        self.assertIn("case UI_STATUS_ICON_IDLE:\n        return &s_rady_ready;", source)
        self.assertIn("case UI_STATUS_ICON_RECORDING:\n        return &s_rady_listening;", source)


if __name__ == "__main__":
    unittest.main()
