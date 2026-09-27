"""Guard the command gate's coverage and identity accounting without game data."""
import copy
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("check_texture_commands",
    Path(__file__).resolve().parents[1] / "tools/check_texture_commands.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


class TextureCommandRunnerTests(unittest.TestCase):
    def setUp(self):
        self.rows = [({"entry_id": 5, "child_path": [2], "sha256": "a" * 64,
                      "offset_in_decoded_entry": 16, "size": 512, "count_word8": 3}, "fixture")]
        self.report = {"schema_version": 1, "scope": tool.ORACLE_SCOPE, "success": True,
                       "input_count": 1, "descriptor_records": 3, "builder_calls": 2,
                       "emitted_command_slots": 4, "max_interpreter_slices": 400,
                       "inputs": [{"entry_id": 5, "child_id": "2", "child_sha256": "a" * 64,
                                   "offset_in_decoded_entry": 16, "size": 512, "records": 3,
                                   "full_commands_sha256": "b" * 64, "partial_commands_sha256": "c" * 64}]}

    def test_exact_shard_is_bound_to_inventory(self):
        result = tool.validate_shard(self.report, self.rows)
        self.assertEqual(result["emitted_command_slots"], 4)
        self.assertEqual(result["builder_calls"], 2)

    def test_omitted_records_or_wrong_identity_cannot_pass(self):
        for key, value in (("input_count", 0), ("descriptor_records", 2), ("builder_calls", 1),
                           ("emitted_command_slots", 3), ("builder_calls", True)):
            with self.subTest(key=key):
                changed = copy.deepcopy(self.report)
                changed[key] = value
                with self.assertRaises(ValueError): tool.validate_shard(changed, self.rows)
        for key in ("entry_id", "child_id", "child_sha256", "offset_in_decoded_entry", "size", "records"):
            changed = copy.deepcopy(self.report)
            changed["inputs"][0][key] = None
            with self.assertRaises(ValueError): tool.validate_shard(changed, self.rows)

    def test_interpreter_bounds_and_command_hashes_are_checked(self):
        for slices in (0, -1, True, tool.MAX_INTERPRETER_SLICES + 1):
            changed = copy.deepcopy(self.report)
            changed["max_interpreter_slices"] = slices
            with self.assertRaises(ValueError): tool.validate_shard(changed, self.rows)
        changed = copy.deepcopy(self.report)
        changed["inputs"][0]["full_commands_sha256"] = "truncated"
        with self.assertRaises(ValueError): tool.validate_shard(changed, self.rows)

    def test_synthetic_execution_cannot_be_replaced_by_empty_report(self):
        report = {"schema_version": 1, "scope": tool.ORACLE_SCOPE, "success": True,
                  "input_count": 0, "descriptor_records": 0, "inputs": [],
                  "synthetic_cases": 24, "builder_calls": 24, "max_interpreter_slices": 3420}
        tool.validate_synthetic(report)
        for key, value in (("synthetic_cases", 0), ("builder_calls", 23),
                           ("success", False), ("scope", "pixels")):
            changed = dict(report, **{key: value})
            with self.assertRaises(ValueError): tool.validate_synthetic(changed)

    def test_shards_preserve_every_ordered_input(self):
        rows = [(dict(self.rows[0][0], entry_id=i, count_word8=75), str(i)) for i in range(70)]
        shards = tool.shard_rows(rows)
        self.assertEqual([row for shard in shards for row in shard], rows)
        self.assertTrue(all(len(shard) == 1 for shard in shards))
        with self.assertRaises(ValueError): tool.unique_object([("same", 1), ("same", 2)])


if __name__ == "__main__":
    unittest.main()
