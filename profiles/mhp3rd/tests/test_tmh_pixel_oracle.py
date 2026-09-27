"""Exercise the real corpus oracle with small synthetic source files."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ORACLE = None


def digest(data):
    return hashlib.sha256(data).hexdigest()


def tmh_fixture():
    image = bytes([0x34]) * 2312 + bytes([0xE7]) * 8
    palette = b"".join(struct.pack("<I", 0xFF000000 | i * 0x010101) for i in range(256))
    blocks = (struct.pack("<4I", len(image) + 16, 0, 4, 68 | 68 << 16) + image +
              struct.pack("<4I", len(palette) + 16, 0, 3, 256) + palette)
    record = struct.pack("<4I", len(blocks) + 16, 0, 1, 0) + blocks
    return b".TMH0.14" + struct.pack("<2I", 1, 0) + record


class PixelOracleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tmh-pixel-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.raw = self.root / "raw-entries"
        self.raw.mkdir()
        self.child = tmh_fixture()
        self.index = self.root / "index.tsv"
        self.report = self.root / "report.json"

    def source(self, prefix=b"", suffix=b"", parent_hash=None, child_hash=None):
        data = prefix + self.child + suffix
        (self.raw / "00001.bin").write_bytes(data)
        self.index.write_text(f"1\t{len(data)}\t{parent_hash or digest(data)}\t"
                              f"{len(prefix)}\t{len(self.child)}\t"
                              f"{child_hash or digest(self.child)}\t2\n", encoding="ascii")
        return data

    def run_oracle(self, mode="fast", actual_mode=None):
        environment = {k: v for k, v in os.environ.items() if not k.startswith("MHP3RD_")}
        if (actual_mode or mode) == "slow":
            environment["MHP3RD_NO_FAST_TEXTURE_DECODE"] = "1"
        return subprocess.run([str(ORACLE), str(self.raw), str(self.index), mode, str(self.report)],
                              env=environment, capture_output=True, text=True, timeout=10)

    def test_missing_root_neighbor_is_not_a_compared_canvas(self):
        data = self.source()
        result = self.run_oracle()
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(self.report.read_text())
        counts = report["counts"]
        self.assertEqual((counts["rectangle_compared"], counts["canvas_compared"],
                          counts["missing_neighbor_context"]), (1, 0, 1))
        record = report["inputs"][0]["records"][0]
        self.assertTrue(record["rectangle"]["strict_swizzle_rejected"])
        self.assertEqual(record["canvas"]["missing_range"],
                         {"root_start": len(data), "root_end_exclusive": 48 + 34 * 128})
        self.assertNotIn("portable_rgba_sha256_le", record["canvas"])
        self.assertEqual((self.raw / "00001.bin").read_bytes(), data)

    def test_canvas_can_use_real_parent_bytes_outside_child(self):
        data = self.source(prefix=bytes(32), suffix=bytes([0x12]) * 2048)
        result = self.run_oracle("slow")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(self.report.read_text())
        record = report["inputs"][0]["records"][0]
        self.assertEqual(report["counts"]["canvas_compared"], 1)
        self.assertTrue(record["canvas_reads_after_tmh_child"])
        self.assertEqual(record["canvas"]["window_root_offset"], 80)
        self.assertEqual(record["canvas"]["window_bytes"], 4352)
        self.assertEqual(record["canvas"]["window_sha256"], digest(data[80:4432]))
        self.assertTrue(record["canvas"]["pixel_equal"])
        self.assertEqual(record["top_left"]["differing_pixels"], 0)

    def test_changed_source_or_child_hash_cannot_publish_success(self):
        for field in ("parent_hash", "child_hash"):
            with self.subTest(field=field):
                self.source(**{field: "0" * 64})
                self.assertNotEqual(self.run_oracle().returncode, 0)
                self.assertFalse(self.report.exists())

    def test_duplicate_inputs_are_rejected(self):
        self.source()
        self.index.write_text(self.index.read_text() * 2)
        self.assertNotEqual(self.run_oracle().returncode, 0)
        self.assertFalse(self.report.exists())

    def test_declared_mode_must_match_actual_legacy_environment(self):
        self.source()
        self.assertNotEqual(self.run_oracle("slow", "fast").returncode, 0)
        self.assertFalse(self.report.exists())

    def test_existing_report_is_preserved(self):
        self.source()
        self.report.write_text("prior evidence")
        self.assertNotEqual(self.run_oracle().returncode, 0)
        self.assertEqual(self.report.read_text(), "prior evidence")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--oracle", required=True, type=Path)
    arguments, rest = parser.parse_known_args()
    ORACLE = arguments.oracle.resolve(strict=True)
    unittest.main(argv=[__file__] + rest)
