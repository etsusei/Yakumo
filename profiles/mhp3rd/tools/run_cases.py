"""Validate finite test cases and reconstruct their observed journal segments.

The journal reader owns framing and recovery. This module interprets case
markers only; a completed case is not a claim of deterministic replay or of
human acceptance.
"""

from __future__ import annotations

import json
import re
import unicodedata
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


CATALOG_SCHEMA = "yakumo-case-catalog-v1"
MAX_CATALOG_BYTES = 512 * 1024
MAX_CASES = 128
MAX_STEPS = 128
MAX_CHECKPOINTS = 128
MAX_PROBES = 32
MAX_STATE_FIELDS = 32
MAX_UINT32 = (1 << 32) - 1
MAX_UINT64 = (1 << 64) - 1

CASE_BEGIN = 3
CASE_END = 4
CHECKPOINT = 5
ANOMALY = 6
RUN_END = 2
RECORDING_LOSS = 12

_ID = re.compile(r"[A-Za-z][A-Za-z0-9_.-]{0,95}\Z", re.ASCII)
_SHA256 = re.compile(r"[0-9a-f]{64}\Z", re.ASCII)
_OUTCOMES = frozenset(("normal", "abnormal", "uncertain", "skipped"))
_CASE_KEYS = frozenset((
    "id", "version", "title", "steps", "checkpoints", "required_probes",
    "required_state_fields", "human_acceptance",
))


class CaseCatalogError(ValueError):
    """A case catalog cannot be used for evidence interpretation."""


def _keys(value: Any, required: frozenset[str], location: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise CaseCatalogError(f"{location} must be an object")
    missing = required - value.keys()
    extra = value.keys() - required
    if missing or extra:
        raise CaseCatalogError(
            f"{location} has missing keys {sorted(missing)} or unknown keys {sorted(map(str, extra))}"
        )
    return value


def _list(value: Any, maximum: int, location: str) -> list[Any]:
    if type(value) is not list or len(value) > maximum:
        raise CaseCatalogError(f"{location} must be a list of at most {maximum} items")
    return value


def _text(value: Any, maximum_bytes: int, location: str) -> str:
    if type(value) is not str or not value or any(unicodedata.category(char) == "Cc" for char in value):
        raise CaseCatalogError(f"{location} must be nonempty text without control characters")
    try:
        length = len(value.encode("utf-8"))
    except UnicodeEncodeError as exc:
        raise CaseCatalogError(f"{location} must be valid UTF-8 text") from exc
    if length > maximum_bytes:
        raise CaseCatalogError(f"{location} exceeds {maximum_bytes} UTF-8 bytes")
    return value


def _id(value: Any, location: str) -> str:
    if type(value) is not str or _ID.fullmatch(value) is None:
        raise CaseCatalogError(f"{location} must be an ASCII-safe ID of at most 96 characters")
    return value


def _positive(value: Any, maximum: int, location: str) -> int:
    if type(value) is not int or not 1 <= value <= maximum:
        raise CaseCatalogError(f"{location} must be an integer in 1..{maximum}")
    return value


def validate_case_catalog(data: dict[str, Any]) -> dict[str, Any]:
    """Return a copied, bounded catalog, rejecting ambiguous or misspelled data."""
    root = _keys(data, frozenset(("schema", "cases")), "catalog")
    if root["schema"] != CATALOG_SCHEMA:
        raise CaseCatalogError(f"catalog.schema must be {CATALOG_SCHEMA}")
    cases = []
    case_ids: set[str] = set()
    for index, raw in enumerate(_list(root["cases"], MAX_CASES, "catalog.cases")):
        location = f"catalog.cases[{index}]"
        item = _keys(raw, _CASE_KEYS, location)
        case_id = _id(item["id"], f"{location}.id")
        if case_id in case_ids:
            raise CaseCatalogError(f"duplicate case ID: {case_id}")
        case_ids.add(case_id)
        version = _positive(item["version"], MAX_UINT32, f"{location}.version")
        title = _text(item["title"], 256, f"{location}.title")
        steps = [
            _text(value, 512, f"{location}.steps[{step}]")
            for step, value in enumerate(_list(item["steps"], MAX_STEPS, f"{location}.steps"))
        ]
        checkpoints = [
            _id(value, f"{location}.checkpoints[{checkpoint}]")
            for checkpoint, value in enumerate(
                _list(item["checkpoints"], MAX_CHECKPOINTS, f"{location}.checkpoints")
            )
        ]
        if len(set(checkpoints)) != len(checkpoints):
            raise CaseCatalogError(f"{location}.checkpoints contains duplicate IDs")
        probes = []
        entries: set[int] = set()
        for probe_index, raw_probe in enumerate(
            _list(item["required_probes"], MAX_PROBES, f"{location}.required_probes")
        ):
            probe_location = f"{location}.required_probes[{probe_index}]"
            probe = _keys(raw_probe, frozenset(("entry", "min_calls")), probe_location)
            entry = probe["entry"]
            if type(entry) is not int or not 0 <= entry <= MAX_UINT32:
                raise CaseCatalogError(f"{probe_location}.entry must be a uint32")
            if entry in entries:
                raise CaseCatalogError(f"{location}.required_probes contains duplicate entries")
            entries.add(entry)
            probes.append({
                "entry": entry,
                "min_calls": _positive(probe["min_calls"], MAX_UINT64, f"{probe_location}.min_calls"),
            })
        state_fields = [
            _text(value, 128, f"{location}.required_state_fields[{state}]")
            for state, value in enumerate(_list(
                item["required_state_fields"], MAX_STATE_FIELDS, f"{location}.required_state_fields"
            ))
        ]
        if len(set(state_fields)) != len(state_fields):
            raise CaseCatalogError(f"{location}.required_state_fields contains duplicate names")
        if type(item["human_acceptance"]) is not bool:
            raise CaseCatalogError(f"{location}.human_acceptance must be a Boolean")
        cases.append({
            "id": case_id,
            "version": version,
            "title": title,
            "steps": steps,
            "checkpoints": checkpoints,
            "required_probes": probes,
            "required_state_fields": state_fields,
            "human_acceptance": item["human_acceptance"],
        })
    return {"schema": CATALOG_SCHEMA, "cases": cases}


def _unique_json_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result = {}
    for key, value in pairs:
        if key in result:
            raise CaseCatalogError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _reject_json_constant(value: str) -> Any:
    raise CaseCatalogError(f"nonstandard JSON number: {value}")


def load_case_catalog(path: Path) -> dict[str, Any]:
    """Read a small JSON catalog without silently accepting duplicate keys."""
    with Path(path).open("rb") as stream:
        raw = stream.read(MAX_CATALOG_BYTES + 1)
    if len(raw) > MAX_CATALOG_BYTES:
        raise CaseCatalogError(f"case catalog exceeds {MAX_CATALOG_BYTES} bytes")
    try:
        data = json.loads(
            raw.decode("utf-8"), object_pairs_hook=_unique_json_object,
            parse_constant=_reject_json_constant,
        )
    except CaseCatalogError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError) as exc:
        raise CaseCatalogError("invalid UTF-8 JSON case catalog") from exc
    return validate_case_catalog(data)


