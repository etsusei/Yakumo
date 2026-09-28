"""Keep the compiled G1a report bounded and source-readiness claims false."""
import importlib.util
from pathlib import Path
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "texture_transfer_observation_runner",
    TOOLS / "check_texture_transfer_observation.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


class G1aReportTests(unittest.TestCase):
    def setUp(self):
        self.report = dict(
            tool.EXPECTED,
            aot_interpreter_calls=117,
            schema_version=1,
            scope="original-g1a-load-enqueue-descriptor-generation",
            success=True,
            max_interpreter_slices=193515,
            full_ram_vram_cpu_compared=True,
            healthy_authority_exact_notready=True,
            transfer_readiness=False,
            event_notification_modeled=True,
            game_executed=False,
        )

    def test_exact_g1a_scope(self):
        tool.validate(self.report)

    def test_missing_counts_and_broader_claims_fail(self):
        for field, value in (
            ("descriptor_generations", 1), ("fault_cases", 13),
            ("range_fault_cases", 3), ("slot_boundary_cases", 1),
            ("rounded_transform_footprint_cases", 0),
            ("healthy_authority_exact_notready", False),
            ("transfer_readiness", True), ("game_executed", True),
            ("event_notification_modeled", False),
            ("max_interpreter_slices", 2_000_000),
            ("aot_interpreter_calls", 0),
        ):
            with self.subTest(field=field), self.assertRaises(ValueError):
                tool.validate(dict(self.report, **{field: value}))

    def test_unknown_claim_or_field_is_rejected(self):
        with self.assertRaises(ValueError):
            tool.validate(dict(self.report, source_loaded=True))
        report = dict(self.report)
        del report["transfer_readiness"]
        with self.assertRaises(ValueError):
            tool.validate(report)

    def test_instrumentation_manifest_binds_both_sides(self):
        import hashlib
        import json
        import tempfile

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.cpp"
            output = root / "output.cpp"
            manifest = root / "output.cpp.json"
            source.write_text("int original;\n")
            output.write_text("int instrumented;\n")
            sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
            manifest.write_text(json.dumps({
                "schema": "mhp3rd-texture-transfer-instrumentation-v1",
                "unit": "generated_unit_0023",
                "checkpoint_calls": 3,
                "source_sha256": sha(source),
                "output_sha256": sha(output),
            }))
            tool._manifest(manifest, "mhp3rd-texture-transfer-instrumentation-v1",
                           "generated_unit_0023", 3, source, output)
            with self.assertRaises(ValueError):
                tool._manifest(manifest, "mhp3rd-texture-transfer-instrumentation-v1",
                               "generated_unit_0023", 3, output, output)


if __name__ == "__main__":
    unittest.main()
