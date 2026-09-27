#!/usr/bin/env python3
"""Conservative, local-only comparison of paired observation packages."""
from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import html
import json
import math
from pathlib import Path
import re
import statistics
import sys

if __package__:
    from . import native_modes
    from .native_batch import load_profile, validate_profile, profile_sha256
    from .run_cases import collect_cases, load_case_catalog, validate_case_catalog
    from .run_package import load_package
else:
    import native_modes
    from native_batch import load_profile, validate_profile, profile_sha256
    from run_cases import collect_cases, load_case_catalog, validate_case_catalog
    from run_package import load_package

SCHEMA = "yakumo-comparison-v1"
NATIVE_MODES = native_modes.LEGACY_FIELDS
CERTIFIED_ENTRIES = set(native_modes.V2_BY_ENTRY)
IDENTITY_KEYS = ("batch_id", "baseline_id", "baseline_commit", "recorder_revision",
                 "observer_schema", "recording_mode")
CONTEXT_KEYS = ("game_sha256", "elf_sha256", "overlay_sha256", "starting_save_sha256", "config_sha256",
                "build_config_sha256", "case_catalog_sha256", "platform_os", "platform_arch")
COUNTERS = ("entry_hits", "certified_entries", "uncertified_entries", "uncertified_returns",
            "completed", "incomplete", "orphan_exits", "return_mismatches",
            "aot_calls", "native_calls", "verify_calls", "fallback_calls")
PERFORMANCE_KEYS = ("fps", "game_fps", "emulation_speed", "frame_average_ms",
                    "frame_maximum_ms", "guest_ms", "render_ms", "wait_ms", "gpu_average_ms")
COMMON_INPUT_KEYS = {"observation_ordinal", "control_read_ordinal", "guest_frame", "virtual_us",
                     "vblank", "source_timestamp_ns", "window_id", "device_id"}


def canonical_hash(value: object) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                    ensure_ascii=False, allow_nan=False).encode("utf-8")).hexdigest()


def comparison_revision() -> str:
    directory = Path(__file__).resolve().parent
    files = ("compare_test_runs.py", "run_package.py", "run_cases.py", "native_batch.py",
             "native_modes.py")
    return "source-sha256:" + canonical_hash({name: hashlib.sha256((directory / name).read_bytes()).hexdigest()
                                              for name in files})


def unsigned(value: object) -> bool:
    return type(value) is int and 0 <= value < (1 << 64)


def compatibility(baseline: dict, candidate: dict, catalog: dict) -> list[str]:
    issues = []
    for role, package in (("baseline", baseline), ("candidate", candidate)):
        validation = package["validation"]
        identity = package["manifest"].get("identity", {})
        if identity.get("role") != role:
            issues.append(role + ":wrong_role")
        if not validation.get("metadata_complete"):
            issues.append(role + ":metadata_incomplete")
        if validation.get("identity_binding") != "bound":
            issues.append(role + ":launch_context_unbound")
        declared_catalog = package["manifest"].get("context", {}).get("case_catalog_sha256")
        if type(declared_catalog) is not str or declared_catalog.lower() != canonical_hash(catalog):
            issues.append(role + ":case_catalog_differs")
    b, c = baseline["manifest"], candidate["manifest"]
    left_schema = b.get("identity", {}).get("native_mode_schema")
    right_schema = c.get("identity", {}).get("native_mode_schema")
    if left_schema != right_schema:
        issues.append("identity:native_mode_schema")
    try:
        expected_modes = native_modes.fields(left_schema)
        expected_entries = native_modes.entries(left_schema)
        for spec in catalog["cases"]:
            if any(probe["entry"] not in expected_entries for probe in spec["required_probes"]):
                issues.append("catalog:probe_outside_native_mode_schema")
                break
    except ValueError:
        issues.append("identity:unknown_native_mode_schema")
        expected_modes = native_modes.LEGACY_FIELDS
    for key in IDENTITY_KEYS:
        left, right = b.get("identity", {}).get(key), c.get("identity", {}).get(key)
        if not left or not right or left != right:
            issues.append("identity:" + key)
    for key in CONTEXT_KEYS:
        left, right = b.get("context", {}).get(key), c.get("context", {}).get(key)
        if key.endswith("_sha256"):
            left = left.lower() if type(left) is str else left
            right = right.lower() if type(right) is str else right
        if not left or not right or left != right:
            issues.append("context:" + key)
    if b.get("identity", {}).get("run_id") == c.get("identity", {}).get("run_id"):
        issues.append("same_run_id")
    baseline_modes = b.get("identity", {}).get("native_modes", {})
    for key in expected_modes:
        if baseline_modes.get(key) not in ("off", "0"):
            issues.append("baseline_replacement_not_off:" + key)
    return issues


