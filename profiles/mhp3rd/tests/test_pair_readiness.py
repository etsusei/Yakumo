"""Reject incompatible first-pack delivery identities before running a launcher."""

from copy import deepcopy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from check_pair_readiness import ReadinessError, check_contract
from run_package import NATIVE_MODE_FIELDS


class ReadinessContractTests(unittest.TestCase):
    def setUp(self):
        commit = "4292eb66ee66eab37c327575382d071addcf6249"
        self.pair = {"schema": "yakumo-test-pair-v1", "baseline_id": "B0", "batch_id": "initial",
                     "recorder_revision": "source-sha256:" + "a" * 64,
                     "build_config_sha256": "b" * 64, "configuration_sha256": "c" * 64,
                     "overlays_tree_id": "d" * 64}
        baseline = {"role": "baseline", "baseline_id": "B0", "batch_id": "initial",
                    "source_commit": commit, "baseline_commit": commit,
                    "cases": {"sha256": "e" * 64}, "probe_selection": "all",
                    "native_modes": {key: "off" for key in NATIVE_MODE_FIELDS},
                    "settings": {"ui.language": "zh-CN"}, "work_root": Path("/synthetic/runs"),
                    "iso": {"path": Path("/synthetic/disc.iso"), "sha256": "f" * 64},
                    "elf": {"path": Path("/synthetic/elf"), "sha256": "0" * 64},
                    "starting_save": {"tree_id": "1" * 64}, "overlays": {"tree_id": "d" * 64},
                    "binary": {"sha256": "2" * 64}}
        for key in ("recorder_revision", "build_config_sha256", "configuration_sha256"):
            baseline[key] = self.pair[key]
        candidate = deepcopy(baseline)
        candidate.update(role="candidate", source_commit="3" * 40,
                         native_modes={key: "verify" for key in NATIVE_MODE_FIELDS},
                         binary={"sha256": "4" * 64})
        self.configs = {"baseline": baseline, "candidate": candidate}

    def check(self):
        check_contract(self.pair, self.configs, "e" * 64)

    def test_same_prerequisites_with_distinct_role_binaries(self):
        self.check()

    def test_reject_stale_catalog_despite_matching_pair_claims(self):
        self.configs["candidate"]["cases"]["sha256"] = "5" * 64
        with self.assertRaisesRegex(ReadinessError, "stale case catalog"):
            self.check()

    def test_reject_unplanned_native_mode_or_disabled_discovery(self):
        for role, key, value in [("baseline", "native_modes", {name: "native" for name in NATIVE_MODE_FIELDS}),
                                 ("candidate", "native_modes", {name: "off" for name in NATIVE_MODE_FIELDS}),
                                 ("candidate", "probe_selection", "scale_matrix")]:
            with self.subTest(role=role, key=key):
                original = self.configs[role][key]
                self.configs[role][key] = value
                with self.assertRaises(ReadinessError): self.check()
                self.configs[role][key] = original

    def test_reject_same_binary_or_changed_starting_save(self):
        for key in ("binary", "starting_save"):
            with self.subTest(key=key):
                original = self.configs["candidate"][key]
                self.configs["candidate"][key] = (deepcopy(self.configs["baseline"][key])
                                                     if key == "binary" else {"tree_id": "5" * 64})
                with self.assertRaises(ReadinessError): self.check()
                self.configs["candidate"][key] = original

    def test_reject_role_build_and_baseline_identity_changes(self):
        for key, value in [("role", "baseline"), ("baseline_commit", "6" * 40),
                           ("build_config_sha256", "7" * 64), ("settings", {"ui.language": "en"})]:
            with self.subTest(key=key):
                original = self.configs["candidate"][key]
                self.configs["candidate"][key] = value
                with self.assertRaises(ReadinessError): self.check()
                self.configs["candidate"][key] = original


if __name__ == "__main__":
    unittest.main()
