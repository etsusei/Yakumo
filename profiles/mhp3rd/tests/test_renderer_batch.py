"""Synthetic, game-free renderer profile and paired evidence checks."""

from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import renderer_batch  # noqa: E402


def catalog() -> dict:
    return {"schema": "yakumo-case-catalog-v1", "cases": [{
        "id": "RENDERER", "version": 1, "title": "Renderer observation",
        "steps": ["Observe a finite game scene"], "checkpoints": ["seen"],
        "required_probes": [], "required_state_fields": [], "human_acceptance": True,
    }]}


def profile(cases: dict, mode: str = "verify") -> dict:
    return {"schema": renderer_batch.SCHEMA, "id": "renderer-1",
            "case_catalog_sha256": renderer_batch._digest(cases),
            "candidate_mode": mode, "minimum_decodes": 2, "coverage_scope": "run_total"}


def counters(mode: str = "verify", requests: int = 5) -> dict:
    value = {name: 0 for name in renderer_batch.COUNTERS}
    value.update({"requests": requests, "immediate_requests": requests - 2 if requests >= 2 else requests,
                  "async_requests": 2 if requests >= 2 else 0,
                  "async_capture_attempts": 2 if requests >= 2 else 0,
                  "portable_success": requests, "verified": requests if mode == "verify" else 0,
                  "native": requests if mode == "native" else 0,
                  "portable_elapsed_ns": 1000 * requests,
                  "reference_elapsed_ns": 2000 * requests if mode == "verify" else 0})
    return value


def counter_record(sequence: int, mode: str = "verify", *, final: bool = True,
                   counts: dict | None = None) -> dict:
    return {"kind": 8, "sequence": sequence, "fields": {
        "event": renderer_batch.COUNTER_EVENT,
        "schema": "yakumo-texture-decode-v1", "mode": mode,
        "scope": renderer_batch.COUNTER_SCOPE,
        "final": final, "workers_drained": final,
        **(counters(mode) if counts is None else counts),
    }}


def fixture(mode: str = "verify") -> tuple[dict, dict, dict, dict]:
    selected = profile(catalog(), mode)
    digest = renderer_batch.profile_sha256(selected)
    packages = {}
    for role in ("baseline", "candidate"):
        identity = {"role": role, "texture_decode_schema": "yakumo-texture-decode-v1",
                    "texture_decode_mode": "off" if role == "baseline" else mode,
                    "renderer_profile_sha256": digest,
                    "case_catalog_sha256": selected["case_catalog_sha256"],
                    "binary_sha256": ("a" if role == "baseline" else "b") * 64,
                    "source_commit": "a234567" if role == "baseline" else "b234567",
                    "recorder_revision": "observer-revision", "observer_schema": "observers-v1"}
        records = [
            {"kind": 1, "sequence": 1, "fields": {"role": role}},
            {"kind": 3, "sequence": 2, "fields": {"case_id": "RENDERER", "case_version": 1,
                                                   "attempt": 1, "prerequisites_sha256": "c" * 64}},
            {"kind": 5, "sequence": 3, "fields": {"case_id": "RENDERER", "case_version": 1,
                                                   "attempt": 1, "checkpoint_id": "seen"}},
            {"kind": 4, "sequence": 4, "fields": {"case_id": "RENDERER", "case_version": 1,
                                                   "attempt": 1, "outcome": "normal"}},
        ]
        if role == "candidate":
            records.append(counter_record(5, mode))
        records.append({"kind": 2, "sequence": len(records) + 1, "fields": {}})
        validation = {"identity_binding": "bound", "metadata_complete": True,
                      "recording_complete": True, "issues": []}
        packages[role] = {"manifest": {"identity": identity,
                                       "context": {"case_catalog_sha256": selected["case_catalog_sha256"]}},
                          "records": records, "validation": validation}
    case = {"case_id": "RENDERER", "case_version": 1, "attempt": 1,
            "outcome": "observational_match",
            "findings": ["required_observations_present_without_confirmed_regression"],
            "user_outcomes": {"baseline": "normal", "candidate": "normal"},
            "human_acceptance_required": True,
            "reference_verification": {"outcome": "not_covered", "scope": "optional_helpers"},
            "sequence_ranges": {"baseline": [2, 4], "candidate": [2, 4]},
            "configuration_events": {"baseline": [], "candidate": []},
            "diagnostics": {"baseline": [], "candidate": []},
            "diagnostics_omitted": {"baseline": 0, "candidate": 0},
            "state_differences": [], "input_alignment": "different_observed_stream"}
    report = {"case_catalog_sha256": selected["case_catalog_sha256"],
              "compatibility_issues": [], "recording_validation": {
                  role: packages[role]["validation"] for role in packages},
              "runs": {role: packages[role]["manifest"]["identity"] for role in packages},
              "case_lifecycle_issues": {"baseline": [], "candidate": []},
              "unmatched_cases": {"baseline": [], "candidate": []},
              "outside_case_diagnostics": {"baseline": [], "candidate": []},
              "outside_case_diagnostics_omitted": {"baseline": 0, "candidate": 0},
              "cases": [case]}
    return report, packages["baseline"], packages["candidate"], selected


