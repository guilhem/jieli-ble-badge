"""Check the badge fonts with the patched SDK's LVGL decoder (set BADGE_SDK)."""

import hashlib
import itertools
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

MEDIA = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get("BADGE_SDK", MEDIA.parents[1] / "tools/community-re/jieli-sdks/e_badge_707_sdk_200"))
LVGL = SDK / "SDK/apps/common/ui/lvgl_v8"

# Golden metrics and pixels from plain DejaVu at 258a3e9 and SDK Montserrat at d0167685.
FONTS = {
    "Sans": {
        16: "bcdc8a7e65d3705f221eca85265619b2eccfa931120382a44457143f784eb10b",
        20: "cfc98209f2ef6c9e5913c9406edf737d41ea035dadcfe9da83210077ca00a7bf",
        32: "7526d0436a440981e6b6671ea00b3d1313516b514f15fa1cded94c7e1ca14f7c",
    },
    "Serif": {
        16: "cc4066cbbc63a3b06fe59d7989ad474dfbd14968d6be5263b5ba263d5e620c17",
        20: "891fc7d607fcc4aad2b29c3be1ac9a83f900e68d8072c95dcfbca3c8b504af18",
        32: "bdaf34642ab521d854c43005cdaa74ac190c6a5ff6158ca2ac11e48fbf47f387",
    },
    "Montserrat": {14: "920d5e2ab726712bb44169f30be105f2bc7d314ccc9b54c367cd0f7578eb6022"},
}

# Degree, bullet and Font Awesome symbols shipped with the SDK menu font.
MENU_GLYPHS = [
    *range(32, 127),
    0x00B0, 0x2022, 0xF001, 0xF008, 0xF00B, 0xF00C, 0xF00D, 0xF011, 0xF013, 0xF015,
    0xF019, 0xF01C, 0xF021, 0xF026, 0xF027, 0xF028, 0xF03E, 0xF043, 0xF048, 0xF04B,
    0xF04C, 0xF04D, 0xF051, 0xF052, 0xF053, 0xF054, 0xF067, 0xF068, 0xF06E, 0xF070,
    0xF071, 0xF074, 0xF077, 0xF078, 0xF079, 0xF07B, 0xF093, 0xF095, 0xF0C4, 0xF0C5,
    0xF0C7, 0xF0C9, 0xF0E0, 0xF0E7, 0xF0EA, 0xF0F3, 0xF11C, 0xF124, 0xF15B, 0xF1EB,
    0xF240, 0xF241, 0xF242, 0xF243, 0xF244, 0xF287, 0xF293, 0xF2ED, 0xF304, 0xF55A,
    0xF7C2, 0xF8A2,
]


class FontTest(unittest.TestCase):
    def test_decoded_glyphs(self):
        codes = [*range(32, 127), *range(160, 256), *map(ord, "ŒœŸ–—‘’“”…\u202f€")]
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            # The SDK includes this target header even with GPU list drawing disabled.
            (work / "gpu_port.h").touch()
            program = work / "font_glyphs"
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-O2", "-DCONFIG_BADGE_MENU",
                "-DCONFIG_BADGE_BOARD_E87", "-I", str(work), "-I", str(MEDIA), "-I", str(LVGL),
                str(MEDIA / "tests/font_glyphs.c"), str(MEDIA / "badge_media_svg_font.c"),
                str(LVGL / "src/font/lv_font_fmt_txt.c"), str(LVGL / "src/font/lv_font_montserrat_14.c"),
                str(LVGL / "src/misc/lv_utils.c"),
                str(LVGL / "src/misc/lv_log.c"), "-o", str(program),
            ], check=True)
            for family, fingerprints in FONTS.items():
                letters = MENU_GLYPHS if family == "Montserrat" else codes
                probes = [(code, 0) for code in letters]
                if family == "Montserrat":
                    probes.extend(itertools.product(letters, repeat=2))  # every kerning pair
                glyphs = "\n".join(f"{code} {next_code}" for code, next_code in probes).encode()
                for size, expected in fingerprints.items():
                    with self.subTest(family=family, size=size):
                        result = subprocess.check_output(
                            [program, family, str(size)], input=glyphs)
                        if family == "Montserrat":
                            encoding, result = result.split(b"\n", 1)
                            self.assertEqual(encoding, b"compressed")
                        self.assertEqual(hashlib.sha256(result).hexdigest(), expected)


if __name__ == "__main__":
    unittest.main()
