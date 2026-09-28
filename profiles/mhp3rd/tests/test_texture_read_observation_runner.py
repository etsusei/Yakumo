"""Keep G1b-read evidence bounded to attempt/result observation."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "texture_read_observation_runner",
    TOOLS / "check_texture_read_observation.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


class G1bReadReportTests(unittest.TestCase):
    def setUp(self):
        self.report = dict(
            tool.EXPECTED_COUNTS,
            **tool.EXPECTED_COUNTERS,
            schema_version=1,
            scope="original-g1b-state8-read-attempt-result-prefix",
            success=True,
            pending_writers_retained=True,
            full_ram_vram_cpu_compared=True,
            read_prefix_stopped_after_result=True,
            copy_transform_terminal_observations=0,
            completion_receipts=0,
            transfer_readiness=False,
            healthy_authority_exact_notready=True,
            modeled_io_imports=True,
            event_scheduling_modeled=True,
            game_executed=False,
            max_interpreter_slices=193515,
            scenario_ids=list(tool.EXPECTED_SCENARIO_IDS),
        )

    def test_exact_g1b_read_scope(self):
        tool.validate(self.report)

    def test_missing_counts_and_broader_claims_fail(self):
        for field, value in (
            ("scenario_count", 68), ("descriptor_byte_resampling_cases", 11),
            ("checkpoint_correlation_fault_cases", 19),
            ("register_argument_fault_cases", 10),
            ("owner_invalidation_cases", 10), ("capacity_loss_cases", 1),
            ("incomplete_attempt_cases", 0), ("unowned_disjoint_reads", 0),
            ("rejected_read_results", 0), ("unowned_read_results", 0),
            ("transfer_readiness", True),
            ("completion_receipts", 1), ("game_executed", True),
            ("max_interpreter_slices", 2_000_000),
        ):
            with self.subTest(field=field), self.assertRaises(ValueError):
                tool.validate(dict(self.report, **{field: value}))

    def test_unknown_claim_or_missing_field_is_rejected(self):
        with self.assertRaises(ValueError):
            tool.validate(dict(self.report, source_bytes_complete=True))
        report = dict(self.report)
        del report["pending_writers_retained"]
        with self.assertRaises(ValueError):
            tool.validate(report)

    def test_result_totals_and_writer_retention_are_checked(self):
        with self.assertRaises(ValueError):
            tool.validate(dict(self.report, retry_read_results=28))
        with self.assertRaises(ValueError):
            tool.validate(dict(self.report, pending_writers_observed=74))

    def test_scenario_identity_and_selected_attempt_correspondence_are_exact(self):
        report = dict(self.report)
        report["scenario_ids"] = list(report["scenario_ids"])
        report["scenario_ids"][0] = "different-scenario"
        with self.assertRaises(ValueError):
            tool.validate(report)
        with self.assertRaises(ValueError):
            tool.validate(dict(self.report, read_attempt_records=39))
        with self.assertRaises(ValueError):
            tool.validate(dict(self.report, aot_interpreter_calls=413))

    def test_shard_must_match_its_contiguous_frozen_scenario_slice(self):
        start, count = 25, 25
        scenario_ids = list(tool.EXPECTED_SCENARIO_IDS[start:start + count])
        shard = dict(self.report)
        shard.update(tool._scenario_categories(scenario_ids))
        shard.update(scenario_start=start, scenario_total=len(tool.EXPECTED_SCENARIO_IDS),
                     scenario_ids=scenario_ids, scenario_count=count,
                     aot_interpreter_calls=25, modeled_read_imports=1,
                     state8_entries=1, read_helper_entries=1, read_invocations=1,
                     read_results=1, exact_read_results=1, retry_read_results=0,
                     read_attempt_records=1, pending_writers_observed=count,
                     unowned_disjoint_reads=0, rejected_read_results=0,
                     unowned_read_results=0, max_interpreter_slices=193515)
        tool._validate_shard(shard, start, count)
        with self.assertRaises(ValueError):
            tool._validate_shard(dict(shard, scenario_start=0), start, count)
        changed = dict(shard)
        changed["scenario_ids"] = list(changed["scenario_ids"])
        changed["scenario_ids"][0] = "duplicate-or-omitted-case"
        with self.assertRaises(ValueError):
            tool._validate_shard(changed, start, count)

    def test_shard_merge_checks_complete_ordered_coverage_and_fixed_totals(self):
        fixed = {
            "schema_version": 1,
            "scope": "original-g1b-state8-read-attempt-result-prefix",
            "success": True,
            "pending_writers_retained": True,
            "full_ram_vram_cpu_compared": True,
            "read_prefix_stopped_after_result": True,
            "copy_transform_terminal_observations": 0,
            "completion_receipts": 0,
            "transfer_readiness": False,
            "healthy_authority_exact_notready": True,
            "modeled_io_imports": True,
            "event_scheduling_modeled": True,
            "game_executed": False,
        }
        shards = []
        for shard_index, start in enumerate(range(0, len(tool.EXPECTED_SCENARIO_IDS),
                                               tool.SCENARIO_SHARD_SIZE)):
            count = min(tool.SCENARIO_SHARD_SIZE,
                        len(tool.EXPECTED_SCENARIO_IDS) - start)
            ids = list(tool.EXPECTED_SCENARIO_IDS[start:start + count])
            shard = dict(fixed)
            shard.update(tool._scenario_categories(ids))
            shard.update(scenario_start=start, scenario_total=len(tool.EXPECTED_SCENARIO_IDS),
                         scenario_ids=ids)
            for key in tool.EXPECTED_COUNTERS:
                value = tool.EXPECTED_COUNTERS[key] if shard_index == 0 else 0
                if key == "pending_writers_observed":
                    value = count
                shard[key] = value
            shard["max_interpreter_slices"] = 193515 if shard_index == 0 else 1
            for key in tool._EVENT_COUNTERS:
                shard[key] = (tool.EXPECTED_COUNTS[key] if shard_index == 0 else 0)
            shards.append(shard)
        merged = tool._merge_shards(shards, ["a" * 64, "b" * 64, "c" * 64])
        self.assertEqual(tuple(merged["scenario_ids"]), tool.EXPECTED_SCENARIO_IDS)
        self.assertEqual(merged["aot_interpreter_calls"], 450)
        with self.assertRaises(ValueError):
            tool._merge_shards(shards[:2], ["a" * 64, "b" * 64])

    def test_read_manifest_binds_the_input_and_build_local_output(self):
        import hashlib

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.cpp"
            output = root / "output.cpp"
            manifest = root / "output.cpp.json"
            source.write_text("int original;\n", encoding="utf-8")
            output.write_text("int instrumented;\n", encoding="utf-8")
            digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
            manifest.write_text(json.dumps({
                "schema": "mhp3rd-texture-read-instrumentation-v1",
                "unit": "generated_unit_0024",
                "checkpoint_calls": 2,
                "source_sha256": digest(source),
                "output_sha256": digest(output),
                "oracle_stop_after_read_result": True,
            }), encoding="utf-8")
            tool._read_manifest(manifest, "generated_unit_0024", source, output)
            with self.assertRaises(ValueError):
                tool._read_manifest(manifest, "generated_unit_0024", output, output)


if __name__ == "__main__":
    unittest.main()