def probe_window(case: dict, entry: int, minimum: int) -> dict:
    rows = [r for r in case["events"] if r["kind"] == 9 and
            r["fields"].get("event") == "probe.summary" and unsigned(r["fields"].get("entry")) and
            r["fields"].get("entry") == entry]
    begins = [r for r in rows if r["fields"].get("boundary") == "case_begin"]
    ends = [r for r in rows if r["fields"].get("boundary") == "case_end"]
    result = {"entry": entry, "minimum_calls": minimum, "status": "not_covered", "delta": {}, "issues": []}
    if len(begins) != 1 or len(ends) != 1:
        result["issues"].append("missing_or_duplicate_case_counter_boundaries")
        return result
    begin, end = begins[0], ends[0]
    before, after = begin["fields"], end["fields"]
    if begin["sequence"] >= end["sequence"] or not unsigned(before.get("counter_epoch")) or \
            before["counter_epoch"] == 0 or not unsigned(after.get("counter_epoch")) or \
            before["counter_epoch"] != after.get("counter_epoch"):
        result["issues"].append("counter_epoch_or_order_differs")
        return result
    for key in COUNTERS:
        a, b = before.get(key), after.get(key)
        if not unsigned(a) or not unsigned(b) or b < a:
            result["issues"].append("invalid_or_reset_counter:" + key)
        else:
            result["delta"][key] = b - a
    if result["issues"]:
        return result
    for label, row in (("begin", before), ("end", after)):
        if row["certified_entries"] + row["uncertified_entries"] != row["entry_hits"] or \
                row["entry_hits"] != row["completed"] + row["incomplete"] + row["uncertified_returns"]:
            result["status"] = "incomplete"
            result["issues"].append("open_or_inconsistent_scopes_at_case_" + label)
            return result
    delta = result["delta"]
    if sum(delta[v + "_calls"] for v in ("aot", "native", "verify", "fallback")) != delta["completed"]:
        result["issues"].append("inconsistent_variant_counts")
        return result
    if delta["incomplete"] or delta["orphan_exits"] or delta["return_mismatches"]:
        result["status"] = "incomplete"
        result["issues"].append("incomplete_probe_scopes")
    elif delta["uncertified_entries"] or delta["uncertified_returns"] or \
            delta["completed"] != delta["entry_hits"] or delta["certified_entries"] != delta["entry_hits"]:
        result["issues"].append("uncertified_or_open_probe_scopes")
    elif delta["completed"] < minimum:
        result["issues"].append("required_calls_not_observed")
    else:
        result["status"] = "covered"
    return result


def input_summary(case: dict) -> dict:
    records = []
    counts: Counter = Counter()
    first_frame = next((r["fields"].get("guest_frame") for r in case["events"]
                        if unsigned(r["fields"].get("guest_frame"))), None)
    for record in case["events"]:
        fields = record["fields"]
        if record["kind"] != 7:
            continue
        event = fields.get("event", "unknown")
        counts[event] += 1
        canonical = {k: v for k, v in fields.items() if k not in COMMON_INPUT_KEYS}
        frame = fields.get("guest_frame")
        canonical["relative_guest_frame"] = frame - first_frame if unsigned(frame) and first_frame is not None else None
        records.append(canonical)
    return {"events": len(records), "event_counts": dict(counts),
            "observed_stream_sha256": canonical_hash(records) if records else None,
            "scope": "observed_inputs_only_not_complete_game_state_or_rng"}


def state_value(fields: dict, key: str):
    if fields.get("supported_executable") is not True:
        return None
    value = fields.get(key)
    if key == "character_loaded":
        return value if type(value) is bool else None
    if key == "money_zenny":
        return value if fields.get("character_loaded") is True and unsigned(value) else None
    if fields.get("quest_status") != "verified":
        return None
    if key in ("stamina_current_game_units", "stamina_maximum_game_units"):
        return value if type(value) in (int, float) and math.isfinite(value) and value >= 0 else None
    if key in ("health_current", "health_recoverable", "health_maximum", "quest_time_left_frames",
               "quest_time_limit_frames", "overlay_generation", "overlay_code_epoch") or \
            re.fullmatch(r"monster_slot_[0-4]_(kind|health|maximum_health)", key):
        return value if unsigned(value) else None
    return None


def checkpoint_states(case: dict, fields: list[str]) -> dict:
    result = {}
    previous = case["begin_sequence"]
    end_record = next((r for r in reversed(case["events"]) if r["kind"] == 4), None)
    boundaries = case["checkpoints"] or ([{**end_record, "fields": {**end_record["fields"], "checkpoint_id": "case_end"}}]
                                          if end_record else [])
    for checkpoint in boundaries:
        candidates = [r for r in case["events"] if previous < r["sequence"] < checkpoint["sequence"] and
                      r["kind"] == 8 and r["fields"].get("event") == "game.state"]
        last = candidates[-1] if candidates else None
        age_ns = checkpoint["monotonic_ns"] - last["monotonic_ns"] if last else None
        frame, sample_frame = checkpoint["fields"].get("guest_frame"), last["fields"].get("guest_frame") if last else None
        age_frames = frame - sample_frame if unsigned(frame) and unsigned(sample_frame) else None
        fresh = age_ns is not None and 0 <= age_ns <= 1_500_000_000 and \
            (age_frames is None or 0 <= age_frames <= 30)
        result[checkpoint["fields"]["checkpoint_id"]] = {
            "sample_sequence": last["sequence"] if last else None,
            "sample_age_host_ns": age_ns, "sample_age_guest_frames": age_frames,
            "fresh": fresh,
            "values": {key: state_value(last["fields"], key) if last and fresh else None for key in fields},
            "coverage": "periodic_sample_not_exact_checkpoint_state",
        }
        previous = checkpoint["sequence"]
    return result


def performance(case: dict) -> dict:
    values: dict[str, list[float]] = {key: [] for key in PERFORMANCE_KEYS}
    seen = set()
    for record in case["events"]:
        f = record["fields"]
        if record["kind"] != 10 or f.get("event") != "perf.summary":
            continue
        second = f.get("second")
        if not unsigned(second) or second in seen:
            continue
        seen.add(second)
        for key in PERFORMANCE_KEYS:
            value = f.get(key)
            if type(value) in (int, float) and math.isfinite(value) and value >= 0:
                values[key].append(value)
    return {key: {"samples": len(samples), "median": statistics.median(samples) if samples else None}
            for key, samples in values.items()}