def analyze(data: tuple[dict, dict, dict, dict]) -> dict:
    return renderer_batch.analyze_renderer(*data)


def replace_final(candidate: dict, counts: dict) -> None:
    candidate["records"][-2] = counter_record(5, candidate["manifest"]["identity"]["texture_decode_mode"],
                                                counts=counts)


class RendererProfileTests(unittest.TestCase):
    def test_exact_catalog_bound_profile_and_hash(self) -> None:
        cases = catalog()
        value = profile(cases)
        normalized = renderer_batch.validate_profile(value, cases)
        self.assertEqual(normalized, value)
        self.assertIsNot(normalized, value)
        self.assertEqual(renderer_batch.profile_sha256(value), renderer_batch._digest(normalized))
        for mode in ("verify", "native"):
            self.assertEqual(renderer_batch.validate_profile(profile(cases, mode), cases)["candidate_mode"], mode)
        changed = copy.deepcopy(cases)
        changed["cases"][0]["version"] = 2
        with self.assertRaisesRegex(renderer_batch.RendererBatchError, "catalog hash differs"):
            renderer_batch.validate_profile(value, changed)

    def test_rejects_missing_extra_or_bad_fields(self) -> None:
        cases = catalog()
        for name, bad in (("schema", "future"), ("id", "bad/id"),
                          ("case_catalog_sha256", "A" * 64),
                          ("candidate_mode", "off"), ("candidate_mode", True),
                          ("minimum_decodes", 0), ("minimum_decodes", 1_000_001),
                          ("minimum_decodes", True), ("minimum_decodes", 2.0),
                          ("coverage_scope", "case")):
            with self.subTest(name=name, bad=bad):
                value = profile(cases)
                value[name] = bad
                with self.assertRaises(renderer_batch.RendererBatchError):
                    renderer_batch.validate_profile(value, cases)
        for name in profile(cases):
            value = profile(cases)
            del value[name]
            with self.assertRaises(renderer_batch.RendererBatchError):
                renderer_batch.validate_profile(value, cases)
        value = profile(cases)
        value["environment"] = "off"
        with self.assertRaises(renderer_batch.RendererBatchError):
            renderer_batch.validate_profile(value, cases)

    def test_bounded_file_reader(self) -> None:
        cases = catalog()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "profile.json"
            path.write_text(json.dumps(profile(cases)), encoding="utf-8")
            self.assertEqual(renderer_batch.load_profile(path, cases), profile(cases))
            path.write_text(path.read_text().replace('"schema":', '"id":"duplicate", "schema":'))
            with self.assertRaises(renderer_batch.RendererBatchError):
                renderer_batch.load_profile(path, cases)
            path.write_bytes(b" " * (renderer_batch.MAX_PROFILE_BYTES + 1))
            with self.assertRaises(renderer_batch.RendererBatchError):
                renderer_batch.load_profile(path, cases)
            link = root / "linked.json"
            link.symlink_to(path)
            with self.assertRaises(renderer_batch.RendererBatchError):
                renderer_batch.load_profile(link, cases)
            fifo = root / "profile.fifo"
            os.mkfifo(fifo)
            with self.assertRaises(renderer_batch.RendererBatchError):
                renderer_batch.load_profile(fifo, cases)


