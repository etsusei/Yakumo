"""Keep callback-path evidence distinct from production authority claims."""
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("dispatch_runner",
    Path(__file__).resolve().parents[1] / "tools/check_texture_command_dispatch.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


class DispatchEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.result = dict(schema_version=1, scope="texture-command-instrumented-aot",
                           success=True, cases=32, full_ram_vram_cpu_compared=True,
                           caller_chain_cases=18,
                           production_authority=False, elf_sha256=tool.ELF_HASH,
                           max_interpreter_slices=140)

    def test_complete_scoped_result(self):
        tool.validate_result(self.result)

    def test_incomplete_or_broader_claims_rejected(self):
        for key, value in (("cases", 31), ("production_authority", True),
                           ("full_ram_vram_cpu_compared", False), ("success", 1),
                           ("caller_chain_cases", 17),
                           ("elf_sha256", "unknown"), ("max_interpreter_slices", 100000),
                           ("max_interpreter_slices", True), ("max_interpreter_slices", 0)):
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                tool.validate_result(dict(self.result, **{key: value}))
        with self.assertRaises(ValueError):
            tool.validate_result(dict(self.result, gameplay_validated=True))
        del self.result["production_authority"]
        with self.assertRaises(ValueError):
            tool.validate_result(self.result)


if __name__ == "__main__":
    unittest.main()
