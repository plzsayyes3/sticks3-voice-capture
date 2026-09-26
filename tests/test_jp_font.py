import importlib.util
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
UI_DIR = ROOT / "firmware/components/ui_status"
FONT_FILES = ("ui_font_jp_12.c", "ui_font_jp_16.c")


def load_font_script():
    spec = importlib.util.spec_from_file_location(
        "prepare_jp_font", ROOT / "scripts/prepare-jp-font.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def font_glyphs(path: Path) -> set:
    return {chr(int(code, 16))
            for code in re.findall(r"/\* U\+([0-9A-F]+) ", path.read_text(encoding="utf-8"))}


class JapaneseFontTests(unittest.TestCase):
    def test_every_on_screen_character_has_a_glyph(self):
        needed = set(load_font_script().screen_symbols())
        self.assertIn("ま", needed)  # sanity: the scan found the Japanese strings
        for name in FONT_FILES:
            with self.subTest(font=name):
                missing = needed - font_glyphs(UI_DIR / "fonts" / name)
                self.assertFalse(
                    missing,
                    f"{name} lacks {''.join(sorted(missing))}; "
                    "run scripts/prepare-jp-font.py")

    def test_fonts_are_built_and_licensed(self):
        cmake = (UI_DIR / "CMakeLists.txt").read_text()
        for name in FONT_FILES:
            self.assertIn(f'"fonts/{name}"', cmake)
            text = (UI_DIR / "fonts" / name).read_text(encoding="utf-8")
            self.assertNotIn("/Users/", text)
            self.assertNotIn("/private/", text)
        self.assertIn("SIL Open Font License",
                      (UI_DIR / "fonts/OFL.txt").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