class RendererEvidenceTests(unittest.TestCase):
    def test_verify_and_native_are_distinct_scoped_outcomes(self) -> None:
        for mode, expected in (("verify", "verified"), ("native", "native_observed")):
            with self.subTest(mode=mode):
                data = fixture(mode)
                result = analyze(data)
                self.assertEqual(result["outcome"], expected)
                self.assertEqual(result["coverage_scope"], "run_total")
                self.assertEqual(result["counter_scope"], renderer_batch.COUNTER_SCOPE)
                self.assertEqual(result["counter_events"], {"baseline": 0, "candidate": 1})
                self.assertIn("not same-input equivalence", " ".join(result["limitations"]))

    def test_missing_duplicate_or_nonterminal_final_is_incomplete(self) -> None:
        data = fixture()
        data[2]["records"][-2]["fields"]["final"] = False
        data[2]["records"][-2]["fields"]["workers_drained"] = False
        self.assertEqual(analyze(data)["outcome"], "incomplete")
        data = fixture()
        data[2]["records"].insert(-1, counter_record(6))
        self.assertEqual(analyze(data)["outcome"], "incomplete")
        data = fixture()
        data[2]["records"].insert(-1, counter_record(6, final=False))
        self.assertEqual(analyze(data)["outcome"], "incomplete")
        data = fixture()
        data[2]["records"][-2]["fields"]["workers_drained"] = False
        self.assertEqual(analyze(data)["outcome"], "incomplete")

    def test_malformed_counter_fields_modes_and_reset_fail_closed(self) -> None:
        for name, bad in (("requests", True), ("portable_success", -1),
                          ("verified", 1.0), ("native", 1 << 64),
                          ("reference_elapsed_ns", None)):
            with self.subTest(field=name):
                data = fixture()
                data[2]["records"][-2]["fields"][name] = bad
                self.assertEqual(analyze(data)["outcome"], "incomplete")
        for name, bad in (("schema", "future"), ("mode", "native"),
                          ("scope", "case")):
            data = fixture()
            data[2]["records"][-2]["fields"][name] = bad
            self.assertEqual(analyze(data)["outcome"], "incomparable")
        data = fixture()
        data[2]["records"][-2]["kind"] = 9
        self.assertEqual(analyze(data)["outcome"], "incomplete")
        data = fixture()
        periodic = counters()
        periodic["requests"] = 6
        periodic["immediate_requests"] = 4
        periodic["async_requests"] = 2
        data[2]["records"].insert(-2, counter_record(5, final=False, counts=periodic))
        data[2]["records"][-2]["sequence"] = 6
        self.assertEqual(analyze(data)["outcome"], "incomplete")

    def test_in_flight_periodic_partition_is_not_treated_as_final(self) -> None:
        data = fixture()
        periodic = counters()
        periodic["portable_success"] = 2
        periodic["verified"] = 2
        periodic["immediate_requests"] = 0
        periodic["async_requests"] = 0
        data[2]["records"].insert(-2, counter_record(5, final=False, counts=periodic))
        data[2]["records"][-2]["sequence"] = 6
        self.assertEqual(analyze(data)["outcome"], "verified")

    def test_final_partition_and_uint64_overflow_are_incomplete(self) -> None:
        data = fixture()
        value = counters()
        value["async_requests"] = 3
        replace_final(data[2], value)
        self.assertEqual(analyze(data)["outcome"], "incomplete")
        data = fixture()
        value = counters()
        value.update({"requests": 0, "immediate_requests": (1 << 64) - 1,
                      "async_requests": 1, "portable_success": 0, "verified": 0})
        replace_final(data[2], value)
        self.assertEqual(analyze(data)["outcome"], "incomplete")
        data = fixture()
        maximum = (1 << 64) - 1
        value = counters()
        value.update({"requests": maximum, "immediate_requests": maximum,
                      "async_requests": 0, "portable_success": maximum, "verified": maximum,
                      "portable_elapsed_ns": maximum, "reference_elapsed_ns": maximum})
        replace_final(data[2], value)
        self.assertEqual(analyze(data)["outcome"], "verified")

    def test_baseline_activity_and_identity_policy_mismatch_are_incomparable(self) -> None:
        data = fixture()
        data[1]["records"].insert(-1, counter_record(5, "off"))
        self.assertEqual(analyze(data)["outcome"], "incomparable")
        for role in (1, 2):
            data = fixture()
            identity = data[role]["manifest"]["identity"]
            del identity["renderer_profile_sha256"]
            data[0]["runs"]["baseline" if role == 1 else "candidate"] = identity
            self.assertEqual(analyze(data)["outcome"], "incomparable")
        data = fixture()
        data[2]["manifest"]["identity"]["texture_decode_mode"] = "native"
        self.assertEqual(analyze(data)["outcome"], "incomparable")
        data = fixture()
        data[0]["compatibility_issues"] = ["context:config_sha256"]
        self.assertEqual(analyze(data)["outcome"], "incomparable")

    def test_zero_low_coverage_and_negative_counters_do_not_pass(self) -> None:
        data = fixture()
        replace_final(data[2], counters(requests=0))
        self.assertEqual(analyze(data)["outcome"], "not_covered")
        data = fixture()
        replace_final(data[2], counters(requests=1))
        self.assertEqual(analyze(data)["outcome"], "not_covered")
        for name in ("fallbacks", "errors", "legacy_failures"):
            data = fixture()
            value = counters()
            value.update({"portable_success": 4, "verified": 4, name: 1})
            replace_final(data[2], value)
            self.assertEqual(analyze(data)["outcome"], "needs_review")
        data = fixture()
        value = counters()
        value.update({"portable_success": 4, "verified": 4, "fallbacks": 1,
                      "unsupported_state": 1})
        replace_final(data[2], value)
        self.assertEqual(analyze(data)["outcome"], "needs_review")
        data = fixture()
        value = counters()
        value["unsupported_state"] = 1
        replace_final(data[2], value)
        self.assertEqual(analyze(data)["outcome"], "incomplete")
        data = fixture()
        value = counters()
        value["snapshot_rejected"] = 1
        replace_final(data[2], value)
        self.assertEqual(analyze(data)["outcome"], "needs_review")
        data = fixture()
        value = counters()
        value["snapshot_rejected"] = 6
        replace_final(data[2], value)
        self.assertEqual(analyze(data)["outcome"], "incomplete")
        data = fixture()
        value = counters()
        value["mismatches"] = 1
        replace_final(data[2], value)
        self.assertEqual(analyze(data)["outcome"], "confirmed_mismatch")
        data = fixture()
        value = counters()
        value["mismatches"] = 1
        data[2]["records"][-2] = counter_record(5, final=False, counts=value)
        data[2]["validation"]["recording_complete"] = False
        result = analyze(data)
        self.assertEqual(result["outcome"], "incomplete")
        self.assertIsNone(result["candidate_counters"])
        self.assertEqual(result["candidate_last_counters"]["mismatches"], 1)
        self.assertTrue(result["partial_mismatch_observed"])
        data = fixture()
        value = counters()
        value["mismatches"] = 1
        replace_final(data[2], value)
        data[2]["validation"]["recording_complete"] = False
        self.assertEqual(analyze(data)["outcome"], "incomplete")

    def test_case_evidence_and_optional_vector_zero_are_separate(self) -> None:
        data = fixture()
        data[0]["cases"][0]["outcome"] = "not_covered"
        data[0]["cases"][0]["findings"] = ["candidate:probe:143159876:not_covered"]
        self.assertEqual(analyze(data)["outcome"], "verified")
        data = fixture()
        data[0]["cases"][0]["user_outcomes"]["candidate"] = "abnormal"
        self.assertEqual(analyze(data)["outcome"], "needs_review")
        data = fixture()
        data[0]["cases"][0]["configuration_events"]["candidate"] = [{"sequence": 3}]
        self.assertEqual(analyze(data)["outcome"], "needs_review")
        data = fixture()
        data[0]["cases"][0]["state_differences"] = [{"field": "some_state"}]
        self.assertEqual(analyze(data)["outcome"], "needs_review")
        data = fixture()
        data[0]["cases"][0].pop("reference_verification")
        self.assertNotEqual(analyze(data)["outcome"], "verified")
        data = fixture()
        data[2]["records"][1]["fields"]["prerequisites_sha256"] = "d" * 64
        self.assertEqual(analyze(data)["outcome"], "incomparable")
        data = fixture()
        data[1]["records"].pop(2)
        self.assertEqual(analyze(data)["outcome"], "needs_review")
        data = fixture()
        data[0]["outside_case_diagnostics"]["candidate"] = [{"event": "error"}]
        self.assertEqual(analyze(data)["outcome"], "needs_review")
        data = fixture()
        data[0]["cases"][0]["outcome"] = "not_covered"
        data[0]["cases"][0]["findings"] = ["candidate:probe:143159876:not_covered"]
        data[2]["records"].insert(3, {"kind": 6, "sequence": 3, "fields": {"message": "visible defect"}})
        self.assertEqual(analyze(data)["outcome"], "needs_review")

    def test_optional_helper_mode_identity_blocks_renderer_but_zero_calls_do_not(self) -> None:
        data = fixture()
        helper = {"profile_id": "vectors", "profile_sha256": "d" * 64}
        data[0]["native_execution"] = {**helper, "outcome": "not_covered",
                                        "issues": ["profile_requests_no_native_execution"]}
        data[0]["profile_observations"] = {**helper, "mode_issues": [],
                                            "unperformed_cases": []}
        self.assertEqual(analyze(data)["outcome"], "verified")
        data[0]["profile_observations"]["mode_issues"] = ["candidate:execution_modes_differ_from_profile"]
        self.assertEqual(analyze(data)["outcome"], "incomparable")
        data[0]["profile_observations"]["mode_issues"] = []
        data[0]["native_execution"]["issues"] = ["candidate:execution_modes_differ_from_profile"]
        self.assertEqual(analyze(data)["outcome"], "incomparable")
        data[0]["native_execution"]["issues"] = []
        data[0]["native_execution"]["profile_sha256"] = "e" * 64
        self.assertEqual(analyze(data)["outcome"], "incomparable")


if __name__ == "__main__":
    unittest.main()
