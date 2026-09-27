"""Do not promote allocation receipts into loaded resource authority."""
import importlib.util
from pathlib import Path
import sys
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


if __name__ == "__main__":
    unittest.main()
