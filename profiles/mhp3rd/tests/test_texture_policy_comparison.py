"""Keep renderer experiments from silently changing a PSP helper case."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import compare_test_runs as comparison
import texture_decode_policy as policy


class TexturePolicyComparisonTests(unittest.TestCase):
    def setUp(self):
        self.catalog = {"cases": []}
        self.baseline, self.candidate = [self.package(role) for role in ("baseline", "candidate")]

    def package(self, role):
        identity = {name: "same" for name in comparison.IDENTITY_KEYS}
        identity.update(role=role, run_id=role,
                        native_modes={name: "off" for name in comparison.NATIVE_MODES})
        context = {name: "a" * 64 for name in comparison.CONTEXT_KEYS}
        context["case_catalog_sha256"] = comparison.canonical_hash(self.catalog)
        return {"validation": {"metadata_complete": True, "identity_binding": "bound"},
                "manifest": {"identity": identity, "context": context}}

    def declare(self, package, mode="off"):
        package["manifest"]["identity"].update(texture_decode_schema=policy.SCHEMA,
                                            texture_decode_mode=mode)

    def issues(self):
        return comparison.compatibility(self.baseline, self.candidate, self.catalog)

    def test_historical_absence_remains_off(self):
        self.assertEqual(self.issues(), [])

    def test_declared_off_pair_is_comparable(self):
        self.declare(self.baseline)
        self.declare(self.candidate)
        self.assertEqual(self.issues(), [])

    def test_renderer_modes_require_their_own_case_policy(self):
        self.declare(self.baseline)
        for mode in ("verify", "native"):
            self.declare(self.candidate, mode)
            self.assertIn("candidate:texture_decode_requires_renderer_case_policy", self.issues())

    def test_partial_or_unknown_policy_is_not_treated_as_off(self):
        identity = self.candidate["manifest"]["identity"]
        identity["texture_decode_mode"] = "off"
        self.assertIn("candidate:invalid_texture_decode_policy", self.issues())
        identity["texture_decode_schema"] = "unknown"
        self.assertIn("candidate:invalid_texture_decode_policy", self.issues())

    def test_schema_presence_is_bound(self):
        self.declare(self.candidate)
        self.assertIn("identity:texture_decode_schema", self.issues())


if __name__ == "__main__":
    unittest.main()
