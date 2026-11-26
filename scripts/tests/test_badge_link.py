"""Link native objects with the patched badge SDK Makefile."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SDK = Path(os.environ.get("BADGE_SDK", ROOT / "tools/community-re/jieli-sdks/e_badge_707_sdk_200"))
MAKEFILE = SDK / "SDK/build/Makefile.mk"


class BadgeLink(unittest.TestCase):
    def test_source_list_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "build").mkdir()
            sources = work / "build/fileList.mk"
            elf = work / "badge.elf"
            for symbol in ("first", "second"):
                (work / f"{symbol}.c").write_text(f"int {symbol}(void) {{ return 1; }}\n")

            def link(link_at, *symbols):
                sources.write_text("c_SRC_FILES := " + " ".join(f"{s}.c" for s in symbols)
                                   + "\nS_SRC_FILES :=\ncpp_SRC_FILES :=\n")
                subprocess.run(["make", "-sf", str(MAKEFILE), "-j2", elf.name,
                                "BUILD_DIR=objs", "OUT_ELF=badge.elf", "OBJ_FILE=badge.objs",
                                "MKDIR=mkdir -p", "CC=cc", "LD=ld", "LFLAGS=-r", "LIBPATHS=",
                                "LIBS=", f"LINK_AT={link_at}"], cwd=work, check=True)
                output = subprocess.check_output(["nm", "-g", "--defined-only", str(elf)], text=True)
                self.assertEqual({line.split()[-1] for line in output.splitlines()}, set(symbols))
                self.assertEqual((work / "badge.objs").read_text().split(),
                                 [f"objs/{symbol}.c.o" for symbol in symbols])

            for mode in (0, 1):
                with self.subTest(link_at=mode):
                    link(mode, "first")
                    link(mode, "first", "second")
                    link_time = elf.stat().st_mtime_ns
                    link(mode, "second", "first")
                    self.assertGreater(elf.stat().st_mtime_ns, link_time)
                    link(mode, "second")
                    self.assertTrue((work / "objs/first.c.o").is_file())
                    link_time = elf.stat().st_mtime_ns
                    link(mode, "second")
                    self.assertEqual(elf.stat().st_mtime_ns, link_time)


if __name__ == "__main__":
    unittest.main()
