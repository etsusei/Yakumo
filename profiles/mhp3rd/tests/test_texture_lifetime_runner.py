"""Do not promote allocation receipts into loaded resource authority."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location("lifetime_gate", TOOLS / "check_texture_lifetime.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


class LifetimeEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.report = dict(schema_version=1, scope="original-texture-lifetime-checkpoints",
                           success=True, full_ram_vram_cpu_compared=True, transfer_readiness=False,
                           max_interpreter_slices=193515, **tool.EXPECTED)

    def test_exact_scope(self):
        tool.validate(self.report)

    def test_missing_counts_or_broader_claims_fail(self):
        for key, value in (("consumed_tickets", 0), ("owner_leases", 8),
                           ("transfer_readiness", True), ("loss_cases", True),
                           ("max_interpreter_slices", 2000000), ("max_interpreter_slices", 0)):
            with self.subTest(key=key), self.assertRaises(ValueError):
                tool.validate(dict(self.report, **{key: value}))
        with self.assertRaises(ValueError):
            tool.validate(dict(self.report, gameplay_validated=True))
        del self.report["transfer_readiness"]
        with self.assertRaises(ValueError):
            tool.validate(self.report)


class TransferManifestBindingTests(unittest.TestCase):
    def test_final_transfer_outputs_and_manifests_are_bound(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            profile = root / "profile"
            build = root / "build"
            original23 = profile / "generated/generated_unit_0023.cpp"
            prior40 = build / "profiles/mhp3rd/texture_lifetime_generated/generated_unit_0040.cpp"
            original23.parent.mkdir(parents=True)
            prior40.parent.mkdir(parents=True)
            original23.write_text("int original23;\n")
            prior40.write_text("int lifetime40;\n")
            for unit, calls, source in (("0023", 3, original23), ("0040", 2, prior40)):
                output = build / "profiles/mhp3rd/texture_transfer_generated" / f"generated_unit_{unit}.cpp"
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_text(f"int transfer{unit};\n")
                manifest = output.with_suffix(".cpp.json")
                manifest.write_text(json.dumps({
                    "schema": "mhp3rd-texture-transfer-instrumentation-v1",
                    "unit": f"generated_unit_{unit}",
                    "checkpoint_calls": calls,
                    "source_sha256": tool.digest(source),
                    "output_sha256": tool.digest(output),
                }))
            bound = tool.transfer_instrumentation_paths(profile, build)
            self.assertEqual(len(bound), 6)
            self.assertEqual(bound["transfer_0040_input"], prior40)
            output = bound["transfer_0023_output"]
            output.write_text("stale output\n")
            with self.assertRaisesRegex(ValueError, "Build-local transfer identity"):
                tool.transfer_instrumentation_paths(profile, build)


if __name__ == "__main__":
    unittest.main()