def _event_identity(fields: Any) -> tuple[tuple[str, int, int] | None, list[str]]:
    if not isinstance(fields, dict):
        return None, ["fields"]
    invalid = []
    case_id = fields.get("case_id")
    version = fields.get("case_version")
    attempt = fields.get("attempt")
    if type(case_id) is not str or _ID.fullmatch(case_id) is None:
        invalid.append("case_id")
    if type(version) is not int or not 1 <= version <= MAX_UINT32:
        invalid.append("case_version")
    if type(attempt) is not int or not 1 <= attempt <= MAX_UINT64:
        invalid.append("attempt")
    if invalid:
        return None, invalid
    return (case_id, version, attempt), []


def _append_issue(case: dict[str, Any], issue: str) -> None:
    case["issues"].append(issue)
    case["complete"] = False


@dataclass
class _ActiveCase:
    result: dict[str, Any]
    expected: list[str]
    seen: set[str] = field(default_factory=set)
    cursor: int = 0
    boundary_valid: bool = True

    @property
    def identity(self) -> tuple[str, int, int]:
        return self.result["case_id"], self.result["case_version"], self.result["attempt"]

    def add_record(self, record: dict[str, Any]) -> None:
        self.result["events"].append(record)

    def finish(self, end: dict[str, Any] | None = None, *, matching_end: bool = False) -> None:
        if end is not None:
            self.result["end_sequence"] = end["sequence"]
        self.result["lifecycle_complete"] = matching_end and self.boundary_valid
        self.result["missing_checkpoints"] = [name for name in self.expected if name not in self.seen]
        self.result["complete"] = (
            self.result["lifecycle_complete"]
            and not self.result["issues"]
            and not self.result["missing_checkpoints"]
        )