def compare_case(b: dict | None, c: dict | None, spec: dict, pair_issues: list[str],
                 recordings_complete: bool, certified_entries: frozenset[int]) -> dict:
    present = b or c
    result = {"case_id": spec["id"], "title": spec["title"], "case_version": spec["version"],
              "attempt": present["attempt"] if present else 1,
              "outcome": "not_covered", "findings": [], "probes": [], "state_differences": []}
    if b is None or c is None:
        result["findings"].append("case_missing_from_" + ("baseline" if b is None else "candidate"))
        if present is not None and not present["lifecycle_complete"]:
            result["outcome"] = "incomplete"
            result["findings"].extend(present["issues"])
        return result
    result["user_outcomes"] = {"baseline": b["outcome"], "candidate": c["outcome"]}
    result["human_acceptance_required"] = spec["human_acceptance"]
    result["configuration_events"] = {
        role: [{"sequence": r["sequence"], "settings_sha256": r["fields"].get("settings_sha256")}
               for r in case["events"] if r["kind"] == 8 and r["fields"].get("event") == "config.effective"]
        for role, case in (("baseline", b), ("candidate", c))}
    result["sequence_ranges"] = {"baseline": [b["begin_sequence"], b["end_sequence"]],
                                 "candidate": [c["begin_sequence"], c["end_sequence"]]}
    result["inputs"] = {"baseline": input_summary(b), "candidate": input_summary(c)}
    left_hash, right_hash = (result["inputs"][role]["observed_stream_sha256"] for role in ("baseline", "candidate"))
    result["input_alignment"] = "not_observed" if not left_hash or not right_hash else \
        "same_observed_stream" if left_hash == right_hash else "different_observed_stream"
    result["performance"] = {"baseline": performance(b), "candidate": performance(c),
                             "interpretation": "diagnostic_only_no_automatic_speedup_or_failure_claim"}
    errors = {role: [r for r in case["events"] if r["kind"] in (11, 12)] for role, case in (("baseline", b), ("candidate", c))}
    all_errors = errors["candidate"]
    result["diagnostics"] = {role: [{"sequence": r["sequence"], "event": r["fields"].get("event", "recording_loss")}
                                   for r in rows[:128]] for role, rows in errors.items()}
    result["diagnostics_omitted"] = {role: max(0, len(rows) - 128) for role, rows in errors.items()}
    mismatch = [r for r in all_errors if r["fields"].get("event") == "native.verification_mismatch" and
                r["fields"].get("evidence") == "same_input_reference" and r["fields"].get("certified") is True and
                unsigned(r["fields"].get("entry")) and r["fields"].get("entry") in certified_entries]
    result["reference_mismatch_sequences"] = [r["sequence"] for r in mismatch]
    if not recordings_complete or not b["lifecycle_complete"] or not c["lifecycle_complete"]:
        result["outcome"] = "incomplete"
        result["findings"].extend(["recording_or_case_lifecycle_incomplete", *b["issues"], *c["issues"]])
        return result
    if pair_issues or b["prerequisites_sha256"] != c["prerequisites_sha256"] or b["case_version"] != c["case_version"]:
        result["outcome"] = "incomparable"
        result["findings"].extend(pair_issues or ["case_prerequisites_or_version_differ"])
        return result
    if mismatch:
        result["outcome"] = "confirmed_mismatch"
        result["findings"].append("certified_candidate_same_input_reference_check_failed")
        return result
    if b["outcome"] != "normal" or c["outcome"] != "normal":
        result["outcome"] = "inconclusive"
        result["findings"].append("user_marked_abnormal_uncertain_or_skipped")
        return result
    if not b["complete"] or not c["complete"]:
        result["findings"].append("required_checkpoints_missing_or_invalid")
        result["missing_checkpoints"] = {"baseline": b["missing_checkpoints"], "candidate": c["missing_checkpoints"]}
        return result
    state_fields = spec["required_state_fields"]
    states = {"baseline": checkpoint_states(b, state_fields), "candidate": checkpoint_states(c, state_fields)}
    result["checkpoint_states"] = states
    for checkpoint, sample in states["baseline"].items():
        other = states["candidate"].get(checkpoint, {"values": {}})
        for key in state_fields:
            x, y = sample["values"].get(key), other["values"].get(key)
            if x is None or y is None:
                result["findings"].append("state_unavailable:" + checkpoint + ":" + key)
            elif type(x) is not type(y) or x != y:
                result["state_differences"].append({"checkpoint": checkpoint, "field": key,
                                                     "baseline": x, "candidate": y})
    all_verified = bool(spec["required_probes"])
    for required in spec["required_probes"]:
        baseline = probe_window(b, required["entry"], required["min_calls"])
        candidate = probe_window(c, required["entry"], required["min_calls"])
        if baseline["status"] == "covered" and baseline["delta"]["aot_calls"] != baseline["delta"]["completed"]:
            baseline["status"] = "incomplete"
            baseline["issues"].append("baseline_scope_did_not_use_original_aot")
        result["probes"].append({"entry": required["entry"], "baseline": baseline, "candidate": candidate})
        all_verified &= candidate["status"] == "covered" and candidate["delta"].get("verify_calls", 0) > 0 and \
            candidate["delta"].get("verify_calls") == candidate["delta"].get("completed")
        for role, item in (("baseline", baseline), ("candidate", candidate)):
            if item["status"] != "covered":
                result["findings"].append(role + ":probe:" + str(required["entry"]) + ":" + item["status"])
    result["reference_verification"] = {
        "outcome": "same_input_match" if all_verified else "not_covered",
        "scope": "required_candidate_helper_calls_checked_in_process_not_paired_route_equality"}
    if any(p[role]["status"] == "incomplete" for p in result["probes"] for role in ("baseline", "candidate")):
        result["outcome"] = "incomplete"
    elif result["findings"]:
        result["outcome"] = "not_covered"
    elif any(errors.values()) or b["anomalies"] or c["anomalies"]:
        result["outcome"] = "inconclusive"
        result["findings"].append("diagnostics_or_user_anomalies_require_review")
    elif any(result["configuration_events"].values()):
        result["outcome"] = "inconclusive"
        result["findings"].append("configuration_changed_during_case_requires_review")
    elif result["state_differences"]:
        result["outcome"] = "observed_difference"
        result["findings"].append("periodic_state_difference_not_proof_of_regression")
    elif result["input_alignment"] == "different_observed_stream":
        result["outcome"] = "observed_difference"
        result["findings"].append("manual_input_streams_differ_review_without_assuming_regression")
    elif not result["probes"] and not state_fields and result["input_alignment"] == "not_observed":
        result["outcome"] = "inconclusive"
        result["findings"].append("user_marked_normal_without_observed_evidence")
    else:
        result["outcome"] = "observational_match"
        result["findings"].append("required_observations_present_without_confirmed_regression")
    return result


