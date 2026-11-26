from contextlib import redirect_stdout
import io
from pathlib import Path
import unittest

from repack_ufw import crc16_ccitt_false, jl_crc16, parse_ufw, repack


class RepackCrc(unittest.TestCase):
    def test_crc_variants_and_incremental_seed(self):
        self.assertEqual(jl_crc16(b"123456789"), 0x31C3)
        self.assertEqual(crc16_ccitt_false(b"123456789"), 0x29B1)
        self.assertEqual(jl_crc16(b"56789", jl_crc16(b"1234")), 0x31C3)

    def test_repacking_original_app_preserves_every_byte(self):
        root = Path(__file__).resolve().parents[2]
        original = (root / "firmware/custom-fw-ac707n.ufw").read_bytes()
        layout = parse_ufw(original)
        slot = next(item for item in layout.jlfs_app_dir if item.name == "app.bin")
        app = layout.flash_bin_plain[slot.data_offset:slot.data_offset + slot.data_size]
        with redirect_stdout(io.StringIO()):
            result = repack(layout, [("app.bin", 0, app)])
        self.assertTrue(result == original, "repacking changed the reference OTA")


if __name__ == "__main__":
    unittest.main()
