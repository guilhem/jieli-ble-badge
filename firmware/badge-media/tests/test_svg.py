"""Exercise the firmware SVG subset on a host without LVGL or the SDK."""

import ctypes as C
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

MEDIA = Path(__file__).resolve().parents[1]
SVG_START = b'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 360 360">'


class Animation(C.Structure):
    _fields_ = [
        ("duration", C.c_uint32), ("values", C.c_int16 * 16),
        ("count", C.c_uint8), ("loop", C.c_uint8), ("freeze", C.c_uint8),
    ]


class Text(C.Structure):
    _fields_ = [
        ("text", C.c_char_p), ("motion", Animation), ("fade", Animation),
        ("rgb", C.c_uint32), ("x", C.c_int16), ("y", C.c_int16),
        ("size", C.c_uint8), ("serif", C.c_uint8), ("anchor", C.c_uint8), ("opacity", C.c_uint8),
    ]


class Scene(C.Structure):
    _fields_ = [("texts", Text * 4), ("count", C.c_uint8)]


class SvgTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        work = tempfile.TemporaryDirectory()
        cls.addClassCleanup(work.cleanup)
        library = Path(work.name) / "svg.so"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-O2", "-shared", "-fPIC",
            "-I", str(MEDIA), str(MEDIA / "badge_svg.c"), str(MEDIA / "yxml.c"),
            "-o", str(library),
        ], check=True)
        cls.lib = C.CDLL(str(library))
        cls.lib.badge_svg_parse.argtypes = [C.c_void_p, C.c_size_t, C.POINTER(Scene)]
        xy = [C.POINTER(C.c_int)] * 2
        cls.lib.badge_svg_sample.argtypes = [C.POINTER(Text), C.c_uint32, *xy, C.POINTER(C.c_uint8)]
        cls.lib.badge_svg_place.argtypes = [C.POINTER(Text), C.c_int, C.c_int, *xy]
        cls.lib.badge_svg_place.restype = None

    def parse(self, document, valid=True):
        source = (C.c_char * len(document)).from_buffer_copy(document)
        scene = Scene()
        result = self.lib.badge_svg_parse(source, len(document), C.byref(scene))
        self.assertEqual(result, 0 if valid else -1)
        self.addCleanup(lambda: source)
        return scene

    def sample(self, text, ms, width=0, baseline=0):
        x, y, opacity = C.c_int(), C.c_int(), C.c_uint8()
        refs = C.byref(x), C.byref(y)
        active = self.lib.badge_svg_sample(C.byref(text), ms, *refs, C.byref(opacity))
        self.lib.badge_svg_place(C.byref(text), width, baseline, *refs)
        return x.value, y.value, opacity.value, active

    def test_bonjour_and_two_fonts(self):
        scene = self.parse(SVG_START + """
            <text x="180" y="148" font-family="DejaVu Serif" font-size="32px"
                  text-anchor="middle" fill="#FfEECC"> Élodie &amp; André </text>
            <text x="80" y="211" font-family="DejaVu Sans" font-size="20"
                  text-anchor="end" fill="#f0f0ff">Bonjour !
                <animate attributeName="x" from="-20" to="380" dur="2s" repeatCount="indefinite"/>
                <animate attributeName="fill-opacity" values="0; 1; 0" dur="2000ms" repeatCount="indefinite"/>
            </text>
        </svg>""".encode())
        self.assertEqual(scene.count, 2)
        name, greeting = scene.texts[:2]
        self.assertEqual((name.text.decode(), name.rgb, name.size, name.serif), ("Élodie & André", 0xFFEECC, 32, 1))
        self.assertEqual((greeting.text, greeting.rgb, greeting.size, greeting.serif), (b"Bonjour !", 0xF0F0FF, 20, 0))
        self.assertEqual(self.sample(name, 0, width=99, baseline=30), (131, 118, 255, 0))
        self.assertEqual(self.sample(greeting, 0, 80, 19), (-100, 192, 0, 1))
        self.assertEqual(self.sample(greeting, 1000, 80, 19), (100, 192, 255, 1))
        self.assertEqual(self.sample(greeting, 2250, 80, 19), (-50, 192, 64, 1))

    def test_repository_example(self):
        example = MEDIA.parents[1] / "docs/media/animated-text.svg"
        scene = self.parse(example.read_bytes())
        self.assertEqual(scene.count, 2)
        self.assertEqual({text.serif for text in scene.texts[:scene.count]}, {0, 1})
        self.assertIn(b"Bonjour !", scene.texts[1].text)
        self.assertEqual(self.sample(scene.texts[0], 0)[3], 0)
        self.assertEqual(self.sample(scene.texts[1], 0)[3], 1)

    def test_once_freezes_motion_and_restores_base_opacity(self):
        scene = self.parse(SVG_START + b'''
            <text x="12" y="8" fill-opacity=".25">A
                <animate values="0; 50; 100" attributeName="x" dur="400ms" fill="freeze" repeatCount="1"/>
                <animate from="0" to="1" dur=".2s" attributeName="fill-opacity"/>
            </text>
        </svg>''')
        text = scene.texts[0]
        self.assertEqual(self.sample(text, 100), (25, 8, 128, 1))
        self.assertEqual(self.sample(text, 200), (50, 8, 64, 1))
        self.assertEqual(self.sample(text, 400), (100, 8, 64, 0))

    def test_file_and_keyframe_limits(self):
        end = b'<text>Bonjour !</text></svg>'
        exactly = SVG_START + b" " * (4096 - len(SVG_START) - len(end)) + end
        self.parse(exactly)
        self.parse(exactly + b" ", valid=False)
        animate = b'<animate attributeName="x" dur="86400000ms" values="%s"/>'
        for count in (2, 16, 17):
            values = b";".join(str(i - 1).encode() for i in range(count))
            self.parse(SVG_START + b"<text>A" + animate % values + b"</text></svg>", valid=count <= 16)
        self.parse(SVG_START + b"<text>" + b"A" * 128 + b"</text></svg>")

    def test_utf8_and_xml_tokens(self):
        document = (b'\xef\xbb\xbf<?xml version="1.0" encoding="utf-8"?>'
                    b'<!-- SVG text with the literal string <!DOCTYPE -->' + SVG_START +
                    b'<text>   b&#xF4;n\r\n<![CDATA[jour ]x]]>\t&#33;</text></svg>')
        scene = self.parse(document)
        self.assertEqual(scene.texts[0].text.decode(), "bôn jour ]x !")

    def test_incompatible_or_malformed_documents(self):
        documents = [
            b"<svg><text>missing viewBox</text></svg>",
            b'<svg viewBox="0 0 360 360.001"><text>A</text></svg>',
            b'<svg viewBox="0 0 360 360" xmlns="urn:foreign"><text>A</text></svg>',
            b'<svg viewBox="0 0 360 360" xmlns=" http://www.w3.org/2000/svg"><text>A</text></svg>',
            b'<?xml version="1.0" encoding="ISO-8859-1"?>' + SVG_START + b"<text>A</text></svg>",
            b'<?xml version="1.0" encoding = "UTF-8"?><!-- prolog --><!DOCTYPE svg>'
            + SVG_START + b"<text>A</text></svg>",
            b"<text>wrong root</text>",
        ]
        invalid_body = [
            b"", b"<text/>", b"<text> \n </text>", b"<text>A</text>" * 5,
            b"<path/>", b"<text><tspan>A</tspan></text>", b"<?xml-stylesheet?>",
            b"<text>A</svg>", b"<text>&name;</text>", b"<text>\xc0\x80</text>",
            b"<text>\xed\xa0\x80</text>", b"<text>\0</text>", b"<text>\xf0\x9f\x8c\x8d</text>",
            b"<text>&#1;</text>", b"<text>" + b"A" * 129 + b"</text>",
            b'<text x="1" x="2">A</text>', b'<text x="2049">A</text>',
            b'<text x=".">A</text>', b'<text x="-">A</text>', b'<text x="">A</text>',
            b'<text fill-opacity="1.001">A</text>', b'<text fill="#abc">A</text>',
            b'<text font-family="monospace">A</text>', b'<text font-size="24">A</text>',
        ]
        motions = [
            b'attributeName="x" from="0" to="1"',  # missing duration
            b'attributeName="x" from="0" dur="1s"',
            b'attributeName="x" values="0;1;" dur="1s"',
            b'attributeName="x" values="0;1" from="0" to="1" dur="1s"',
            b'attributeName="x" values="0;1" dur="0s"',
            b'attributeName="x" values="0;1" dur="86401s"',
            b'attributeName="x" values="0;1" dur="1s" repeatCount="2"',
            b'attributeName="fill-opacity" values="0;2" dur="1s"',
            b'attributeName="y" values="0;1" dur="1s"',
        ]
        invalid_body.extend(b"<text>A<animate " + motion + b"/></text>" for motion in motions)
        animation = b'<animate attributeName="x" values="0;5" dur="1s"/>'
        invalid_body.append(b"<text>A" + animation * 2 + b"</text>")
        documents.extend(SVG_START + body + b"</svg>" for body in invalid_body)
        documents.append(SVG_START + b"<text>A</text></svg><svg/>")
        for document in documents:
            with self.subTest(document=document):
                self.parse(document, valid=False)
