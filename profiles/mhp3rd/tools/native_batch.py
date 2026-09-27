"""Validate bounded, catalog-bound native execution profiles for paired batches."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
from typing import Any

if __package__:
    from . import native_modes, run_cases, run_package
else:
    import native_modes
    import run_cases
    import run_package


SCHEMA = "yakumo-native-batch-v1"
V2_SCHEMA = "yakumo-native-batch-v2"
MAX_PROFILE_BYTES = 64 * 1024
NATIVE_SWITCH_BY_ENTRY = native_modes.LEGACY_BY_ENTRY
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
    schema = value["schema"]
    if schema not in (SCHEMA, V2_SCHEMA):
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
    switches = set(native_modes.LEGACY_FIELDS if schema == SCHEMA else native_modes.V2_FIELDS)
    if type(modes) is not dict or set(modes) != switches:
        raise NativeBatchError("Candidate modes must name exactly the switches for the profile schema")
    if any(type(mode) is not str or mode not in _MODES for mode in modes.values()):
        raise NativeBatchError("Candidate modes must be off, verify, or native")
    if schema == SCHEMA:
        expected_modes = {name: ("native" if entry in REQUIRED_NATIVE_ENTRIES else "off")
                          for entry, name in NATIVE_SWITCH_BY_ENTRY.items()}
        if modes != expected_modes:
            raise NativeBatchError("Pilot candidate must enable only scale and matrix copy in native mode")
    else:
        if any(modes[name] != "off" for name in native_modes.LEGACY_FIELDS):
            raise NativeBatchError("Vector profile keeps the original five helpers off")
        if not any(modes[name] in ("verify", "native") for name in native_modes.VECTOR_FIELDS):
            raise NativeBatchError("Vector profile must request verify or native for a vector helper")
        expected_modes = dict(modes)

    entries = value["required_native_entries"]
    if type(entries) is not list or any(type(entry) is not int for entry in entries):
        raise NativeBatchError("Required native entries must be an integer list")
    if len(entries) != len(set(entries)):
        raise NativeBatchError("Required native entries contain duplicates")
    expected_entries = (list(REQUIRED_NATIVE_ENTRIES) if schema == SCHEMA else
                        sorted(entry for entry, name in native_modes.VECTOR_BY_ENTRY.items()
                               if modes[name] == "native"))
    if entries != expected_entries:
        raise NativeBatchError("Required native entries must match native-mode helpers in sorted order")
    observed = {probe["entry"] for case in normalized_catalog["cases"]
                for probe in case["required_probes"]}
    if schema == V2_SCHEMA:
        outside = observed - native_modes.entries(native_modes.V2_SCHEMA)
        if outside:
            raise NativeBatchError("Vector profile catalog requires probes outside the v2 native registry: " +
                                   ", ".join(f"0x{entry:08X}" for entry in sorted(outside)))
    missing = set(entries) - observed
    if missing:
        raise NativeBatchError("Case catalog lacks required native probe entries: " +
                               ", ".join(f"0x{entry:08X}" for entry in sorted(missing)))
    return {"schema": schema, "id": value["id"],
            "case_catalog_sha256": value["case_catalog_sha256"],
            "candidate_modes": dict(expected_modes),
            "required_native_entries": list(expected_entries)}


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