def native_execution(report: dict, profile: dict) -> dict:
    """Prove requested native execution, separately from reference equivalence."""
    result = {"profile_id": profile["id"], "profile_sha256": profile_sha256(profile),
              "outcome": "not_covered", "scope": "declared_native_paths_in_finite_cases_not_same_input_proof",
              "issues": [], "cases": []}
    for role in ("baseline", "candidate"):
        identity = report["runs"][role]
        actual = identity.get("native_modes", {})
        required_schema = None if profile["schema"] == "yakumo-native-batch-v1" else native_modes.V2_SCHEMA
        if identity.get("native_mode_schema") != required_schema:
            result["issues"].append(role + ":execution_mode_schema_differs_from_profile")
        expected = {key: "off" for key in native_modes.fields(required_schema)} if role == "baseline" else profile["candidate_modes"]
        if actual != expected:
            result["issues"].append(role + ":execution_modes_differ_from_profile")
    if report["compatibility_issues"] or result["issues"]:
        result["outcome"] = "incomparable"
        result["issues"].extend(report["compatibility_issues"])
        return result
    if (any(not value.get("recording_complete") for value in report["recording_validation"].values()) or
            any(report["case_lifecycle_issues"].values()) or
            any(case["outcome"] == "incomplete" for case in report["cases"])):
        result["outcome"] = "incomplete"
        result["issues"].append("recording_or_case_lifecycle_incomplete")
        return result
    if (any(report["outside_case_diagnostics"].values()) or
            any(report["outside_case_diagnostics_omitted"].values())):
        result["issues"].append("outside_case_diagnostics_require_review")

    seen = set()
    all_cases_eligible = True
    for case in report["cases"]:
        issues = []
        if case["outcome"] not in ("observational_match", "observed_difference"):
            issues.append("case_requires_review:" + case["outcome"])
        if case.get("user_outcomes") != {"baseline": "normal", "candidate": "normal"}:
            issues.append("normal_user_outcomes_missing")
        if case.get("state_differences"):
            issues.append("checkpoint_state_differs")
        if any(case.get("configuration_events", {}).values()):
            issues.append("configuration_changed_during_case")
        if any(case.get("diagnostics", {}).values()) or any(case.get("diagnostics_omitted", {}).values()):
            issues.append("case_diagnostics_require_review")
        targets = []
        for probe in case.get("probes", []):
            if probe["entry"] not in profile["required_native_entries"]:
                continue
            target_issues = []
            for role, variant in (("baseline", "aot_calls"), ("candidate", "native_calls")):
                window = probe[role]
                counts = window["delta"]
                completed = counts.get("completed")
                if (window["status"] != "covered" or not unsigned(completed) or completed == 0 or
                        counts.get(variant) != completed or
                        any(counts.get(key) != 0 for key in
                            ("aot_calls", "native_calls", "verify_calls", "fallback_calls") if key != variant)):
                    target_issues.append(role + ":requested_execution_path_not_exclusive")
            targets.append({"entry": probe["entry"], "outcome": "observed" if not target_issues else "not_covered",
                            "issues": target_issues})
            if not target_issues:
                seen.add(probe["entry"])
            else:
                issues.extend(target_issues)
        if issues:
            all_cases_eligible = False
        result["cases"].append({"case_id": case["case_id"], "attempt": case.get("attempt"),
                                "outcome": "observed" if targets and not issues else
                                           "not_covered" if not targets and not issues else "needs_review",
                                "targets": targets, "issues": issues})
    missing = set(profile["required_native_entries"]) - seen
    if missing:
        result["issues"].append("native_targets_not_covered:" + ",".join(f"0x{x:08X}" for x in sorted(missing)))
    no_native_targets = not profile["required_native_entries"]
    if no_native_targets:
        result["issues"].append("profile_requests_no_native_execution")
    if no_native_targets:
        result["outcome"] = "not_covered"
    elif not missing and all_cases_eligible and not result["issues"]:
        result["outcome"] = "observed"
    elif not missing:
        result["outcome"] = "needs_review"
    return result


