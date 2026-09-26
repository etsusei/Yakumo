"""Paired evidence semantics using independently framed synthetic sessions."""
from __future__ import annotations

from copy import deepcopy
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import compare_test_runs as compare
import run_package as package

ENTRY = 0x08877818
ELF_SHA = "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c"


def catalog():
    return {"schema": "yakumo-case-catalog-v1", "cases": [{
        "id": "NATIVE-01", "version": 1, "title": "Inspect a supported helper",
        "steps": ["Perform the route", "Mark the checkpoint"], "checkpoints": ["checked"],
        "required_probes": [{"entry": ENTRY, "min_calls": 1}],
        "required_state_fields": ["health_current"], "human_acceptance": True}]}


def counter(total, variant, boundary, epoch=1):
    fields = {key: 0 for key in compare.COUNTERS}
    fields.update(event="probe.summary", entry=ENTRY, leaf="vector_construct", boundary=boundary,
                  counter_epoch=epoch, entry_hits=total, certified_entries=total, completed=total)
    fields[variant + "_calls"] = total
    return fields


def make_records(role, cases=None, *, calls=1, mode="verify", input_button=1, health=100,
                 before=0, context_changes=None):
    cases = catalog() if cases is None else cases
    ctx = {"schema": "yakumo-run-context-v1", "run_id": role + "-run", "source_commit": "a" * 40,
           "platform_os": "macos", "platform_arch": "arm64",
           **{key: "a" * 64 for key in package.CONTEXT_HASH_FIELDS}}
    ctx["case_catalog_sha256"] = compare.canonical_hash(cases)
    ctx["elf_sha256"] = ELF_SHA
    ctx.update(context_changes or {})
    begin = {"schema": "journal-v1", "role": role, "run_id": ctx["run_id"], "batch_id": "batch-1",
             "baseline_id": "B0", "baseline_commit": "4292eb6", "recorder_revision": "source-sha256:" + "f" * 64,
             "observer_schema": "observers-v1", "recording_mode": "observational-summary",
             "binary_sha256": ("a" if role == "baseline" else "b") * 64,
             "context_sha256": compare.canonical_hash(ctx),
             **{key: "off" for key in compare.NATIVE_MODES}}
    if role == "candidate":
        begin["MHP3RD_NATIVE_VECTOR_CONSTRUCT"] = mode
    identity = {"case_id": "NATIVE-01", "case_version": 1, "attempt": 1}
    variant = "aot" if role == "baseline" else mode
    rows = [(1, begin),
            (3, {**identity, "prerequisites_sha256": "c" * 64}),
            (9, counter(before, variant, "case_begin")),
            (7, {"event": "input.pad", "buttons": input_button, "analog_x": 128, "analog_y": 128,
                 "right_x": 128, "right_y": 128, "count": 1, "domain": "game", "guest_frame": 100}),
            (8, {"event": "game.state", "supported_executable": True, "quest_status": "verified", "health_current": health}),
            (5, {**identity, "checkpoint_id": "checked"}),
            (9, counter(before + calls, variant, "case_end")),
            (4, {**identity, "outcome": "normal"}),
            (8, {"event": "observer.health", "emission_errors": 0, "dropped_events": 0,
                 "invalid_events": 0, "io_failed": False})]
    return ctx, rows


