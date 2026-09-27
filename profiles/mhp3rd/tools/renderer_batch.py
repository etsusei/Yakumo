"""Catalog-bound renderer policy and conservative run-total decode evidence.

This policy is deliberately separate from native PSP helper profiles. A clean
renderer result describes cache-miss decodes in one recorded run, gated by the
paired human cases; it does not establish identical routes or presentation.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
from typing import Any

if __package__:
    from . import run_cases, run_package, texture_decode_policy
else:
    import run_cases
    import run_package
    import texture_decode_policy


SCHEMA = "yakumo-renderer-batch-v1"
MAX_PROFILE_BYTES = 64 * 1024
COUNTER_SCOPE = "renderer_cache_miss_decodes_not_all_draws"
COUNTER_EVENT = "texture_decode.counters"
COUNTERS = (
    "requests", "immediate_requests", "async_requests", "async_capture_attempts",
    "snapshot_rejected", "unsupported_state", "portable_success", "verified",
    "native", "fallbacks", "mismatches", "errors", "legacy_failures",
    "portable_elapsed_ns", "reference_elapsed_ns",
)
_FIELDS = frozenset(("schema", "id", "case_catalog_sha256", "candidate_mode",
                     "minimum_decodes", "coverage_scope"))
_ID = re.compile(r"[A-Za-z][A-Za-z0-9_.-]{0,95}\Z", re.ASCII)
_SHA256 = re.compile(r"[0-9a-f]{64}\Z", re.ASCII)
_PROBE_NOT_COVERED = re.compile(r"(?:baseline|candidate):probe:[0-9]+:not_covered\Z")
_UINT64 = 1 << 64
_ROLES = ("baseline", "candidate")


class RendererBatchError(ValueError):
    """A renderer profile is invalid or does not bind its case catalog."""


def _digest(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, ensure_ascii=False,
                         separators=(",", ":"), allow_nan=False).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def profile_sha256(profile: dict[str, Any]) -> str:
    """Hash canonical UTF-8 JSON, matching the case and native profile tools."""
    return _digest(profile)


def _shape(value: Any) -> dict[str, Any]:
    if type(value) is not dict or set(value) != _FIELDS:
        raise RendererBatchError("Renderer profile has missing or unknown fields")
    if value["schema"] != SCHEMA:
        raise RendererBatchError("Unknown renderer profile schema")
    if type(value["id"]) is not str or _ID.fullmatch(value["id"]) is None:
        raise RendererBatchError("Renderer profile id must be an ASCII-safe ID")
    catalog_hash = value["case_catalog_sha256"]
    if type(catalog_hash) is not str or _SHA256.fullmatch(catalog_hash) is None:
        raise RendererBatchError("Renderer profile catalog hash must be lowercase SHA-256")
    if type(value["candidate_mode"]) is not str or value["candidate_mode"] not in ("verify", "native"):
        raise RendererBatchError("Renderer candidate mode must be verify or native")
    minimum = value["minimum_decodes"]
    if type(minimum) is not int or not 1 <= minimum <= 1_000_000:
        raise RendererBatchError("Renderer minimum decodes must be an integer in 1..1000000")
    if value["coverage_scope"] != "run_total":
        raise RendererBatchError("Renderer coverage scope must be run_total")
    return {"schema": SCHEMA, "id": value["id"],
            "case_catalog_sha256": catalog_hash,
            "candidate_mode": value["candidate_mode"],
            "minimum_decodes": minimum, "coverage_scope": "run_total"}


def validate_profile(value: Any, catalog: dict[str, Any]) -> dict[str, Any]:
    """Return a copied profile bound to a validated finite case catalog."""
    normalized = _shape(value)
    try:
        normalized_catalog = run_cases.validate_case_catalog(catalog)
    except run_cases.CaseCatalogError as exc:
        raise RendererBatchError("Renderer profile case catalog is invalid") from exc
    if normalized["case_catalog_sha256"] != _digest(normalized_catalog):
        raise RendererBatchError("Renderer profile case catalog hash differs")
    return normalized


def load_profile(path: Path, catalog: dict[str, Any]) -> dict[str, Any]:
    """Read bounded UTF-8 JSON, rejecting duplicate keys and unsafe paths."""
    try:
        raw = run_package._read_file(Path(path), MAX_PROFILE_BYTES)
    except run_package.PackageError as exc:
        raise RendererBatchError(str(exc)) from exc
    try:
        value = json.loads(raw.decode("utf-8"),
                           object_pairs_hook=run_cases._unique_json_object,
                           parse_constant=run_cases._reject_json_constant)
    except run_cases.CaseCatalogError as exc:
        raise RendererBatchError(str(exc)) from exc
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError) as exc:
        raise RendererBatchError("Invalid UTF-8 JSON renderer profile") from exc
    return validate_profile(value, catalog)


def _uint64(value: Any) -> bool:
    return type(value) is int and 0 <= value < _UINT64


def _list_has_items(value: Any) -> bool:
    return type(value) is list and bool(value)


def _identity_issues(report: dict, packages: dict[str, dict], profile: dict) -> list[str]:
    issues: list[str] = []
    expected_hash = profile_sha256(profile)
    if report.get("case_catalog_sha256") != profile["case_catalog_sha256"]:
        issues.append("report:case_catalog_differs_from_profile")
    if _list_has_items(report.get("compatibility_issues")) or type(report.get("compatibility_issues")) is not list:
        issues.append("report:pair_identity_or_context_incompatible")
    helper_sections = []
    native_execution = report.get("native_execution")
    if native_execution is not None:
        if type(native_execution) is not dict or type(native_execution.get("issues")) is not list:
            issues.append("report:native_execution_identity_missing")
        else:
            helper_sections.append(native_execution)
            if native_execution.get("outcome") == "incomparable":
                issues.append("report:native_execution_identity_incomparable")
            if any(type(item) is str and ("execution_modes_differ_from_profile" in item or
                                          "execution_mode_schema_differs_from_profile" in item)
                   for item in native_execution["issues"]):
                issues.append("report:native_execution_mode_identity_differs")
    observations = report.get("profile_observations")
    if observations is not None:
        if type(observations) is not dict or type(observations.get("mode_issues")) is not list:
            issues.append("report:profile_observations_identity_missing")
        else:
            helper_sections.append(observations)
            if observations["mode_issues"]:
                issues.append("report:profile_observations_mode_identity_differs")
    for section in helper_sections:
        if type(section.get("profile_id")) is not str or \
                type(section.get("profile_sha256")) is not str or \
                _SHA256.fullmatch(section["profile_sha256"]) is None:
            issues.append("report:helper_profile_identity_malformed")
    if len(helper_sections) == 2 and any(helper_sections[0].get(key) != helper_sections[1].get(key)
                                         for key in ("profile_id", "profile_sha256")):
        issues.append("report:helper_profile_identities_differ")
    runs = report.get("runs")
    recording = report.get("recording_validation")
    if type(runs) is not dict or type(recording) is not dict:
        return issues + ["report:missing_run_or_recording_identity"]
    for role in _ROLES:
        package = packages[role]
        manifest = package.get("manifest")
        identity = manifest.get("identity") if type(manifest) is dict else None
        context = manifest.get("context") if type(manifest) is dict else None
        validation = package.get("validation")
        if type(identity) is not dict or type(context) is not dict or type(validation) is not dict:
            issues.append(role + ":missing_package_identity_context_or_validation")
            continue
        if runs.get(role) != identity or recording.get(role) != validation:
            issues.append(role + ":report_package_binding_differs")
        if identity.get("role") != role:
            issues.append(role + ":wrong_role")
        if identity.get("texture_decode_schema") != texture_decode_policy.SCHEMA:
            issues.append(role + ":texture_decode_schema_differs_from_profile")
        expected_mode = "off" if role == "baseline" else profile["candidate_mode"]
        if identity.get("texture_decode_mode") != expected_mode:
            issues.append(role + ":texture_decode_mode_differs_from_profile")
        actual_hash = identity.get("renderer_profile_sha256")
        if type(actual_hash) is not str or _SHA256.fullmatch(actual_hash) is None or actual_hash != expected_hash:
            issues.append(role + ":renderer_profile_sha256_differs")
        if identity.get("case_catalog_sha256") != profile["case_catalog_sha256"] or \
                context.get("case_catalog_sha256") != profile["case_catalog_sha256"]:
            issues.append(role + ":case_catalog_differs_from_profile")
        if validation.get("identity_binding") != "bound" or validation.get("metadata_complete") is not True:
            issues.append(role + ":identity_unbound_or_incomplete")
    return issues


def _counter_stream(records: Any, mode: str) -> dict[str, Any]:
    result = {"events": 0, "final": None, "last": None, "issues": [],
              "mismatch_seen": False}
    if type(records) is not list:
        result["issues"].append("journal_records_missing")
        return result
    rows = []
    for row in records:
        if type(row) is not dict:
            result["issues"].append("malformed_journal_record")
            continue
        fields = row.get("fields")
        if type(fields) is dict and fields.get("event") == COUNTER_EVENT:
            rows.append(row)
    result["events"] = len(rows)
    if mode == "off":
        if rows:
            result["issues"].append("off_role_emitted_active_counters")
        return result
    if not rows:
        result["issues"].append("active_counters_missing")
        return result

    previous: dict[str, int] | None = None
    previous_sequence = 0
    final_count = 0
    for row in rows:
        fields = row["fields"]
        sequence = row.get("sequence")
        if row.get("kind") != 8 or not _uint64(sequence) or sequence <= previous_sequence:
            result["issues"].append("counter_kind_or_sequence_invalid")
        else:
            previous_sequence = sequence
        if fields.get("schema") != texture_decode_policy.SCHEMA or \
                fields.get("mode") != mode or fields.get("scope") != COUNTER_SCOPE:
            result["issues"].append("counter_schema_mode_or_scope_invalid")
        final = fields.get("final")
        drained = fields.get("workers_drained")
        if type(final) is not bool or type(drained) is not bool or drained != final:
            result["issues"].append("counter_final_or_drain_flag_invalid")
        elif final:
            final_count += 1
        counters = {name: fields.get(name) for name in COUNTERS}
        if any(not _uint64(value) for value in counters.values()):
            result["issues"].append("counter_field_missing_or_not_uint64")
            continue
        if previous is not None and any(counters[name] < previous[name] for name in COUNTERS):
            result["issues"].append("cumulative_counter_reset")
        previous = counters
        result["last"] = counters
        if final is True:
            result["final"] = counters
        if counters["mismatches"] > 0:
            result["mismatch_seen"] = True
    if final_count != 1:
        result["issues"].append("exactly_one_final_counter_required")
    elif rows[-1]["fields"].get("final") is not True:
        result["issues"].append("final_counter_must_be_last")

    final = result["final"]
    if final is not None:
        if final["requests"] != final["immediate_requests"] + final["async_requests"]:
            result["issues"].append("final_request_partition_differs")
        if final["portable_success"] > final["requests"] or \
                final["verified"] > final["portable_success"] or \
                final["native"] > final["portable_success"] or \
                final["mismatches"] > final["verified"] or \
                final["fallbacks"] > final["requests"] or \
                final["unsupported_state"] > final["fallbacks"] or \
                final["snapshot_rejected"] > final["async_capture_attempts"] + final["immediate_requests"] or \
                final["async_requests"] > final["async_capture_attempts"]:
            result["issues"].append("final_success_partition_invalid")
        if (mode == "verify" and final["native"] != 0) or \
                (mode == "native" and final["verified"] != 0):
            result["issues"].append("opposite_execution_path_observed")
    # Intermediate atomic snapshots can contain in-flight work. Only the
    # drained final sample is checked for partition equality.
    result["issues"] = list(dict.fromkeys(result["issues"]))
    return result


def _case_issues(case: Any, packages: dict[str, dict]) -> tuple[str, list[str]]:
    if type(case) is not dict:
        return "incomplete", ["malformed_case_summary"]
    if case.get("outcome") == "incomplete":
        return "incomplete", ["case_lifecycle_or_checkpoints_incomplete"]
    if case.get("outcome") == "incomparable":
        return "incomparable", ["case_prerequisites_or_version_differ"]
    if case.get("outcome") == "confirmed_mismatch":
        return "needs_review", ["separate_helper_same_input_mismatch"]
    if case.get("findings") == ["case_not_performed"]:
        return "not_covered", ["human_case_not_performed"]

    issues: list[str] = []
    if case.get("outcome") not in ("observational_match", "observed_difference",
                                   "not_covered", "inconclusive"):
        issues.append("unknown_case_outcome")
    if case.get("human_acceptance_required") is not True:
        issues.append("human_acceptance_not_required")
    if case.get("user_outcomes") != {"baseline": "normal", "candidate": "normal"}:
        issues.append("paired_normal_user_outcomes_missing")
    if type(case.get("reference_verification")) is not dict:
        issues.append("checkpoint_completion_not_established")
    configuration = case.get("configuration_events")
    if type(configuration) is not dict or any(configuration.get(role) for role in _ROLES):
        issues.append("configuration_changed_during_case")
    diagnostics = case.get("diagnostics")
    omitted = case.get("diagnostics_omitted")
    if type(diagnostics) is not dict or type(omitted) is not dict or \
            any(diagnostics.get(role) or omitted.get(role) for role in _ROLES):
        issues.append("case_diagnostics_require_review")
    if type(case.get("state_differences")) is not list or case["state_differences"]:
        issues.append("checkpoint_state_differs")
    findings = case.get("findings")
    allowed = {"required_observations_present_without_confirmed_regression",
               "manual_input_streams_differ_review_without_assuming_regression",
               "user_marked_normal_without_observed_evidence"}
    if type(findings) is not list or any(type(item) is not str or
                                         (item not in allowed and _PROBE_NOT_COVERED.fullmatch(item) is None)
                                         for item in findings):
        issues.append("case_findings_require_review")

    ranges = case.get("sequence_ranges")
    if type(ranges) is not dict:
        return "incomplete", issues + ["paired_case_sequence_ranges_missing"]
    prerequisites = []
    for role in _ROLES:
        pair = ranges.get(role)
        if type(pair) is not list or len(pair) != 2 or not all(_uint64(x) and x > 0 for x in pair) or pair[0] >= pair[1]:
            return "incomplete", issues + [role + ":case_sequence_range_invalid"]
        records = packages[role].get("records")
        if type(records) is not list:
            return "incomplete", issues + [role + ":journal_records_missing"]
        by_sequence = {row["sequence"]: row for row in records if type(row) is dict and
                       _uint64(row.get("sequence"))}
        begin, end = by_sequence.get(pair[0]), by_sequence.get(pair[1])
        if type(begin) is not dict or type(end) is not dict or begin.get("kind") != 3 or end.get("kind") != 4:
            return "incomplete", issues + [role + ":case_boundary_records_missing"]
        expected = (case.get("case_id"), case.get("case_version"), case.get("attempt"))
        for marker in (begin, end):
            fields = marker.get("fields")
            if type(fields) is not dict or \
                    (fields.get("case_id"), fields.get("case_version"), fields.get("attempt")) != expected:
                return "incomplete", issues + [role + ":case_marker_identity_differs"]
        if end["fields"].get("outcome") != "normal":
            issues.append(role + ":case_end_not_normal")
        prerequisite = begin["fields"].get("prerequisites_sha256")
        if type(prerequisite) is not str or _SHA256.fullmatch(prerequisite) is None:
            return "incomplete", issues + [role + ":case_prerequisites_missing"]
        prerequisites.append(prerequisite)
        scoped = [row for row in records if type(row) is dict and _uint64(row.get("sequence")) and
                  pair[0] < row["sequence"] < pair[1]]
        checkpoints = [row for row in scoped if row.get("kind") == 5 and
                       type(row.get("fields")) is dict and
                       (row["fields"].get("case_id"), row["fields"].get("case_version"),
                        row["fields"].get("attempt")) == expected]
        if not checkpoints:
            issues.append(role + ":no_case_checkpoint_observed")
        if any(row.get("kind") in (6, 11, 12) for row in scoped):
            issues.append(role + ":case_anomaly_error_or_loss_observed")
        if any(row.get("kind") == 8 and type(row.get("fields")) is dict and
               row["fields"].get("event") == "config.effective" for row in scoped):
            issues.append(role + ":configuration_changed_during_case")
    if prerequisites[0] != prerequisites[1]:
        return "incomparable", issues + ["case_prerequisites_differ"]
    return ("eligible" if not issues else "needs_review"), issues


def analyze_renderer(report: dict, baseline_package: dict, candidate_package: dict,
                     profile: dict) -> dict:
    """Classify one paired run's renderer decode evidence.

    The caller supplies the existing package comparison and its two verified
    loaded packages. Counter coverage is the full candidate run, including
    startup before the first case; human cases gate interpretation rather than
    assigning individual decodes to a case.
    """
    profile = _shape(profile)
    packages = {"baseline": baseline_package, "candidate": candidate_package}
    result = {
        "profile_id": profile["id"], "profile_sha256": profile_sha256(profile),
        "candidate_mode": profile["candidate_mode"],
        "minimum_decodes": profile["minimum_decodes"],
        "coverage_scope": "run_total", "counter_scope": COUNTER_SCOPE,
        "outcome": "incomplete", "issues": [], "candidate_counters": None,
        "candidate_last_counters": None, "partial_mismatch_observed": False,
        "counter_events": {"baseline": 0, "candidate": 0}, "cases": [],
        "limitations": [
            "Run-total counters include startup before the case and are not case-specific draw, UV, format, or hardware coverage.",
            "Verification compares pixels inside the candidate process; native execution alone is not same-input equivalence.",
            "Decoder CPU durations are instrumented totals, not proof of gameplay speedup or presentation quality.",
            "Matched human cases and observations do not establish whole-game acceptance.",
        ],
    }
    if type(report) is not dict or any(type(package) is not dict for package in packages.values()):
        result["outcome"] = "incomparable"
        result["issues"].append("report_or_package_missing")
        return result
    identity_issues = _identity_issues(report, packages, profile)
    if identity_issues:
        result["outcome"] = "incomparable"
        result["issues"].extend(identity_issues)
        return result

    baseline = _counter_stream(packages["baseline"].get("records"), "off")
    candidate = _counter_stream(packages["candidate"].get("records"), profile["candidate_mode"])
    result["counter_events"] = {"baseline": baseline["events"], "candidate": candidate["events"]}
    result["candidate_counters"] = candidate["final"]
    result["candidate_last_counters"] = candidate["last"]
    result["partial_mismatch_observed"] = candidate["mismatch_seen"]
    if baseline["issues"]:
        result["outcome"] = "incomparable"
        result["issues"].extend("baseline:" + issue for issue in baseline["issues"])
        return result
    if candidate["issues"]:
        result["issues"].extend("candidate:" + issue for issue in candidate["issues"])
        if "counter_schema_mode_or_scope_invalid" in candidate["issues"]:
            result["outcome"] = "incomparable"
        return result
    final = candidate["final"]
    assert final is not None
    for role in _ROLES:
        if packages[role]["validation"].get("recording_complete") is not True:
            result["issues"].append(role + ":recording_incomplete")
    if result["issues"]:
        return result
    if final["mismatches"] > 0:
        result["outcome"] = "confirmed_mismatch"
        result["issues"].append("candidate:portable_reference_pixel_mismatch")
        return result

    lifecycle = report.get("case_lifecycle_issues")
    unmatched = report.get("unmatched_cases")
    if type(lifecycle) is not dict or type(unmatched) is not dict or \
            any(lifecycle.get(role) or unmatched.get(role) for role in _ROLES):
        result["issues"].append("case_lifecycle_or_unmatched_case_requires_review")
        return result
    outside = report.get("outside_case_diagnostics")
    omitted = report.get("outside_case_diagnostics_omitted")
    if type(outside) is not dict or type(omitted) is not dict or \
            any(outside.get(role) or omitted.get(role) for role in _ROLES):
        result["outcome"] = "needs_review"
        result["issues"].append("outside_case_diagnostics_require_review")
        return result

    summaries = report.get("cases")
    if type(summaries) is not list:
        result["issues"].append("case_summaries_missing")
        return result
    if not summaries:
        result["outcome"] = "not_covered"
        result["issues"].append("human_cases_missing")
        return result
    statuses = []
    for case in summaries:
        status, issues = _case_issues(case, packages)
        result["cases"].append({"case_id": case.get("case_id") if type(case) is dict else None,
                                "attempt": case.get("attempt") if type(case) is dict else None,
                                "outcome": status, "issues": issues})
        statuses.append(status)
    if "incomparable" in statuses:
        result["outcome"] = "incomparable"
        result["issues"].append("paired_case_prerequisites_or_version_differ")
        return result
    if "incomplete" in statuses:
        result["issues"].append("paired_case_incomplete")
        return result
    if "not_covered" in statuses:
        result["outcome"] = "not_covered"
        result["issues"].append("paired_human_case_not_covered")
        return result
    if "needs_review" in statuses:
        result["outcome"] = "needs_review"
        result["issues"].append("paired_human_case_requires_review")
        return result

    negative = [name for name in ("fallbacks", "errors", "legacy_failures", "unsupported_state",
                                  "snapshot_rejected") if final[name] > 0]
    if negative:
        result["outcome"] = "needs_review"
        result["issues"].extend("candidate:" + name + "_observed" for name in negative)
        return result
    expected_path = "verified" if profile["candidate_mode"] == "verify" else "native"
    if final["requests"] != final["portable_success"] or \
            final["requests"] != final[expected_path]:
        result["issues"].append("candidate:final_clean_execution_partition_differs")
        return result
    if final["requests"] < profile["minimum_decodes"]:
        result["outcome"] = "not_covered"
        result["issues"].append("candidate:minimum_decodes_not_met")
        return result
    result["outcome"] = "verified" if profile["candidate_mode"] == "verify" else "native_observed"
    return result