def _observation_window(case: dict | None, entry: int) -> dict:
    """Expose a selected helper's scoped counters without making it required."""
    if case is None:
        return {"entry": entry, "minimum_calls": 1, "status": "not_covered", "delta": {},
                "duration_totals_ns": {}, "duration_status": "not_covered",
                "issues": ["case_missing_from_role"]}
    result = probe_window(case, entry, 1)
    result["duration_totals_ns"] = {}
    result["duration_status"] = "not_covered"
    if result["status"] != "covered":
        return result
    rows = [r["fields"] for r in case["events"] if r["kind"] == 9 and
            r["fields"].get("event") == "probe.summary" and r["fields"].get("entry") == entry]
    begin = next(r for r in rows if r.get("boundary") == "case_begin")
    end = next(r for r in rows if r.get("boundary") == "case_end")
    for variant in ("aot", "native", "verify", "fallback"):
        key = variant + "_total_ns"
        before, after = begin.get(key), end.get(key)
        if not unsigned(before) or not unsigned(after):
            result["issues"].append("missing_or_invalid_duration:" + key)
        elif after < before:
            result["issues"].append("reset_duration:" + key)
        else:
            result["duration_totals_ns"][variant] = after - before
    result["duration_status"] = "complete" if len(result["duration_totals_ns"]) == 4 else "incomplete"
    return result


def profile_observations(report: dict, profile: dict, indexed: dict,
                         catalog: dict) -> dict:
    """Show optional enabled-helper windows; never decide native acceptance."""
    mode_issues = []
    for role in ("baseline", "candidate"):
        identity = report["runs"][role]
        if identity.get("native_mode_schema") != native_modes.V2_SCHEMA:
            mode_issues.append(role + ":execution_mode_schema_differs_from_profile")
        actual = identity.get("native_modes", {})
        expected = ({name: "off" for name in native_modes.V2_FIELDS} if role == "baseline"
                    else profile["candidate_modes"])
        if actual != expected:
            mode_issues.append(role + ":execution_modes_differ_from_profile")
    enabled = [(entry, switch, profile["candidate_modes"][switch])
               for entry, switch in native_modes.V2_BY_ENTRY.items()
               if profile["candidate_modes"][switch] in ("verify", "native")]
    result = {"profile_id": profile["id"], "profile_sha256": profile_sha256(profile),
              "scope": "optional_scoped_helper_discovery_not_case_acceptance_or_speedup",
              "duration_scope": "case_delta_of_cumulative_host_ns_including_observation_overhead; not a speedup comparison",
              "mode_issues": mode_issues, "compatibility_issues": report["compatibility_issues"],
              "cases": [], "unperformed_cases": []}
    for spec in catalog["cases"]:
        keys = sorted({key for role in ("baseline", "candidate") for key in indexed[role]
                       if key[0] == spec["id"]})
        if not keys:
            result["unperformed_cases"].append({
                "case_id": spec["id"], "coverage": "not_covered", "issue": "case_not_performed",
                "entries": [{"entry": entry, "switch": switch, "candidate_mode": mode}
                            for entry, switch, mode in enabled]})
        for key in keys:
            b, c = indexed["baseline"].get(key), indexed["candidate"].get(key)
            shared_issues = []
            if mode_issues or report["compatibility_issues"]:
                shared_issues.append("profile_or_run_identity_incompatible")
            if (any(not value.get("recording_complete") for value in report["recording_validation"].values()) or
                    any(report["case_lifecycle_issues"].values()) or
                    any(case is not None and (not case["lifecycle_complete"] or not case["complete"])
                        for case in (b, c))):
                shared_issues.append("recording_or_case_incomplete")
            if b is None or c is None:
                shared_issues.append("paired_case_attempt_missing")
            elif (b["prerequisites_sha256"] != c["prerequisites_sha256"] or
                  b["case_version"] != c["case_version"] or b["case_version"] != spec["version"]):
                shared_issues.append("case_prerequisites_or_version_differ")
            if any(r["kind"] in (11, 12) for case in (b, c) if case is not None for r in case["events"]):
                shared_issues.append("case_diagnostics_require_review")
            if any(report["outside_case_diagnostics"].values()) or any(
                    report["outside_case_diagnostics_omitted"].values()):
                shared_issues.append("outside_case_diagnostics_require_review")
            if any(r["kind"] == 8 and r["fields"].get("event") == "config.effective"
                   for case in (b, c) if case is not None for r in case["events"]):
                shared_issues.append("configuration_changed_during_case")
            entries = []
            for entry, switch, mode in enabled:
                baseline = _observation_window(b, entry)
                candidate = _observation_window(c, entry)
                issues = list(shared_issues)
                if baseline["status"] == "covered" and baseline["delta"].get("aot_calls") != baseline["delta"].get("completed"):
                    issues.append("baseline_scope_did_not_use_original_aot")
                if candidate["status"] == "covered":
                    if candidate["delta"].get("aot_calls"):
                        issues.append("candidate_unexpected_aot_calls_in_enabled_mode")
                    if mode == "verify" and candidate["delta"].get("native_calls"):
                        issues.append("candidate_unexpected_native_calls_in_verify_mode")
                    if mode == "native" and candidate["delta"].get("verify_calls"):
                        issues.append("candidate_unexpected_verify_calls_in_native_mode")
                for role, window in (("baseline", baseline), ("candidate", candidate)):
                    if window["status"] == "covered" and window["duration_status"] != "complete":
                        issues.append(role + ":duration_totals_incomplete")
                if "profile_or_run_identity_incompatible" in issues or "case_prerequisites_or_version_differ" in issues:
                    coverage = "incomparable"
                elif "recording_or_case_incomplete" in issues or any(
                        window["status"] == "incomplete" for window in (baseline, candidate)):
                    coverage = "incomplete"
                elif baseline["status"] != "covered" or candidate["status"] != "covered":
                    coverage = "not_covered"
                elif issues:
                    coverage = "needs_review"
                else:
                    coverage = "observed"
                verified = candidate["delta"].get("verify_calls", 0) if candidate["status"] == "covered" else 0
                fallback = candidate["delta"].get("fallback_calls", 0) if candidate["status"] == "covered" else 0
                if coverage == "observed" and mode == "verify" and verified:
                    verification = ("covered" if verified == candidate["delta"]["completed"] else "partial")
                else:
                    verification = "not_covered"
                entries.append({"entry": entry, "switch": switch, "candidate_mode": mode,
                                "baseline": baseline, "candidate": candidate, "coverage": coverage,
                                "candidate_verify_calls": verified, "candidate_fallback_calls": fallback,
                                "candidate_reference_verification": verification, "issues": issues})
            result["cases"].append({"case_id": spec["id"], "case_version": spec["version"],
                                    "attempt": key[1], "entries": entries})
    return result


