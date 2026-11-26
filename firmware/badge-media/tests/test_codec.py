"""Run with Python 3 and Pillow against the patched SDK (set BADGE_SDK if it lives elsewhere)."""
import ctypes as C
import io
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from PIL import Image, ImageDraw

MEDIA = Path(__file__).resolve().parents[1]
ROOT = MEDIA.parents[1]
SDK = Path(os.environ.get("BADGE_SDK", ROOT / "tools/community-re/jieli-sdks/e_badge_707_sdk_200"))
LVGL = SDK / "SDK/apps/common/ui/lvgl_v8"
sys.path.insert(0, str(ROOT / "tools/qix-ble"))
from qix_ble.avi import build_mjpg_avi

READ = C.CFUNCTYPE(C.c_size_t, C.c_void_p, C.c_void_p, C.c_size_t)
REPORT = C.CFUNCTYPE(None, C.c_char_p, C.c_int)
READ_AT = C.CFUNCTYPE(C.c_size_t, C.c_void_p, C.c_uint32, C.c_void_p, C.c_size_t)
PIXELS = C.c_uint16 * (144 * 144)


class Avi(C.Structure):
    _fields_ = [
        ("frame_count", C.c_uint32), ("frame_us", C.c_uint32), ("read", READ_AT),
        ("ctx", C.c_void_p), ("movie_start", C.c_uint32), ("movie_end", C.c_uint32),
        ("cursor", C.c_uint32),
    ]


def jpeg(image, **kwargs):
    out = io.BytesIO()
    image.save(out, format="JPEG", **kwargs)
    return out.getvalue()


