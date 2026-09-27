"""Reject incompatible first-pack delivery identities before running a launcher."""

from copy import deepcopy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from check_pair_readiness import ReadinessError, check_contract
import native_batch
import native_modes
import texture_decode_policy
from run_package import NATIVE_MODE_FIELDS


class ReadinessContractTests(unittest.TestCase):
    def test_v2_profile_requires_all_nine_modes_and_sealed_baseline(self):
        catalog = {"schema": "yakumo-case-catalog-v1", "cases": [{
            "id": "VECTOR", "version": 1, "title": "Synthetic vector case",
            "steps": ["Observe"], "checkpoints": ["done"],
            "required_probes": [{"entry": 0x08877244, "min_calls": 1}],
            "required_state_fields": [], "human_acceptance": True,
        }]}
        from check_pair_readiness import digest
        catalog_hash = digest(catalog)
        modes = {name: "off" for name in native_modes.V2_FIELDS}
        modes["MHP3RD_NATIVE_VECTOR_NORM"] = "native"
        profile = {"schema": native_batch.V2_SCHEMA, "id": "vector-test",
                   "case_catalog_sha256": catalog_hash,
                   "candidate_modes": modes, "required_native_entries": [0x08877244]}
        self.pair["execution_profile"] = profile
        self.pair["execution_profile_sha256"] = native_batch.profile_sha256(profile)
        self.pair["native_mode_schema"] = native_modes.V2_SCHEMA
        for role, config in self.configs.items():
            config["cases"]["sha256"] = catalog_hash
            config["native_mode_schema"] = native_modes.V2_SCHEMA
            config["native_modes"] = ({name: "off" for name in native_modes.V2_FIELDS}
                                      if role == "baseline" else dict(modes))
        self.assertEqual(check_contract(self.pair, self.configs, catalog_hash, catalog), profile)
        self.configs["baseline"]["native_modes"]["MHP3RD_NATIVE_VECTOR_NORM"] = "native"
        with self.assertRaisesRegex(ReadinessError, "Native mode differs"):
            check_contract(self.pair, self.configs, catalog_hash, catalog)
        self.configs["baseline"]["native_modes"]["MHP3RD_NATIVE_VECTOR_NORM"] = "off"
        del self.configs["candidate"]["native_mode_schema"]
        with self.assertRaisesRegex(ReadinessError, "schema differs"):
            check_contract(self.pair, self.configs, catalog_hash, catalog)

    def setUp(self):
        commit = "4292eb66ee66eab37c327575382d071addcf6249"
        self.pair = {"schema": "yakumo-test-pair-v1", "baseline_id": "B0", "batch_id": "initial",
                     "recorder_revision": "source-sha256:" + "a" * 64,
                     "build_config_sha256": "b" * 64, "configuration_sha256": "c" * 64,
                     "overlays_tree_id": "d" * 64,
                     "game_font": {"path": "/synthetic/game-font.ttc", "sha256": "8" * 64}}
        baseline = {"role": "baseline", "baseline_id": "B0", "batch_id": "initial",
                    "source_commit": commit, "baseline_commit": commit,
                    "cases": {"sha256": "e" * 64}, "probe_selection": "all",
                    "native_modes": {key: "off" for key in NATIVE_MODE_FIELDS},
                    "settings": {"ui.language": "zh-CN", "text.font": "/synthetic/game-font.ttc"},
                    "work_root": Path("/synthetic/runs"),
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

    def test_texture_policy_is_separate_and_baseline_stays_off(self):
        self.pair.update({texture_decode_policy.SCHEMA_FIELD: texture_decode_policy.SCHEMA,
                          texture_decode_policy.MODE_FIELD: "verify"})
        for role, config in self.configs.items():
            config.update({texture_decode_policy.SCHEMA_FIELD: texture_decode_policy.SCHEMA,
                           texture_decode_policy.MODE_FIELD: "off" if role == "baseline" else "verify"})
        self.check()
        self.configs["baseline"][texture_decode_policy.MODE_FIELD] = "native"
        with self.assertRaisesRegex(ReadinessError, "texture decode mode"):
            self.check()
        self.configs["baseline"][texture_decode_policy.MODE_FIELD] = "off"
        del self.configs["candidate"][texture_decode_policy.SCHEMA_FIELD]
        with self.assertRaises(ReadinessError):
            self.check()

    def test_reject_stale_catalog_despite_matching_pair_claims(self):
        self.configs["candidate"]["cases"]["sha256"] = "5" * 64
        with self.assertRaisesRegex(ReadinessError, "stale case catalog"):
            self.check()

    def test_reject_missing_game_font_even_when_roles_match(self):
        for config in self.configs.values():
            config["settings"].pop("text.font")
        with self.assertRaisesRegex(ReadinessError, "game-text font"):
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

    def test_explicit_native_profile_binds_catalog_modes_and_probe_requirements(self):
        catalog = {"schema": "yakumo-case-catalog-v1", "cases": [{
            "id": "NATIVE-DATA", "version": 1, "title": "Village native data",
            "steps": ["Observe"], "checkpoints": ["done"],
            "required_probes": [{"entry": entry, "min_calls": 1}
                                for entry in native_batch.REQUIRED_NATIVE_ENTRIES],
            "required_state_fields": [], "human_acceptance": True,
        }]}
        from check_pair_readiness import digest
        catalog_hash = digest(catalog)
        profile = {"schema": native_batch.SCHEMA, "id": "native-data-1",
                   "case_catalog_sha256": catalog_hash,
                   "candidate_modes": {
                       switch: ("native" if entry in native_batch.REQUIRED_NATIVE_ENTRIES else "off")
                       for entry, switch in native_batch.NATIVE_SWITCH_BY_ENTRY.items()},
                   "required_native_entries": list(native_batch.REQUIRED_NATIVE_ENTRIES)}
        self.pair["execution_profile"] = profile
        self.pair["execution_profile_sha256"] = native_batch.profile_sha256(profile)
        for config in self.configs.values():
            config["cases"]["sha256"] = catalog_hash
        self.configs["candidate"]["native_modes"] = dict(profile["candidate_modes"])
        self.assertEqual(check_contract(self.pair, self.configs, catalog_hash, catalog), profile)
        self.assertNotEqual(self.pair["batch_id"], profile["id"])

        self.configs["candidate"]["native_modes"]["MHP3RD_NATIVE_MATRIX_COPY"] = "verify"
        with self.assertRaisesRegex(ReadinessError, "Native mode differs"):
            check_contract(self.pair, self.configs, catalog_hash, catalog)
        self.configs["candidate"]["native_modes"] = dict(profile["candidate_modes"])
        self.pair["execution_profile_sha256"] = "0" * 64
        with self.assertRaisesRegex(ReadinessError, "digest differs"):
            check_contract(self.pair, self.configs, catalog_hash, catalog)
        self.pair["execution_profile_sha256"] = native_batch.profile_sha256(profile)
        catalog["cases"][0]["required_probes"].pop()
        with self.assertRaisesRegex(ReadinessError, "catalog hash differs"):
            check_contract(self.pair, self.configs, catalog_hash, catalog)


if __name__ == "__main__":
    unittest.main()
