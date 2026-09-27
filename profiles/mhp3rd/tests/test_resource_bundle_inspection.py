"""Manifest boundaries for the offline bundle audit; no game files required."""
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location(
    "inspect_resource_bundles", Path(__file__).resolve().parents[1] / "tools/inspect_resource_bundles.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


class BundleIndexTests(unittest.TestCase):
    def fixture(self):
        return {"schema_version": 1, "archive": {"entry_count": 2}, "entries": [
            {"id": 0, "output_path": "raw-entries/00000.bin", "extracted_length": 0, "sha256": "a" * 64},
            {"id": 1, "output_path": "raw-entries/00001.bin", "extracted_length": 16, "sha256": "b" * 64},
        ]}

    def test_index_binds_identity_size_and_hash(self):
        self.assertEqual(tool.manifest_index(self.fixture()),
                         "0\t0\t" + "a" * 64 + "\n1\t16\t" + "b" * 64 + "\n")

    def test_wrong_entry_count_is_not_partial_coverage(self):
        value = self.fixture()
        value["archive"]["entry_count"] = 3
        with self.assertRaises(ValueError): tool.manifest_index(value)

    def test_duplicate_or_noninteger_ids_fail(self):
        for ident in (0, True, -1, "1"):
            value = self.fixture()
            value["entries"][1]["id"] = ident
            with self.assertRaises(ValueError): tool.manifest_index(value)

    def test_file_identity_cannot_escape_numeric_path(self):
        for path in ("../manifest.json", "/tmp/payload", "raw-entries/00000.bin", "raw-entries/link.bin"):
            value = self.fixture()
            value["entries"][1]["output_path"] = path
            with self.assertRaises(ValueError): tool.manifest_index(value)

    def test_size_budget_and_digest_are_checked(self):
        for field, invalid in (("extracted_length", -1), ("extracted_length", tool.MAX_FILE + 1),
                               ("extracted_length", True), ("sha256", "g" * 64), ("sha256", "a" * 63)):
            value = self.fixture()
            value["entries"][1][field] = invalid
            with self.assertRaises(ValueError): tool.manifest_index(value)

    def test_duplicate_manifest_keys_rejected(self):
        with self.assertRaises(ValueError): tool.unique_object([("entries", []), ("entries", [])])


if __name__ == "__main__":
    unittest.main()