def collect_cases(records: list[dict[str, Any]], catalog: dict[str, Any]) -> dict[str, Any]:
    """Interpret ordered records as nonoverlapping case attempts.

    Malformed case markers remain diagnostics; one bad marker must not hide a
    later valid attempt. Non-case records outside a case are normal setup data.
    """
    normalized = validate_case_catalog(catalog)
    specs = {case["id"]: case for case in normalized["cases"]}
    result: dict[str, Any] = {"cases": [], "issues": []}
    attempts: dict[tuple[str, int], dict[str, Any]] = {}
    active: _ActiveCase | None = None

    for record in records:
        if not isinstance(record, dict):
            raise ValueError("records must contain journal record objects")
        kind = record.get("kind")
        sequence = record.get("sequence")
        if type(kind) is not int or type(sequence) is not int or sequence < 1:
            raise ValueError("journal records need integer kind and positive sequence")
        fields = record.get("fields")

        if kind == CASE_BEGIN:
            overlap = active is not None
            if active is not None:
                _append_issue(active.result, f"sequence {sequence}: interrupted by another CaseBegin")
                active.boundary_valid = False
                active.finish()
                active = None
            identity, invalid = _event_identity(fields)
            if not isinstance(fields, dict) or type(fields.get("prerequisites_sha256")) is not str \
                    or _SHA256.fullmatch(fields["prerequisites_sha256"]) is None:
                invalid.append("prerequisites_sha256")
            if invalid:
                result["issues"].append(
                    f"sequence {sequence}: malformed CaseBegin ({', '.join(sorted(set(invalid)))})"
                )
                continue
            assert identity is not None
            case_id, version, attempt = identity
            spec = specs.get(case_id)
            case = {
                "case_id": case_id,
                "case_version": version,
                "attempt": attempt,
                "prerequisites_sha256": fields["prerequisites_sha256"],
                "begin_sequence": sequence,
                "end_sequence": None,
                "outcome": None,
                "lifecycle_complete": False,
                "complete": False,
                "missing_checkpoints": [],
                "issues": [],
                "events": [record],
                "checkpoints": [],
                "anomalies": [],
            }
            result["cases"].append(case)
            active = _ActiveCase(case, spec["checkpoints"] if spec else [])
            if overlap:
                _append_issue(case, f"sequence {sequence}: CaseBegin overlaps a previous attempt")
                active.boundary_valid = False
            if spec is None:
                _append_issue(case, f"sequence {sequence}: unknown case ID {case_id}")
            elif version != spec["version"]:
                _append_issue(case, f"sequence {sequence}: catalog version mismatch for {case_id}")
            attempt_key = case_id, attempt
            if attempt_key in attempts:
                _append_issue(case, f"sequence {sequence}: duplicate attempt {attempt} for {case_id}")
                _append_issue(attempts[attempt_key], f"sequence {sequence}: duplicate attempt {attempt} for {case_id}")
            else:
                attempts[attempt_key] = case
            continue

        if kind in (CASE_END, CHECKPOINT, ANOMALY):
            if active is None:
                result["issues"].append(f"sequence {sequence}: {kind} marker outside a case")
                continue
            active.add_record(record)
            identity, invalid = _event_identity(fields)
            if invalid:
                _append_issue(active.result, f"sequence {sequence}: malformed case marker ({', '.join(invalid)})")
            elif identity != active.identity:
                _append_issue(active.result, f"sequence {sequence}: case marker identity does not match CaseBegin")

            if kind == CHECKPOINT:
                active.result["checkpoints"].append(record)
                checkpoint_id = fields.get("checkpoint_id") if isinstance(fields, dict) else None
                if type(checkpoint_id) is not str or _ID.fullmatch(checkpoint_id) is None:
                    _append_issue(active.result, f"sequence {sequence}: malformed checkpoint_id")
                elif identity == active.identity:
                    if checkpoint_id not in active.expected:
                        _append_issue(active.result, f"sequence {sequence}: unknown checkpoint {checkpoint_id}")
                    elif checkpoint_id in active.seen:
                        _append_issue(active.result, f"sequence {sequence}: duplicate checkpoint {checkpoint_id}")
                    else:
                        if active.expected[active.cursor] != checkpoint_id:
                            _append_issue(active.result, f"sequence {sequence}: checkpoint out of order: {checkpoint_id}")
                        active.seen.add(checkpoint_id)
                        while active.cursor < len(active.expected) and active.expected[active.cursor] in active.seen:
                            active.cursor += 1
                continue

            if kind == ANOMALY:
                active.result["anomalies"].append(record)
                if isinstance(fields, dict) and "message" in fields:
                    message = fields["message"]
                    if type(message) is not str or len(message.encode("utf-8", errors="replace")) > 512:
                        _append_issue(active.result, f"sequence {sequence}: malformed anomaly message")
                continue

            outcome = fields.get("outcome") if isinstance(fields, dict) else None
            valid_outcome = type(outcome) is str and outcome in _OUTCOMES
            if not valid_outcome:
                _append_issue(active.result, f"sequence {sequence}: malformed case outcome")
            elif not invalid and identity == active.identity:
                active.result["outcome"] = outcome
            if invalid or identity != active.identity or not valid_outcome:
                active.boundary_valid = False
            active.finish(record, matching_end=True)
            active = None
            continue

        if active is not None:
            active.add_record(record)
            if kind == RECORDING_LOSS:
                _append_issue(active.result, f"sequence {sequence}: recording loss inside case")
            if kind == RUN_END:
                _append_issue(active.result, f"sequence {sequence}: run ended with an open case")
                active.finish()
                active = None

    if active is not None:
        _append_issue(active.result, "journal ended with an open case")
        active.finish()
    return result
