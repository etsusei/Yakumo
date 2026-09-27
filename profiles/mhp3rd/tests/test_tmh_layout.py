"""Independent synthetic bounds checks for structural TMH inventory."""
import importlib.util
from pathlib import Path
import struct
import unittest

SPEC = importlib.util.spec_from_file_location("inspect_tmh_layout",
    Path(__file__).resolve().parents[1] / "tools/inspect_tmh_layout.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


def sample(tail=b""):
    # Unknown fields are deliberately not the values seen in game resources.
    return (struct.pack("<8s2I", b".TMH0.14", 1, 0xCAFE) +
            struct.pack("<4I", 48, 0xDEADBEEF, 7, 99) +
            struct.pack("<4I", 32, 91, 999, 0x01020304) + bytes(range(16)) + tail)


class TmhLayoutTests(unittest.TestCase):
    def test_unknown_words_and_outer_tail_are_preserved(self):
        result = tool.layout(sample(b"tail"))
        self.assertEqual(result["reserved_word12"], 0xCAFE)
        self.assertEqual(result["consumed_bytes"], 64)
        self.assertEqual(result["unindexed_tail_length"], 4)
        record = result["records"][0]
        self.assertEqual(record["word4"], 0xDEADBEEF)
        self.assertFalse(record["header_word_sum_matches_block_count"])
        block = record["blocks"][0]
        self.assertEqual((block["offset"], block["payload_offset"], block["payload_size"]), (32, 48, 16))
        self.assertEqual((block["tag_word4"], block["format_word8"]), (91, 999))

    def test_header_and_count_boundaries(self):
        for data in (b"", sample()[:15], b".TMH0.15" + sample()[8:],
                     struct.pack("<8s2I", b".TMH0.14", 0xFFFFFFFF, 0)):
            with self.assertRaises(ValueError): tool.layout(data)
        with self.assertRaises(ValueError): tool.layout(sample(), max_records=0)

    def test_invalid_record_and_block_strides(self):
        for at, value in ((16, 0), (16, 15), (16, 0xFFFFFFFF), (32, 0), (32, 15), (32, 33)):
            data = bytearray(sample())
            struct.pack_into("<I", data, at, value)
            with self.assertRaises(ValueError): tool.layout(data)

    def test_each_counted_record_must_exist(self):
        data = bytearray(sample())
        struct.pack_into("<I", data, 8, 2)
        with self.assertRaises(ValueError): tool.layout(data)

    def test_nested_offset_coordinate_is_explicit(self):
        node = {"children": [{"status": "present", "index": 2, "offset": 100, "advertised_length": 80,
            "nested_candidate": {"children": [{"status": "present", "index": 1, "offset": 20,
                "advertised_length": 40, "annotation": "TMH_marker", "sha256": "a" * 64}]}}]}
        self.assertEqual(list(tool.children(node)), [(120, 40, "a" * 64, (2, 1))])
        node["children"][0]["offset"] = -1
        with self.assertRaises(ValueError): list(tool.children(node))


if __name__ == "__main__":
    unittest.main()
