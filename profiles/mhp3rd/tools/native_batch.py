"""Validate the bounded scale/copy native execution profile for a paired batch.

The profile declares evidence expectations, not arbitrary launcher environment.
Only the first controlled native pilot is accepted by this module.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
from typing import Any

if __package__:
    from . import run_cases, run_package
else:
    import run_cases
    import run_package


SCHEMA = "yakumo-native-batch-v1"
MAX_PROFILE_BYTES = 64 * 1024
NATIVE_SWITCH_BY_ENTRY = {
    0x088775AC: "MHP3RD_NATIVE_ANGLE_STEP",
    0x08877818: "MHP3RD_NATIVE_VECTOR_CONSTRUCT",
    0x08878B28: "MHP3RD_NATIVE_SCALE_MATRIX",
    0x08878B4C: "MHP3RD_NATIVE_TRANSLATION_MATRIX",
    0x08879D08: "MHP3RD_NATIVE_MATRIX_COPY",
}
REQUIRED_NATIVE_ENTRIES = (0x08878B28, 0x08879D08)
_ID = re.compile(r"[A-Za-z][A-Za-z0-9_.-]{0,95}\Z", re.ASCII)
_SHA256 = re.compile(r"[0-9a-f]{64}\Z", re.ASCII)
_FIELDS = frozenset(("schema", "id", "case_catalog_sha256", "candidate_modes",
                     "required_native_entries"))
_MODES = frozenset(("off", "verify", "native"))


class NativeBatchError(ValueError):
    """An execution profile is invalid or does not bind its case catalog."""


def _digest(value: dict[str, Any]) -> str:
    encoded = json.dumps(value, sort_keys=True, ensure_ascii=False,
                         separators=(",", ":"), allow_nan=False).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def profile_sha256(profile: dict[str, Any]) -> str:
    """Hash the normalized profile with the same canonical JSON as catalogs."""
    return _digest(profile)


def validate_profile(value: Any, catalog: dict[str, Any]) -> dict[str, Any]:
    """Return a copied pilot profile bound to a validated case catalog."""
    if type(value) is not dict or set(value) != _FIELDS:
        raise NativeBatchError("Execution profile has missing or unknown fields")
    if value["schema"] != SCHEMA:
        raise NativeBatchError("Unknown execution profile schema")
    if type(value["id"]) is not str or not _ID.fullmatch(value["id"]):
        raise NativeBatchError("Execution profile id must be an ASCII-safe ID")
    if type(value["case_catalog_sha256"]) is not str or not _SHA256.fullmatch(value["case_catalog_sha256"]):
        raise NativeBatchError("Execution profile catalog hash must be lowercase SHA-256")
    try:
        normalized_catalog = run_cases.validate_case_catalog(catalog)
    except run_cases.CaseCatalogError as exc:
        raise NativeBatchError("Execution profile case catalog is invalid") from exc
    if value["case_catalog_sha256"] != _digest(normalized_catalog):
        raise NativeBatchError("Execution profile case catalog hash differs")

    modes = value["candidate_modes"]
    switches = set(NATIVE_SWITCH_BY_ENTRY.values())
    if type(modes) is not dict or set(modes) != switches:
        raise NativeBatchError("Candidate modes must name exactly the five native switches")
    if any(type(mode) is not str or mode not in _MODES for mode in modes.values()):
        raise NativeBatchError("Candidate modes must be off, verify, or native")
    expected_modes = {name: ("native" if entry in REQUIRED_NATIVE_ENTRIES else "off")
                      for entry, name in NATIVE_SWITCH_BY_ENTRY.items()}
    if modes != expected_modes:
        raise NativeBatchError("Pilot candidate must enable only scale and matrix copy in native mode")

    entries = value["required_native_entries"]
    if type(entries) is not list or any(type(entry) is not int for entry in entries):
        raise NativeBatchError("Required native entries must be an integer list")
    if len(entries) != len(set(entries)):
        raise NativeBatchError("Required native entries contain duplicates")
    if entries != list(REQUIRED_NATIVE_ENTRIES):
        raise NativeBatchError("Pilot requires scale and matrix copy entries")
    observed = {probe["entry"] for case in normalized_catalog["cases"]
                for probe in case["required_probes"]}
    missing = set(entries) - observed
    if missing:
        raise NativeBatchError("Case catalog lacks required native probe entries: " +
                               ", ".join(f"0x{entry:08X}" for entry in sorted(missing)))
    return {"schema": SCHEMA, "id": value["id"],
            "case_catalog_sha256": value["case_catalog_sha256"],
            "candidate_modes": dict(expected_modes),
            "required_native_entries": list(REQUIRED_NATIVE_ENTRIES)}


def load_profile(path: Path, catalog: dict[str, Any]) -> dict[str, Any]:
    """Read a bounded UTF-8 JSON profile, rejecting duplicate keys."""
    try:
        raw = run_package._read_file(Path(path), MAX_PROFILE_BYTES)
    except run_package.PackageError as exc:
        raise NativeBatchError(str(exc)) from exc
    try:
        value = json.loads(raw.decode("utf-8"),
                           object_pairs_hook=run_cases._unique_json_object,
                           parse_constant=run_cases._reject_json_constant)
    except run_cases.CaseCatalogError as exc:
        raise NativeBatchError(str(exc)) from exc
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError) as exc:
        raise NativeBatchError("Invalid UTF-8 JSON execution profile") from exc
    return validate_profile(value, catalog)