def compare_loaded(baseline: dict, candidate: dict, catalog: dict, execution_profile: dict | None = None) -> dict:
    catalog = validate_case_catalog(catalog)
    if execution_profile is not None:
        execution_profile = validate_profile(execution_profile, catalog)
    issues = compatibility(baseline, candidate, catalog)
    baseline_schema = baseline["manifest"].get("identity", {}).get("native_mode_schema")
    candidate_schema = candidate["manifest"].get("identity", {}).get("native_mode_schema")
    try:
        certified_entries = (native_modes.entries(baseline_schema) if baseline_schema == candidate_schema
                             else frozenset())
    except ValueError:
        certified_entries = frozenset()
    reconstructed = {"baseline": collect_cases(baseline["records"], catalog),
                     "candidate": collect_cases(candidate["records"], catalog)}
    expected_ids = {spec["id"] for spec in catalog["cases"]}
    unmatched = {}
    indexed = {}
    outside_diagnostics = {}
    for role, package in (("baseline", baseline), ("candidate", candidate)):
        indexed[role] = {}
        unmatched[role] = []
        ranges = []
        for case in reconstructed[role]["cases"]:
            key = (case["case_id"], case["attempt"])
            ranges.append((case["begin_sequence"], case["end_sequence"] or (1 << 64)))
            if key in indexed[role] or case["case_id"] not in expected_ids:
                reconstructed[role]["issues"].append("duplicate_or_unknown_case:" + str(key))
                unmatched[role].append({k: v for k, v in case.items() if k not in ("events", "checkpoints", "anomalies")})
            else:
                indexed[role][key] = case
        outside_diagnostics[role] = [
            {"sequence": r["sequence"], "event": r["fields"].get("event", "recording_loss")}
            for r in package["records"] if r["kind"] in (11, 12) and
            not any(start <= r["sequence"] <= end for start, end in ranges)]
    complete = all(p["validation"].get("recording_complete") for p in (baseline, candidate))
    complete &= not any(value["issues"] for value in reconstructed.values())
    cases = []
    for spec in catalog["cases"]:
        keys = sorted({key for index in indexed.values() for key in index if key[0] == spec["id"]})
        if not keys:
            cases.append({"case_id": spec["id"], "title": spec["title"], "case_version": spec["version"],
                          "attempt": None, "outcome": "not_covered", "findings": ["case_not_performed"]})
        for key in keys:
            cases.append(compare_case(indexed["baseline"].get(key), indexed["candidate"].get(key),
                                      spec, issues, complete, certified_entries))
    precedence = ("incomplete", "incomparable", "confirmed_mismatch", "not_covered", "inconclusive",
                  "observed_difference", "observational_match", "same_input_match")
    outcomes = {case["outcome"] for case in cases}
    outcome = "incomplete" if not complete else "incomparable" if issues else \
        next((value for value in precedence if value in outcomes), "not_covered")
    if any(outside_diagnostics.values()) and outcome in ("observational_match", "same_input_match", "observed_difference"):
        outcome = "inconclusive"
    report = {"schema": SCHEMA, "tool_revision": comparison_revision(),
            "generated_at": datetime.now(timezone.utc).isoformat(), "outcome": outcome,
            "scope": "finite_cases_not_whole_game_acceptance",
            "case_catalog_sha256": canonical_hash(catalog), "compatibility_issues": issues,
            "recording_validation": {"baseline": baseline["validation"], "candidate": candidate["validation"]},
            "case_lifecycle_issues": {role: value["issues"] for role, value in reconstructed.items()},
            "unmatched_cases": unmatched,
            "outside_case_diagnostics": {role: rows[:128] for role, rows in outside_diagnostics.items()},
            "outside_case_diagnostics_omitted": {role: max(0, len(rows) - 128) for role, rows in outside_diagnostics.items()},
            "runs": {role: package["manifest"]["identity"] for role, package in (("baseline", baseline), ("candidate", candidate))},
            "counts": dict(Counter(case["outcome"] for case in cases)), "cases": cases,
            "limitations": ["Matching recorded inputs does not establish identical RNG, hidden state or scheduling.",
                            "Same-input matches refer to in-process helper reference checks, not identical manual sessions.",
                            "Periodic state and timing differences require investigation; neither automatically proves a regression.",
                            "Missing or incomplete evidence never counts as acceptance."]}
    if execution_profile is not None:
        report["native_execution"] = native_execution(report, execution_profile)
        report["limitations"].append("Observed native execution is not an in-process same-input verification or whole-game acceptance.")
        if execution_profile["schema"] == "yakumo-native-batch-v2":
            report["profile_observations"] = profile_observations(report, execution_profile, indexed, catalog)
            report["limitations"].append("Optional profile observations report scoped calls and instrumented durations only; they do not establish a speedup or human acceptance.")
    return report


