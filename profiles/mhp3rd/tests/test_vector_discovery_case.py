"""Discovery must not turn unknown calls or a human mark into native acceptance."""
from pathlib import Path
import sys
import unittest

PROFILE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROFILE / "tools"))
import native_batch
import native_modes
import run_cases
from test_initial_case_pack import _panel_mappings, _chinese_entries


class VectorDiscoveryCaseTests(unittest.TestCase):
    def test_discovery_declares_no_unproven_required_call(self):
        catalog = run_cases.load_case_catalog(PROFILE / "testing/cases/vector_metrics_discovery.json")
        profile = native_batch.load_profile(PROFILE / "testing/profiles/vector_discovery_v1.json", catalog)
        self.assertEqual(len(catalog["cases"]), 1)
        case = catalog["cases"][0]
        self.assertEqual(case["id"], "VECTOR-DISCOVERY-01")
        self.assertEqual(case["required_probes"], [])
        self.assertEqual(profile["required_native_entries"], [])
        self.assertEqual(case["required_state_fields"], ["character_loaded"])
        self.assertTrue(case["human_acceptance"])
        self.assertEqual(case["checkpoints"], ["vector_observation_complete"])
        self.assertEqual(len(case["steps"]), 8)
        for name in native_modes.VECTOR_FIELDS:
            self.assertEqual(profile["candidate_modes"][name], "verify")
        for name in native_modes.LEGACY_FIELDS:
            self.assertEqual(profile["candidate_modes"][name], "off")

    def test_discovery_panel_has_chinese_instructions(self):
        catalog = run_cases.load_case_catalog(PROFILE / "testing/cases/vector_metrics_discovery.json")
        mappings, checkpoints, translated = _panel_mappings("case_text"), _panel_mappings("checkpoint_text"), _chinese_entries()
        for case in catalog["cases"]:
            for text in [case["title"], *case["steps"]]:
                self.assertEqual(mappings[text], text)
                self.assertNotEqual(translated[text], text)
            for checkpoint in case["checkpoints"]:
                self.assertIn(checkpoints[checkpoint], translated)


if __name__ == "__main__":
    unittest.main()