def encode(rows):
    rows = deepcopy(rows)
    if not any(fields.get("event") == "runtime.inputs" for _, fields in rows):
        rows.insert(1, (8, {"event": "runtime.inputs", "supported_elf": True, "elf_sha256": ELF_SHA}))
    count = sum(kind not in (1, 2, 12) for kind, _ in rows)
    rows.append((2, {"completed": True, "stop_reason": "window closed", "accepted_events": count,
                     "written_events": count, "dropped_events": 0, "invalid_events": 0}))
    result = bytearray(struct.pack("<8sHHI", b"YKMJNL1\0", 1, 16, 0))
    for sequence, (kind, fields) in enumerate(rows, 1):
        payload = json.dumps(fields, separators=(",", ":"), allow_nan=False).encode()
        header = struct.pack("<4sIQQHH", b"YKE1", len(payload), sequence, sequence * 1000, kind, 0)
        result.extend(header + struct.pack("<I", zlib.crc32(header + payload) & 0xffffffff) + payload)
    return bytes(result)


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name).resolve()
        self.serial = 0

    def tearDown(self):
        self.temp.cleanup()

    def packaged(self, role, *, edit=None, cases=None, raw_edit=None, **kwargs):
        ctx, rows = make_records(role, cases, **kwargs)
        if edit:
            edit(rows)
        self.serial += 1
        directory = self.root / str(self.serial)
        directory.mkdir()
        run = directory / "run"
        run.mkdir()
        raw = encode(rows)
        if raw_edit:
            raw = raw_edit(raw)
        (run / "events.journal").write_bytes(raw)
        context = directory / "context.json"
        context.write_text(json.dumps(ctx))
        supervisor = directory / "supervisor.json"
        supervisor.write_text(json.dumps({"schema": "yakumo-supervisor-v1", "run_id": ctx["run_id"],
                                         "status": "exited", "exit_code": 4, "stop_reason": "window closed"}))
        output = directory / "package"
        package.package_run(run, context, output, supervisor)
        self.assertEqual((run / "events.journal").read_bytes(), raw)
        return output

    def report(self, *, candidate=None, baseline=None, cases=None):
        b = self.packaged("baseline", **(baseline or {}))
        c = self.packaged("candidate", **(candidate or {}))
        return compare.compare_runs(b, c, cases or catalog())

    def test_reference_matches_do_not_require_identical_manual_input_or_call_totals(self):
        report = self.report(candidate={"calls": 3, "input_button": 2, "before": 40}, baseline={"before": 9})
        self.assertEqual(report["outcome"], "observed_difference")
        row = report["cases"][0]
        self.assertEqual(row["input_alignment"], "different_observed_stream")
        self.assertEqual(row["probes"][0]["candidate"]["delta"]["completed"], 3)
        self.assertEqual(row["reference_verification"]["outcome"], "same_input_match")

    def test_native_only_is_observational_not_exact_function_match(self):
        self.assertEqual(self.report(candidate={"mode": "native"})["outcome"], "observational_match")

    def test_confirmed_mismatch_requires_explicit_certified_same_input_evidence(self):
        def mismatch(rows, certified=True):
            rows.insert(6, (11, {"event": "native.verification_mismatch", "entry": ENTRY,
                                "evidence": "same_input_reference", "certified": certified}))
        confirmed = self.report(candidate={"edit": mismatch})
        self.assertEqual(confirmed["outcome"], "confirmed_mismatch")
        untrusted = self.report(candidate={"edit": lambda rows: mismatch(rows, False)})
        self.assertEqual(untrusted["outcome"], "inconclusive")
        truncated = self.report(candidate={"edit": mismatch, "raw_edit": lambda raw: raw[:-1]})
        self.assertEqual(truncated["outcome"], "incomplete")
        self.assertTrue(truncated["cases"][0]["reference_mismatch_sequences"])

    def test_periodic_state_and_performance_differences_are_diagnostic(self):
        self.assertEqual(self.report(candidate={"health": 70})["outcome"], "observed_difference")
        def timings(rows):
            rows.insert(6, (10, {"event": "perf.summary", "second": 1, "fps": 3.0, "guest_ms": 400.0}))
        report = self.report(candidate={"edit": timings})
        self.assertEqual(report["outcome"], "observational_match")
        self.assertEqual(report["cases"][0]["performance"]["candidate"]["fps"]["median"], 3.0)

    def test_zero_call_missing_state_and_missing_case_are_not_passes(self):
        self.assertEqual(self.report(candidate={"calls": 0})["outcome"], "not_covered")
        self.assertEqual(self.report(candidate={"health": None})["outcome"], "not_covered")
        self.assertEqual(self.report(candidate={"edit": lambda rows: rows.__delitem__(slice(1, 8))})["outcome"], "not_covered")

    def test_missing_boundaries_and_epoch_reset_prevent_cumulative_false_coverage(self):
        for change in (lambda f: f.pop("boundary"), lambda f: f.update(counter_epoch=2),
                       lambda f: f.update(counter_epoch=True), lambda f: f.update(completed=True)):
            with self.subTest(change=change):
                report = self.report(candidate={"before": 100, "edit": lambda rows: change(rows[6][1])})
                self.assertEqual(report["outcome"], "not_covered")

    def test_partial_scope_is_incomplete_without_inventing_mismatch(self):
        def interrupted(rows):
            rows[6][1].update(incomplete=1, completed=0, verify_calls=0)
        report = self.report(candidate={"edit": interrupted})
        self.assertEqual(report["outcome"], "incomplete")
        self.assertEqual(report["cases"][0]["reference_mismatch_sequences"], [])

    def test_crossing_scopes_cannot_cancel_into_false_case_coverage(self):
        def crossing(rows):
            rows[2][1].update(entry_hits=1, certified_entries=1)
            rows[6][1].update(entry_hits=2, certified_entries=2)
        self.assertEqual(self.report(candidate={"edit": crossing})["outcome"], "incomplete")

    def test_baseline_and_outside_case_diagnostics_remain_visible(self):
        def error_inside(rows): rows.insert(6, (11, {"event": "runtime.exception"}))
        self.assertEqual(self.report(baseline={"edit": error_inside})["outcome"], "inconclusive")
        def error_outside(rows): rows.insert(1, (11, {"event": "probe.configuration_error"}))
        report = self.report(candidate={"edit": error_outside})
        self.assertEqual(report["outcome"], "inconclusive")
        self.assertEqual(report["outside_case_diagnostics"]["candidate"][0]["event"], "probe.configuration_error")

    def test_configuration_changes_inside_cases_require_review(self):
        def changed(rows): rows.insert(6, (8, {"event": "config.effective", "settings_sha256": "d" * 64}))
        report = self.report(candidate={"edit": changed})
        self.assertEqual(report["outcome"], "inconclusive")
        self.assertTrue(report["cases"][0]["configuration_events"]["candidate"])

    def test_unknown_duplicate_and_interrupted_one_sided_cases_do_not_vanish(self):
        def unknown(rows):
            for _, fields in rows:
                if "case_id" in fields: fields["case_id"] = "UNKNOWN-01"
        report = self.report(candidate={"edit": unknown})
        self.assertEqual(report["outcome"], "incomplete")
        self.assertEqual(report["unmatched_cases"]["candidate"][0]["case_id"], "UNKNOWN-01")
        def duplicate(rows): rows[8:8] = deepcopy(rows[1:8])
        self.assertEqual(self.report(candidate={"edit": duplicate})["outcome"], "incomplete")
        def no_case(rows): del rows[1:8]
        def unfinished(rows): del rows[7]
        report = self.report(baseline={"edit": no_case}, candidate={"edit": unfinished})
        self.assertEqual(report["cases"][0]["outcome"], "incomplete")

    def test_stale_or_unsupported_state_is_not_checkpoint_coverage(self):
        for edit in (lambda rows: rows[4][1].update(supported_executable=False),
                     lambda rows: rows[4][1].update(quest_status="overlay_identity_changed"),
                     lambda rows: rows[4][1].update(health_current=False)):
            self.assertEqual(self.report(candidate={"edit": edit})["outcome"], "not_covered")
        b, c = self.packaged("baseline"), self.packaged("candidate")
        left, right = package.load_package(b), package.load_package(c)
        next(r for r in right["records"] if r["kind"] == 5)["monotonic_ns"] += 2_000_000_000
        self.assertEqual(compare.compare_loaded(left, right, catalog())["outcome"], "not_covered")

    def test_empty_requirements_and_no_input_are_not_an_observed_match(self):
        cases = catalog();spec = cases["cases"][0]
        spec.update(checkpoints=[], required_probes=[], required_state_fields=[])
        def markers_only(rows):
            rows[:] = [row for row in rows if row[0] in (1, 3, 4) or row[1].get("event") == "observer.health"]
        b = self.packaged("baseline", cases=cases, edit=markers_only)
        c = self.packaged("candidate", cases=cases, edit=markers_only)
        self.assertEqual(compare.compare_runs(b, c, cases)["outcome"], "inconclusive")

    def test_equal_hashes_with_different_hex_case_are_compatible(self):
        self.assertEqual(self.report(candidate={"context_changes": {"game_sha256": "A" * 64}})["outcome"], "observational_match")

    def test_identity_configuration_and_tool_revision_gate_comparison(self):
        self.assertEqual(self.report(candidate={"context_changes": {"starting_save_sha256": "b" * 64}})["outcome"], "incomparable")
        self.assertEqual(self.report(candidate={"edit": lambda rows: rows[0][1].update(recorder_revision="different")})["outcome"], "incomparable")
        self.assertEqual(self.report(candidate={"edit": lambda rows: rows[0][1].pop("context_sha256")})["outcome"], "incomparable")
        self.assertEqual(self.report(baseline={"edit": lambda rows: rows[0][1].update(MHP3RD_NATIVE_VECTOR_CONSTRUCT="native")})["outcome"], "incomparable")
        self.assertEqual(self.report(candidate={"edit": lambda rows: rows[0][1].update(MHP3RD_NATIVE_VECTOR_CONSTRUCT="unknown")})["outcome"], "incomparable")

    def test_case_prerequisites_and_user_outcomes_are_preserved(self):
        changed = self.report(candidate={"edit": lambda rows: rows[1][1].update(prerequisites_sha256="e" * 64)})
        self.assertEqual(changed["outcome"], "incomparable")
        for outcome in ("abnormal", "uncertain", "skipped"):
            report = self.report(candidate={"edit": lambda rows: rows[7][1].update(outcome=outcome)})
            self.assertEqual(report["outcome"], "inconclusive")
            self.assertEqual(report["cases"][0]["user_outcomes"]["candidate"], outcome)

    def test_corruption_loss_and_manifest_tampering_cannot_pass(self):
        self.assertEqual(self.report(candidate={"raw_edit": lambda raw: raw + b"tail"})["outcome"], "incomplete")
        self.assertEqual(self.report(candidate={"edit": lambda rows: rows.insert(5, (12, {"dropped_total": 2}))})["outcome"], "incomplete")
        b, c = self.packaged("baseline"), self.packaged("candidate")
        path = c / "manifest.json"
        manifest = json.loads(path.read_text()); manifest["identity"]["recorder_revision"] = "fabricated"
        path.write_text(json.dumps(manifest))
        self.assertEqual(compare.compare_runs(b, c, catalog())["outcome"], "incomplete")

    def test_html_is_self_contained_and_escapes_all_user_text(self):
        report = self.report()
        report["cases"][0]["title"] = '<script>alert("x")</script>'
        report["cases"][0]["findings"] = ['<img src=x onerror=alert(1)>']
        page = compare.render_html(report)
        self.assertNotIn("<script>", page)
        self.assertNotIn("<img ", page)
        self.assertIn("&lt;script&gt;", page)
        self.assertIn("Content-Security-Policy", page)
        output = self.root / "report"
        compare.write_report(report, output)
        with self.assertRaises(FileExistsError): compare.write_report(report, output)
        self.assertEqual(json.loads((output / "report.json").read_text())["outcome"], report["outcome"])

    def test_cli_does_not_write_into_input_package(self):
        b, c = self.packaged("baseline"), self.packaged("candidate")
        cases = self.root / "cases.json"; cases.write_text(json.dumps(catalog()))
        result = subprocess.run([sys.executable, compare.__file__, "--baseline", str(b), "--candidate", str(c),
                                 "--cases", str(cases), "--output", str(c / "new")], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertFalse((c / "new").exists())


if __name__ == "__main__":
    unittest.main()