def compare_runs(baseline: Path, candidate: Path, catalog: dict, execution_profile: dict | None = None) -> dict:
    return compare_loaded(load_package(baseline), load_package(candidate), catalog, execution_profile)


def render_html(report: dict) -> str:
    escape = lambda value: html.escape(str(value), quote=True)
    labels = {"confirmed_mismatch": "Confirmed reference mismatch", "observational_match": "Required observations collected",
              "observed_difference": "Differences need review", "incomparable": "Starting conditions or versions differ",
              "not_covered": "More coverage is needed", "incomplete": "Recording is incomplete",
              "inconclusive": "Needs review", "same_input_match": "Helper reference checks match"}
    explanations = {
        "recording_or_case_lifecycle_incomplete": "The recording or this case did not finish cleanly.",
        "certified_candidate_same_input_reference_check_failed": "A modified helper returned a different result from the original for the same input.",
        "required_checkpoints_missing_or_invalid": "One or more required checkpoints were not recorded correctly.",
        "user_marked_abnormal_uncertain_or_skipped": "The user marked a problem, uncertainty, or a skipped case.",
        "diagnostics_or_user_anomalies_require_review": "An error or user observation needs investigation.",
        "configuration_changed_during_case_requires_review": "Settings changed during this case. Their effect needs review before acceptance.",
        "periodic_state_difference_not_proof_of_regression": "State near a checkpoint differs; this alone does not prove a game bug.",
        "manual_input_streams_differ_review_without_assuming_regression": "Recorded actions differ between the two sessions and need review.",
        "user_marked_normal_without_observed_evidence": "The case was marked normal, but there is not enough recorded evidence to compare.",
        "required_observations_present_without_confirmed_regression": "Required observations are present. No confirmed regression was found in this case.",
        "case_not_performed": "This case was not performed.",
    }
    def explain(finding):
        if finding in explanations: return explanations[finding]
        if finding.startswith("state_unavailable:"): return "A required state sample was unavailable or too old near a checkpoint."
        if ":probe:" in finding: return "A required helper did not have enough complete, verified observations."
        if finding.startswith("case_missing_from_"): return "The matching case is missing from one application."
        return "A recorded condition needs review; details are included below."
    sections = []
    for case in report["cases"]:
        findings = "".join("<li>" + escape(item) + "</li>" for item in dict.fromkeys(explain(x) for x in case.get("findings", [])))
        details = escape(json.dumps(case, ensure_ascii=False, indent=2))
        sections.append(f'<section><h2>{escape(case["case_id"])} · {escape(case["title"])}</h2>'
                        f'<p class="status">{escape(labels.get(case["outcome"], "Needs review"))}</p><ul>{findings}</ul>'
                        f'<details><summary>Evidence and observations</summary><pre>{details}</pre></details></section>')
    execution_html = ""
    if "native_execution" in report:
        execution = report["native_execution"]
        execution_html = ('<section><h2>Requested native execution</h2><p>' +
                          escape(execution["outcome"]) +
                          '</p><p>This checks which implementation ran. It does not establish same-input equivalence or whole-game acceptance.</p>' +
                          '<details><summary>Mode and coverage evidence</summary><pre>' +
                          escape(json.dumps(execution, indent=2)) + '</pre></details></section>')
    observation_html = ""
    if "profile_observations" in report:
        observations = report["profile_observations"]
        rows = [(case, entry) for case in observations["cases"] for entry in case["entries"]]
        unperformed_rows = [(case, entry) for case in observations["unperformed_cases"]
                            for entry in case["entries"]]
        def window_text(window):
            counts = window["delta"]
            durations = window["duration_totals_ns"]
            count_text = ", ".join(f"{name}: {counts.get(name + '_calls', '?')}"
                                   for name in ("aot", "verify", "native", "fallback"))
            duration_text = ", ".join(f"{name}: {durations.get(name, '?')}"
                                      for name in ("aot", "verify", "native", "fallback"))
            return (window["status"] + "; " + count_text + "; duration " + window["duration_status"] +
                    "; total ns " + duration_text +
                    ("; " + ", ".join(window["issues"]) if window["issues"] else ""))
        table_rows = "".join(
            "<tr><td>" + escape(case["case_id"]) + " #" + escape(case["attempt"]) +
            "</td><td>" + escape(f"0x{entry['entry']:08X}") + " · " + escape(entry["candidate_mode"]) +
            "</td><td>" + escape(window_text(entry["baseline"])) +
            "</td><td>" + escape(window_text(entry["candidate"])) +
            "</td><td>" + escape(entry["coverage"]) +
            "; in-process Verify: " + escape(entry["candidate_reference_verification"]) +
            ("; " + escape(", ".join(entry["issues"])) if entry["issues"] else "") +
            "</td></tr>" for case, entry in rows[:96])
        remaining = max(0, 96 - len(rows))
        table_rows += "".join(
            "<tr><td>" + escape(case["case_id"]) + " · no attempt</td><td>" +
            escape(f"0x{entry['entry']:08X}") + " · " + escape(entry["candidate_mode"]) +
            "</td><td>not covered</td><td>not covered</td><td>case not performed</td></tr>"
            for case, entry in unperformed_rows[:remaining])
        omitted = max(0, len(rows) + len(unperformed_rows) - 96)
        observation_html = (
            '<section><h2>Optional helper observations</h2>'
            '<p>These case windows show certified calls and instrumented total durations. '
            'The applications were played separately; these figures do not measure a speedup or establish human acceptance. '
            'Only candidate Verify calls can support the separate in-process reference check.</p>'
            '<p>Profile or run issues: ' + escape(", ".join(observations["mode_issues"] +
                                                       observations["compatibility_issues"]) or "none") + '</p>'
            '<table><thead><tr><th>Case</th><th>Helper</th><th>Baseline</th><th>Candidate</th><th>Coverage</th>'
            '</tr></thead><tbody>' + table_rows + '</tbody></table>' +
            (f'<p>{omitted} additional helper windows are in report.json.</p>' if omitted else '') +
            ('<p>No case attempts were recorded.</p>' if not observations["cases"] else '') + '</section>')
    warnings = "".join("<li>" + escape(x) + "</li>" for x in report["limitations"])
    compatibility_text = escape(json.dumps(report["compatibility_issues"], indent=2))
    health = escape(json.dumps(report["recording_validation"], indent=2))
    run_findings = escape(json.dumps({"outside_case_diagnostics": report.get("outside_case_diagnostics", {}),
                                     "unmatched_cases": report.get("unmatched_cases", {}),
                                     "case_lifecycle_issues": report.get("case_lifecycle_issues", {})}, indent=2))
    return ('<!doctype html><html lang="en"><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width,initial-scale=1">'
            '<meta http-equiv="Content-Security-Policy" content="default-src \'none\'; style-src \'unsafe-inline\'">'
            '<title>Yakumo paired observation report</title><style>'
            'body{font:16px/1.55 system-ui;margin:0;background:#f4f6f8;color:#17212b}'
            'main{max-width:1000px;margin:auto;padding:32px}header,section{background:white;padding:24px;margin:16px 0;border-radius:12px}'
            'h1{margin-top:0}h2{font-size:20px}.status{font-weight:700}pre{overflow:auto;font-size:13px;white-space:pre-wrap;overflow-wrap:anywhere}'
            'summary{cursor:pointer}li{margin:6px 0}table{border-collapse:collapse;width:100%;font-size:13px}'
            'th,td{border:1px solid #dbe1e6;padding:8px;vertical-align:top;text-align:left;overflow-wrap:anywhere}'
            '</style><main><header><h1>Paired observation report</h1>'
            f'<p class="status">{escape(labels.get(report["outcome"], "Needs review"))}</p><p>Evidence for the listed cases; this does not establish that the whole game is correct.</p>'
            f'<p>{escape(report["generated_at"])}</p><ul>{warnings}</ul></header>'
            f'<section><h2>Recording and starting conditions</h2><p>{"Review is required before relying on the comparison." if report["compatibility_issues"] else "No starting-condition differences were found in the supplied identities."}</p>'
            f'<details><summary>Identity details</summary><pre>{compatibility_text}</pre></details><details><summary>Recording health</summary><pre>{health}</pre></details>'
            f'<details><summary>Run diagnostics and unmatched cases</summary><pre>{run_findings}</pre></details></section>'
            + execution_html + observation_html + "".join(sections) + '</main></html>')


