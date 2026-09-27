"""Cross-check the shipped first case pack and its reviewed panel translations."""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path


PROFILE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROFILE / "tools"))

import run_cases  # noqa: E402


CATALOG = PROFILE / "testing/cases/initial_batch.json"
SCREEN = PROFILE / "host/ui/test_session_screen.cpp"
TRANSLATIONS = PROFILE / "host/ui/translations/zh_cn.inc"


def _panel_mappings(section: str) -> dict[str, str]:
    source = SCREEN.read_text(encoding="utf-8")
    start = source.index(f"std::string_view {section}(")
    end = source.index("\n}\n", start)
    body = source[start:end]
    pairs = re.findall(r'if \(value == "([^"\\]+)"\)\s*return tr\("([^"\\]+)"\);', body)
    result: dict[str, str] = {}
    for key, translated_key in pairs:
        if key in result:
            raise AssertionError(f"duplicate {section} mapping: {key}")
        result[key] = translated_key
    return result


def _chinese_entries() -> dict[str, str]:
    entries: dict[str, str] = {}
    for line in TRANSLATIONS.read_text(encoding="utf-8").splitlines():
        found = re.fullmatch(r'\{"([^"\\]+)", "([^"\\]+)"\},', line)
        if found:
            key, value = found.groups()
            if key in entries:
                raise AssertionError(f"duplicate translation: {key}")
            entries[key] = value
    return entries


class InitialCasePackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.catalog = run_cases.load_case_catalog(CATALOG)
        cls.case_mappings = _panel_mappings("case_text")
        cls.checkpoint_mappings = _panel_mappings("checkpoint_text")
        cls.chinese = _chinese_entries()

    def test_finite_order_and_evidence_requirements(self) -> None:
        cases = self.catalog["cases"]
        self.assertEqual([case["id"] for case in cases],
                         ["REC-01", "REC-02", "NATIVE-01", "REC-03"])
        self.assertTrue(all(case["version"] == 1 and case["human_acceptance"] for case in cases))
        self.assertTrue(all(1 <= len(case["steps"]) <= 7 for case in cases))
        self.assertTrue(all(len(step) <= 140 for case in cases for step in case["steps"]))
        self.assertTrue(all(1 <= len(case["checkpoints"]) <= 2 for case in cases))
        self.assertEqual(sum((case["required_probes"] for case in cases), []),
                         [{"entry": 0x08878B28, "min_calls": 1}])
        self.assertEqual([case["required_state_fields"] for case in cases],
                         [["character_loaded"], [], ["character_loaded"], ["character_loaded"]])
        self.assertIn("Before this case", cases[0]["steps"][0])
        self.assertIn("after this case has ended", cases[-1]["steps"][-1])
        self.assertEqual(len({checkpoint for case in cases for checkpoint in case["checkpoints"]}),
                         sum(len(case["checkpoints"]) for case in cases))

    def test_every_catalog_string_is_displayed_in_chinese(self) -> None:
        for case in self.catalog["cases"]:
            with self.subTest(case=case["id"]):
                for literal in [case["title"], *case["steps"]]:
                    self.assertEqual(self.case_mappings.get(literal), literal)
                    self.assertIn(literal, self.chinese)
                    self.assertNotEqual(self.chinese[literal], literal)
                for checkpoint in case["checkpoints"]:
                    label = self.checkpoint_mappings.get(checkpoint)
                    self.assertIsNotNone(label, checkpoint)
                    self.assertIn(label, self.chinese)
                    self.assertNotEqual(self.chinese[label], label)


if __name__ == "__main__":
    unittest.main()
