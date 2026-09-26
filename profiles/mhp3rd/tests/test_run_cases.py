"""Offline semantic checks for finite case catalogs and journal case markers."""

from __future__ import annotations

import copy
import sys
import tempfile
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from profiles.mhp3rd.tools import run_cases  # noqa: E402


HASH = "a" * 64


def catalog() -> dict:
    return {
        "schema": run_cases.CATALOG_SCHEMA,
        "cases": [{
            "id": "REC-01",
            "version": 2,
            "title": "Synthetic observation route",
            "steps": ["Begin the route", "Observe the second point"],
            "checkpoints": ["entered", "observed"],
            "required_probes": [{"entry": 0x08878B4C, "min_calls": 1}],
            "required_state_fields": ["player_position"],
            "human_acceptance": True,
        }, {
            "id": "REC-02",
            "version": 1,
            "title": "Optional synthetic route",
            "steps": [],
            "checkpoints": [],
            "required_probes": [],
            "required_state_fields": [],
            "human_acceptance": False,
        }],
    }


def marker(sequence: int, kind: int, case_id: str = "REC-01", version: int = 2,
           attempt: int = 1, **fields) -> dict:
    return {
        "sequence": sequence,
        "monotonic_ns": sequence * 10,
        "kind": kind,
        "fields": {
            "event": "test.synthetic",
            "domain": "game",
            "observation_ordinal": sequence,
            "case_id": case_id,
            "case_version": version,
            "attempt": attempt,
            **fields,
        },
    }


def begin(sequence: int, case_id: str = "REC-01", version: int = 2,
          attempt: int = 1) -> dict:
    return marker(sequence, run_cases.CASE_BEGIN, case_id, version, attempt,
                  prerequisites_sha256=HASH)


def end(sequence: int, outcome: str = "normal", case_id: str = "REC-01",
        version: int = 2, attempt: int = 1) -> dict:
    return marker(sequence, run_cases.CASE_END, case_id, version, attempt,
                  outcome=outcome)


class CatalogTests(unittest.TestCase):
    def test_valid_catalog_is_copied_and_retains_explicit_requirements(self):
        source = catalog()
        validated = run_cases.validate_case_catalog(source)
        source["cases"][0]["checkpoints"].clear()
        self.assertEqual(validated["cases"][0]["checkpoints"], ["entered", "observed"])
        self.assertTrue(validated["cases"][0]["human_acceptance"])
        self.assertEqual(validated["cases"][0]["required_probes"][0]["entry"], 0x08878B4C)

    def test_missing_misspelled_duplicate_and_boolean_fields_cannot_weaken_requirements(self):
        variants = []
        missing = catalog()
        del missing["cases"][0]["required_probes"]
        variants.append(missing)
        extra = catalog()
        extra["cases"][0]["require_probes"] = []
        variants.append(extra)
        duplicate = catalog()
        duplicate["cases"].append(copy.deepcopy(duplicate["cases"][0]))
        variants.append(duplicate)
        duplicate_checkpoint = catalog()
        duplicate_checkpoint["cases"][0]["checkpoints"] = ["entered", "entered"]
        variants.append(duplicate_checkpoint)
        duplicate_probe = catalog()
        duplicate_probe["cases"][0]["required_probes"].append(
            {"entry": 0x08878B4C, "min_calls": 3}
        )
        variants.append(duplicate_probe)
        boolean_entry = catalog()
        boolean_entry["cases"][0]["required_probes"][0]["entry"] = True
        variants.append(boolean_entry)
        boolean_version = catalog()
        boolean_version["cases"][0]["version"] = True
        variants.append(boolean_version)
        zero_calls = catalog()
        zero_calls["cases"][0]["required_probes"][0]["min_calls"] = 0
        variants.append(zero_calls)
        oversized_entry = catalog()
        oversized_entry["cases"][0]["required_probes"][0]["entry"] = 1 << 32
        variants.append(oversized_entry)
        for variant in variants:
            with self.subTest(variant=variant), self.assertRaises(run_cases.CaseCatalogError):
                run_cases.validate_case_catalog(variant)

    def test_bounds_and_json_duplicate_keys_are_rejected(self):
        too_many = catalog()
        too_many["cases"][0]["steps"] = ["step"] * (run_cases.MAX_STEPS + 1)
        with self.assertRaisesRegex(run_cases.CaseCatalogError, "at most"):
            run_cases.validate_case_catalog(too_many)
        unsafe = catalog()
        unsafe["cases"][0]["id"] = "../REC-01"
        with self.assertRaisesRegex(run_cases.CaseCatalogError, "ASCII-safe"):
            run_cases.validate_case_catalog(unsafe)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "cases.json"
            path.write_text('{"schema":"yakumo-case-catalog-v1","schema":"yakumo-case-catalog-v1","cases":[]}')
            with self.assertRaisesRegex(run_cases.CaseCatalogError, "duplicate JSON key"):
                run_cases.load_case_catalog(path)
            path.write_bytes(b" " * (run_cases.MAX_CATALOG_BYTES + 1))
            with self.assertRaisesRegex(run_cases.CaseCatalogError, "exceeds"):
                run_cases.load_case_catalog(path)