def sample(width, height):
    # Distinct quarters identify rotations, reflections, and channel order.
    im = Image.new("RGB", (width, height))
    draw = ImageDraw.Draw(im)
    for col, row, color in [(0, 0, "red"), (1, 0, "lime"), (0, 1, "blue"), (1, 1, "white")]:
        x = width // 2 if col else 0
        y = height // 2 if row else 0
        draw.rectangle((x, y, width - 1 if col else width // 2 - 1,
                        height - 1 if row else height // 2 - 1), fill=color)
    return im


class CodecTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory()
        so = Path(cls.work.name) / "codec.so"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-shared", "-fPIC",
                        "-DCONFIG_BADGE_MENU", "-I", str(LVGL), str(MEDIA / "badge_media_codec.c"),
                        str(LVGL / "src/extra/libs/sjpg/tjpgd.c"), "-o", str(so)], check=True)
        cls.lib = C.CDLL(str(so))
        cls.lib.badge_jpeg_decode.argtypes = [READ, C.c_void_p, C.POINTER(C.c_uint16)]
        cls.lib.badge_jpeg_decode_report.argtypes = [*cls.lib.badge_jpeg_decode.argtypes, REPORT]
        cls.lib.badge_avi_open.argtypes = [C.POINTER(Avi), READ_AT, C.c_void_p, C.c_uint32]
        cls.lib.badge_avi_next.argtypes = [C.POINTER(Avi), C.POINTER(C.c_uint16)]
        cls.lib.badge_avi_rewind.argtypes = [C.POINTER(Avi)]
        cls.lib.badge_text_valid.argtypes = [C.c_void_p, C.c_size_t]

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def decode(self, data, piece=64, report=None):
        pos = 0

        @READ
        def read(_, dst, n):
            nonlocal pos
            part = data[pos:pos + min(n, piece)]
            C.memmove(dst, part, len(part))
            pos += len(part)
            return len(part)

        pixels = PIXELS()
        if report is not None:
            return self.lib.badge_jpeg_decode_report(read, None, pixels, REPORT(report)), pixels
        return self.lib.badge_jpeg_decode(read, None, pixels), pixels

    def test_camera_original_and_all_exif_orientations(self):
        # The compressed original already exceeds the E87 media shelf before expansion.
        image = sample(4032, 3024)
        expected = ["RGBW", "GRWB", "WBGR", "BWRG", "RBGW", "BRWG", "WGBR", "GWRB"]
        colors = {0xf800: "R", 0x07e0: "G", 0x001f: "B", 0xffff: "W"}
        for orientation, order in enumerate(expected, 1):
            exif = Image.Exif()
            exif[274] = orientation
            data = jpeg(image, exif=exif, quality=80)
            self.assertGreater(len(data), 84 * 1024)
            rc, pixels = self.decode(data, 317)
            self.assertEqual(rc, 0, orientation)
            actual = "".join(colors[pixels[y * 144 + x]] for x, y in
                             [(32, 32), (108, 32), (32, 108), (108, 108)])
            self.assertEqual(actual, order, orientation)

    def test_large_app1_and_truncated_jpegs(self):
        raw = jpeg(sample(357, 291))
        # Valid little-endian TIFF, late IFD, unrelated APP1 payload, short reads.
        ifd = (b"II" + struct.pack("<HI", 42, 4104) + bytes(4096) +
               struct.pack("<HHHIH", 1, 274, 3, 1, 6) + bytes(6))
        app1 = b"Exif\0\0" + ifd
        rotated = raw[:2] + b"\xff\xe1" + struct.pack(">H", len(app1) + 2) + app1 + raw[2:]
        rc, pixels = self.decode(rotated, 7)
        self.assertEqual(rc, 0)
        self.assertEqual(pixels[10 * 144 + 10], 0x001f)
        for n in [1, 2, 3, 19, len(raw) // 2, len(raw) - 10, len(raw) - 2, len(raw) - 1]:
            self.assertNotEqual(self.decode(raw[:n])[0], 0, n)
        self.assertNotEqual(self.decode(b"prefix" + raw)[0], 0)
        self.assertNotEqual(self.decode(jpeg(sample(360, 360), progressive=True))[0], 0)

    def test_small_image_upscales_and_trailing_camera_data_is_accepted(self):
        data = jpeg(sample(17, 19)) + b"separate camera metadata"
        rc, pixels = self.decode(data, 5)
        self.assertEqual(rc, 0)
        self.assertEqual(pixels[0], 0xf800)
        self.assertEqual(pixels[-1], 0xffff)

    def test_decode_reports_failure_stage(self):
        raw = jpeg(sample(357, 291))
        for data, stage, detail in [(raw[:2], b"prepare", 2),
                                    (raw[:len(raw) // 2], b"decompress", 2),
                                    (raw[:-2], b"eoi", -1),
                                    (raw, b"done", 0)]:
            observed = []
            rc, _ = self.decode(data, report=lambda name, value: observed.append((name, value)))
            self.assertEqual(observed, [(stage, detail)])
            self.assertEqual(rc == 0, detail == 0)

    def open_avi(self, data):
        overreads = []
        self.addCleanup(self.assertEqual, overreads, [])

        @READ_AT
        def read(_, pos, dst, n):
            if pos + n > len(data):
                overreads.append((pos, n))
                return 0
            part = data[pos:pos + min(n, 73)]
            C.memmove(dst, part, len(part))
            return len(part)
        a = Avi()
        rc = self.lib.badge_avi_open(C.byref(a), read, None, len(data))
        return rc, a, read

    def test_whole_clip_and_rewind(self):
        frames = []
        for i in range(12):
            im = sample(360, 360)
            ImageDraw.Draw(im).rectangle((i * 20, 140, i * 20 + 35, 205), fill="yellow")
            frames.append(jpeg(im, quality=65))
        data = build_mjpg_avi(frames, fps=12)
        self.assertLess(len(data), 84 * 1024)
        rc, avi, reader = self.open_avi(data)
        self.assertEqual(rc, 0)
        self.assertEqual((avi.frame_count, avi.frame_us), (12, 83333))
        pixels = PIXELS()
        for _ in frames:
            self.assertEqual(self.lib.badge_avi_next(C.byref(avi), pixels), 0)
        self.assertEqual(self.lib.badge_avi_next(C.byref(avi), pixels), 1)
        self.lib.badge_avi_rewind(C.byref(avi))
        self.assertEqual(self.lib.badge_avi_next(C.byref(avi), pixels), 0)

    def test_invalid_clip(self):
        frame = jpeg(sample(360, 360))
        avi = build_mjpg_avi([frame])
        invalid = [
            avi[:-1], avi.replace(b"vids", b"auds"), avi.replace(b"00dc", b"01wb"),
            avi.replace(b"movi", b"xxxx"), build_mjpg_avi([frame], fps=13),
            build_mjpg_avi([frame[:-2]]), build_mjpg_avi([frame], width=400),
        ]
        for data in invalid:
            self.assertNotEqual(self.open_avi(data)[0], 0)
        for data in [build_mjpg_avi([jpeg(sample(50, 50))]),
                     build_mjpg_avi([frame[:100] + frame[-2:]])]:
            rc, avi, reader = self.open_avi(data)
            self.assertEqual(rc, 0)
            self.assertLess(self.lib.badge_avi_next(C.byref(avi), PIXELS()), 0)

    def test_utf8(self):
        for text in ["Guilhem\nÉquipe", "Café à Noël", "🌍", "a" * 512]:
            data = text.encode()
            self.assertTrue(self.lib.badge_text_valid(data, len(data)))
        for data in [b"", b"a" * 513, b"a\x00b", b"\xc0\x80", b"\xe0\x80\x81",
                     b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xe2\x82", b"\x80", b"\x1b"]:
            self.assertFalse(self.lib.badge_text_valid(data, len(data)))

    def test_stream_import_and_flash_recovery(self):
        work = Path(self.work.name)
        source = work / "portrait.jpg"
        exif = Image.Exif()
        exif[274] = 6
        source.write_bytes(jpeg(Image.effect_noise((4032, 3024), 60).convert("RGB"),
                                exif=exif, quality=80))
        clip = work / "loop.avi"
        clip.write_bytes(build_mjpg_avi([jpeg(sample(360, 360))] * 12, fps=12))
        exe = work / "test_store"
        codec = work / "store_codec.o"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-O2", "-DCONFIG_BADGE_MENU",
            "-Dmalloc=badge_test_decode_malloc", "-I", str(LVGL),
            "-c", str(MEDIA / "badge_media_codec.c"), "-o", str(codec),
        ], check=True)
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-O2", "-pthread", "-DCONFIG_BADGE_MENU",
            "-I", str(MEDIA), "-I", str(LVGL), "-I", str(MEDIA / "tests/stubs"),
            str(MEDIA / "tests/test_store.c"), str(MEDIA / "badge_media_store.c"),
            str(MEDIA / "badge_media_import.c"), str(MEDIA / "badge_svg.c"), str(MEDIA / "yxml.c"), str(codec),
            str(LVGL / "src/extra/libs/sjpg/tjpgd.c"), "-o", str(exe),
        ], check=True)
        subprocess.run([str(exe), str(source), str(clip)], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
