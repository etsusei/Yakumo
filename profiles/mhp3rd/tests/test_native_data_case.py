"""The delivered native case must bind both targets and translated instructions."""
from pathlib import Path
import sys
import unittest

PROFILE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROFILE / "tools"))
import native_batch
import run_cases
from test_initial_case_pack import _panel_mappings, _chinese_entries


class NativeDataCaseTests(unittest.TestCase):
    def test_authored_profile_and_case_agree(self):
        catalog = run_cases.load_case_catalog(PROFILE / "testing/cases/native_data_batch.json")
        profile = native_batch.load_profile(PROFILE / "testing/profiles/native_data_v1.json", catalog)
        self.assertEqual(len(catalog["cases"]), 1)
        case = catalog["cases"][0]
        self.assertEqual(case["id"], "NATIVE-DATA-01")
        self.assertTrue(case["human_acceptance"])
        self.assertEqual([p["entry"] for p in case["required_probes"]], profile["required_native_entries"])
        self.assertEqual(case["required_state_fields"], ["character_loaded"])
        self.assertEqual(len(case["checkpoints"]), 1)

    def test_panel_covers_every_instruction_and_checkpoint_in_chinese(self):
        catalog = run_cases.load_case_catalog(PROFILE / "testing/cases/native_data_batch.json")
        mappings, checkpoints, translated = _panel_mappings("case_text"), _panel_mappings("checkpoint_text"), _chinese_entries()
        for case in catalog["cases"]:
            for value in [case["title"], *case["steps"]]:
                self.assertEqual(mappings[value], value)
                self.assertNotEqual(translated[value], value)
            for checkpoint in case["checkpoints"]:
                self.assertIn(checkpoints[checkpoint], translated)


if __name__ == "__main__":
    unittest.main()