class ReconstructionTests(unittest.TestCase):
    def collect(self, records: list[dict]) -> dict:
        return run_cases.collect_cases(records, catalog())

    def test_valid_case_retains_observations_and_uses_ordered_checkpoints(self):
        records = [
            {"sequence": 1, "kind": 7, "fields": {"event": "setup"}},
            begin(2),
            marker(3, 5, checkpoint_id="entered"),
            {"sequence": 4, "kind": 9, "fields": {"event": "probe", "calls": 1}},
            marker(5, 6, message="visual difference"),
            {"sequence": 6, "kind": 11, "fields": {"event": "error", "message": "synthetic"}},
            marker(7, 5, checkpoint_id="observed"),
            end(8, "abnormal"),
            {"sequence": 9, "kind": 2, "fields": {"completed": True}},
        ]
        collected = self.collect(records)
        self.assertEqual(collected["issues"], [])
        case = collected["cases"][0]
        self.assertEqual((case["begin_sequence"], case["end_sequence"]), (2, 8))
        self.assertEqual(case["outcome"], "abnormal")
        self.assertTrue(case["lifecycle_complete"])
        self.assertTrue(case["complete"])
        self.assertEqual([item["sequence"] for item in case["events"]], list(range(2, 9)))
        self.assertEqual([item["sequence"] for item in case["checkpoints"]], [3, 7])
        self.assertEqual([item["sequence"] for item in case["anomalies"]], [5])
        self.assertIn(records[5], case["events"])

    def test_unfinished_case_and_missing_checkpoints_stay_incomplete(self):
        collected = self.collect([begin(1), marker(2, 5, checkpoint_id="entered")])
        case = collected["cases"][0]
        self.assertIsNone(case["end_sequence"])
        self.assertFalse(case["lifecycle_complete"])
        self.assertFalse(case["complete"])
        self.assertEqual(case["missing_checkpoints"], ["observed"])
        self.assertTrue(any("open case" in issue for issue in case["issues"]))

    def test_interleaving_and_duplicate_attempts_cannot_be_complete(self):
        collected = self.collect([
            begin(1),
            begin(2, "REC-02", 1),
            end(3, "uncertain", "REC-02", 1),
            begin(4),
            end(5),
        ])
        first, second, repeated = collected["cases"]
        self.assertFalse(first["complete"])
        self.assertFalse(second["complete"])
        self.assertTrue(second["lifecycle_complete"] is False)
        self.assertFalse(repeated["complete"])
        self.assertTrue(any("duplicate attempt" in item for item in first["issues"]))
        self.assertTrue(any("duplicate attempt" in item for item in repeated["issues"]))

    def test_unknown_case_version_and_wrong_end_identity_remain_diagnostic(self):
        collected = self.collect([
            begin(1, "UNKNOWN", 1), end(2, "skipped", "UNKNOWN", 1),
            begin(3, "REC-01", 9), end(4, "uncertain", "REC-01", 9),
            begin(5), end(6, "normal", "REC-01", 2, 8),
            end(7),
        ])
        unknown, version, mismatch = collected["cases"]
        self.assertTrue(any("unknown case" in issue for issue in unknown["issues"]))
        self.assertEqual(unknown["outcome"], "skipped")
        self.assertTrue(any("version mismatch" in issue for issue in version["issues"]))
        self.assertEqual(version["outcome"], "uncertain")
        self.assertFalse(mismatch["lifecycle_complete"])
        self.assertIsNone(mismatch["outcome"])
        self.assertTrue(any("identity" in issue for issue in mismatch["issues"]))
        self.assertTrue(any("outside a case" in issue for issue in collected["issues"]))

    def test_malformed_begin_does_not_hide_a_later_valid_attempt(self):
        bad = begin(1)
        bad["fields"]["attempt"] = True
        collected = self.collect([
            bad,
            marker(2, 5, checkpoint_id="entered"),
            begin(3, "REC-02", 1),
            end(4, "normal", "REC-02", 1),
        ])
        self.assertEqual(len(collected["cases"]), 1)
        self.assertEqual(collected["cases"][0]["case_id"], "REC-02")
        self.assertTrue(collected["cases"][0]["complete"])
        self.assertTrue(any("malformed CaseBegin" in issue for issue in collected["issues"]))
        self.assertTrue(any("outside a case" in issue for issue in collected["issues"]))

    def test_checkpoint_order_duplicates_malformed_fields_and_loss(self):
        malformed = marker(6, 5, checkpoint_id=True)
        malformed["fields"]["attempt"] = True
        invalid_outcome = end(8)
        invalid_outcome["fields"]["outcome"] = []
        collected = self.collect([
            begin(1),
            marker(2, 5, checkpoint_id="observed"),
            marker(3, 5, checkpoint_id="observed"),
            marker(4, 5, checkpoint_id="unknown"),
            marker(5, 5, checkpoint_id="entered"),
            malformed,
            {"sequence": 7, "kind": 12, "fields": {"dropped": 1}},
            invalid_outcome,
        ])
        case = collected["cases"][0]
        self.assertFalse(case["complete"])
        self.assertFalse(case["lifecycle_complete"])
        self.assertEqual(case["missing_checkpoints"], [])
        for fragment in ("out of order", "duplicate checkpoint", "unknown checkpoint",
                         "malformed case marker", "malformed checkpoint_id", "recording loss",
                         "malformed case outcome"):
            self.assertTrue(any(fragment in issue for issue in case["issues"]), fragment)

    def test_skipped_and_uncertain_outcomes_are_retained_without_inventing_acceptance(self):
        collected = self.collect([
            begin(1, "REC-02", 1), end(2, "skipped", "REC-02", 1),
            begin(3, "REC-02", 1, 2), end(4, "uncertain", "REC-02", 1, 2),
        ])
        self.assertEqual([case["outcome"] for case in collected["cases"]], ["skipped", "uncertain"])
        self.assertTrue(all(case["complete"] for case in collected["cases"]))
        self.assertEqual(collected["issues"], [])


if __name__ == "__main__":
    unittest.main()
