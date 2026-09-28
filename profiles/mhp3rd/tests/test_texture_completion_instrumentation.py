import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "instrument_texture_completion", TOOLS / "instrument_texture_completion.py")
assert SPEC and SPEC.loader
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


class CompletionInstrumentationTests(unittest.TestCase):
    def test_call_boundaries_have_audited_after_delay_slot_sites(self):
        self.assertEqual(len(tool.CALL_BOUNDARIES), 11)
        self.assertEqual(len({row[1] for row in tool.CALL_BOUNDARIES}), 11)
        self.assertTrue(all(row[2] for row in tool.CALL_BOUNDARIES))
        names = {row[0] for row in tool.CALL_BOUNDARIES}
        self.assertTrue({
            "InlineCopyCall", "HelperCall", "PolicyCall", "WorkerRequestCall",
            "WorkerWaitCall", "WorkerAckCall", "TransformCall", "DigestCall",
            "RetirementCall",
        } <= names)

    def test_generated_completion_keeps_read_callback_and_call_receipts(self):
        generated = Path(__file__).resolve().parents[1] / "generated" / "generated_unit_0024.cpp"
        if not generated.is_file():
            self.skipTest("private generated corpus is unavailable")
        result, manifest = tool.instrument_source(generated.read_text(encoding="utf-8"))
        self.assertEqual(result.count(tool.CALLBACK), manifest["checkpoint_calls"])
        self.assertEqual(result.count(tool.CALL_BOUNDARY_CALLBACK),
                         manifest["call_boundary_calls"])
        self.assertEqual(result.count("mhp3rd::native::texture_read_checkpoint"), 2)
        self.assertEqual(manifest["audited_checkpoint_calls"], 30)
        marker = "L_088652A4:\n"
        block = result[result.index(marker):]
        self.assertIn(
            "// nop\n    mhp3rd::native::texture_completion_call_boundary",
            block,
        )
        self.assertNotIn("texture_read_oracle_stop_after_result", result)
        self.assertEqual(result.count(tool.STOP_CALLBACK), 1)
        retirement = result.index(
            "TextureCompletionCheckpoint::RetirementReturn")
        self.assertGreater(result.index(tool.STOP_CALLBACK), retirement)

    def test_fixture_shape_is_rejected_without_generated_labels(self):
        with self.assertRaises(ValueError):
            tool.instrument_source("#include \"psprecomp/runtime.hpp\"\n")

    def test_completion_manifest_is_separate_from_read_stop(self):
        source = "#include \"psprecomp/runtime.hpp\"\n"
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "unit.cpp"
            manifest = Path(temp) / "unit.json"
            with self.assertRaises((OSError, ValueError)):
                tool.instrument_file(Path(temp) / "missing.cpp", output, manifest)


if __name__ == "__main__":
    unittest.main()