def write_report(report: dict, output: Path) -> None:
    output = Path(output).absolute()
    for parent in (output, *output.parents):
        if parent.is_symlink():
            raise ValueError("report path cannot contain symlinks")
    documents = {"report.json": json.dumps(report, ensure_ascii=False, allow_nan=False, indent=2) + "\n",
                 "index.html": render_html(report)}
    output.mkdir()  # Exclusive creation; never modify an existing report.
    created = []
    try:
        for name, content in documents.items():
            with (output / name).open("x", encoding="utf-8") as stream:
                created.append(output / name)
                stream.write(content)
    except BaseException:
        for path in created:
            path.unlink(missing_ok=True)
        output.rmdir()
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--cases", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--execution-profile", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.cases.is_symlink() or not args.cases.is_file():
            raise ValueError("case catalog must be a bounded regular JSON file")
        catalog = load_case_catalog(args.cases)
        destination = args.output.resolve()
        for source in (args.baseline.resolve(), args.candidate.resolve()):
            if destination == source or source in destination.parents:
                raise ValueError("report output must be outside source packages")
        profile = load_profile(args.execution_profile, catalog) if args.execution_profile is not None else None
        report = compare_runs(args.baseline, args.candidate, catalog, profile)
        write_report(report, args.output)
        print(json.dumps({"outcome": report["outcome"], "counts": report["counts"]}))
        return 0  # Finding a mismatch is a successful analysis, not an I/O error.
    except (ValueError, OSError, KeyError, TypeError) as error:
        print("Comparison failed: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
