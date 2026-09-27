#!/usr/bin/env python3
"""Read a session journal and publish a bounded, local evidence package.

Only the fixed ``events.journal`` input is taken from a run directory. Context
and supervisor metadata are explicit, small JSON inputs supplied by the later
launcher; no game assets, saves, memory dumps, or arbitrary logs are collected.
"""

from __future__ import annotations

import argparse
from collections import Counter
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import sys
import tempfile
import zlib
from typing import Any, Dict, List, Optional, Tuple

if __package__:
    from . import native_modes
else:
    import native_modes


FILE_HEADER = struct.Struct("<8sHHI")
RECORD_HEADER = struct.Struct("<4sIQQHHI")
MAX_JOURNAL_BYTES = 256 * 1024 * 1024
MAX_RECORDS = 1_000_000
MAX_PAYLOAD_BYTES = 65_536
MAX_METADATA_BYTES = 65_536
MAX_DERIVED_BYTES = 512 * 1024 * 1024
PACKAGE_SCHEMA = "yakumo-run-package-v1"
CONTEXT_SCHEMA = "yakumo-run-context-v1"
SUPERVISOR_SCHEMA = "yakumo-supervisor-v1"
# profiles/mhp3rd/host/install/game_identity.hpp::kExecutableSha256
SUPPORTED_ELF_SHA256 = "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c"

KIND_NAMES = {
    1: "run_begin", 2: "run_end", 3: "case_begin", 4: "case_end",
    5: "checkpoint", 6: "anomaly", 7: "input", 8: "state",
    9: "probe", 10: "performance", 11: "error", 12: "recording_loss",
}
# Retained for callers that construct historical five-helper fixtures.
NATIVE_MODE_FIELDS = native_modes.LEGACY_FIELDS
IDENTITY_FIELDS = (
    "role", "run_id", "batch_id", "baseline_id", "baseline_commit",
    "recorder_revision", "observer_schema", "recording_mode", "binary_sha256",
)
OPTIONAL_IDENTITY_FIELDS = ("source_commit", "build_config", "build_config_sha256",
                            "case_catalog_sha256", "prerequisite_basis_sha256")
CONTEXT_FIELDS = (
    "run_id", "game_sha256", "elf_sha256", "overlay_sha256", "starting_save_sha256",
    "config_sha256", "build_config_sha256", "case_catalog_sha256",
    "source_commit", "platform_os", "platform_arch",
)
CONTEXT_HASH_FIELDS = frozenset({
    "game_sha256", "elf_sha256", "overlay_sha256", "starting_save_sha256",
    "config_sha256", "build_config_sha256", "case_catalog_sha256",
})
ARTIFACT_NAMES = (
    "events.journal", "events.jsonl", "statistics.json", "markers.json",
    "diagnostics.jsonl", "summary.json",
)
_HASH = re.compile(r"[0-9a-fA-F]{64}\Z")
_COMMIT = re.compile(r"[0-9a-fA-F]{7,40}\Z")
_SAFE_ID = re.compile(r"[A-Za-z0-9_.-]{1,96}\Z")


class PackageError(ValueError):
    """An input or package path/content cannot be safely used."""


def _canonical_json(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False, allow_nan=False).encode("utf-8")


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def prerequisite_basis_sha256(context: Dict[str, Any], baseline_id: str, baseline_commit: str) -> str:
    """Common launch conditions; role/run/source/binary identities are excluded."""
    keys = ("game_sha256", "elf_sha256", "overlay_sha256", "starting_save_sha256", "config_sha256",
            "build_config_sha256", "case_catalog_sha256", "platform_os", "platform_arch")
    if not _valid_id(baseline_id) or type(baseline_commit) is not str or not _COMMIT.fullmatch(baseline_commit):
        raise PackageError("invalid prerequisite baseline identity")
    basis = {"schema": "yakumo-case-basis-v1", "baseline_id": baseline_id, "baseline_commit": baseline_commit.lower()}
    for key in keys:
        value = context.get(key)
        if key.endswith("_sha256"):
            if not _valid_hash(value): raise PackageError("missing prerequisite identity: " + key)
            value = value.lower()
        elif type(value) is not str or not value:
            raise PackageError("missing prerequisite platform: " + key)
        basis[key] = value
    return _sha256(_canonical_json(basis))


def _unique_object(pairs: List[Tuple[str, Any]]) -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    for key, value in pairs:
        if not key or key in result:
            raise ValueError("empty or duplicate JSON key")
        key.encode("utf-8")
        result[key] = value
    return result


def _json_int(text: str) -> int:
    value = int(text)
    if value < -(1 << 63) or value > (1 << 64) - 1:
        raise ValueError("integer outside journal field range")
    return value


def _json_float(text: str) -> float:
    value = float(text)
    if not math.isfinite(value):
        raise ValueError("non-finite journal field")
    return value


def _reject_constant(text: str) -> None:
    raise ValueError("nonstandard JSON constant: " + text)


def _parse_json(data: bytes, *, flat: bool = False) -> Dict[str, Any]:
    value = json.loads(data.decode("utf-8"), object_pairs_hook=_unique_object,
                       parse_int=_json_int, parse_float=_json_float,
                       parse_constant=_reject_constant)
    if not isinstance(value, dict):
        raise ValueError("JSON root must be an object")
    if flat:
        for key, item in value.items():
            key.encode("utf-8")
            if type(item) not in (type(None), bool, int, float, str):
                raise ValueError("journal fields must be flat scalars")
            if isinstance(item, str):
                item.encode("utf-8")
    return value


def _issue(issue: str, detail: str, records: List[Dict[str, Any]],
           valid_bytes: int, file_bytes: int) -> Dict[str, Any]:
    return {
        "records": records, "issue": issue, "detail": detail,
        "valid_bytes": valid_bytes, "file_bytes": file_bytes,
        "complete_framing": issue == "none" and bool(records) and records[-1]["kind"] == 2,
        "loss_seen": any(row["kind"] == 12 for row in records),
    }


def _read_journal_bytes(data: bytes) -> Dict[str, Any]:
    size = len(data)
    records: List[Dict[str, Any]] = []
    if size > MAX_JOURNAL_BYTES:
        return _issue("limit_exceeded", "journal exceeds file limit", records, 0, size)
    if size < FILE_HEADER.size:
        return _issue("truncated", "partial journal header", records, 0, size)
    magic, version, header_bytes, reserved = FILE_HEADER.unpack_from(data)
    if magic != b"YKMJNL1\0":
        return _issue("corrupt", "journal magic differs", records, 0, size)
    if version != 1 or header_bytes != FILE_HEADER.size:
        return _issue("unsupported", "journal version or header length", records, 0, size)
    if reserved:
        return _issue("corrupt", "journal reserved bytes are nonzero", records, 0, size)
    offset = FILE_HEADER.size
    while offset < size:
        if records and records[-1]["kind"] == 2:
            return _issue("corrupt", "bytes follow RunEnd", records, offset, size)
        if size - offset < RECORD_HEADER.size:
            return _issue("truncated", "partial record header", records, offset, size)
        marker, length, sequence, monotonic_ns, kind, reserved, checksum = (
            RECORD_HEADER.unpack_from(data, offset))
        if marker != b"YKE1" or reserved:
            return _issue("corrupt", "record marker or reserved bytes", records, offset, size)
        if kind not in KIND_NAMES:
            return _issue("unsupported", "unknown record kind", records, offset, size)
        if length > MAX_PAYLOAD_BYTES or len(records) >= MAX_RECORDS:
            return _issue("limit_exceeded", "payload or record count limit", records, offset, size)
        end = offset + RECORD_HEADER.size + length
        if end > size:
            return _issue("truncated", "partial record payload", records, offset, size)
        payload = data[offset + RECORD_HEADER.size:end]
        expected = zlib.crc32(data[offset:offset + 28] + payload) & 0xFFFFFFFF
        if checksum != expected:
            return _issue("corrupt", "record CRC differs", records, offset, size)
        if sequence != len(records) + 1 or (not records and kind != 1) or (records and kind == 1):
            return _issue("corrupt", "invalid sequence or RunBegin position", records, offset, size)
        try:
            fields = _parse_json(payload, flat=True)
        except (ValueError, UnicodeError, OverflowError, RecursionError) as error:
            return _issue("corrupt", "invalid record JSON: " + str(error), records, offset, size)
        records.append({
            "sequence": sequence, "monotonic_ns": monotonic_ns,
            "kind": kind, "fields": fields,
        })
        offset = end
    if records and records[-1]["kind"] == 2:
        return _issue("none", "complete journal framing", records, offset, size)
    return _issue("open", "valid prefix without RunEnd", records, offset, size)


def _safe_path(path: os.PathLike[str], *, existing: bool) -> Path:
    value = Path(path)
    if ".." in value.parts:
        raise PackageError("path traversal is not allowed")
    path_abs = Path(os.path.abspath(str(value)))
    # Callers using a system path alias (for example /var on macOS) can pass
    # its canonical path. Reject every symlink component, not only the leaf.
    current = Path(path_abs.anchor)
    for part in path_abs.parts[1:]:
        current /= part
        try:
            mode = current.lstat().st_mode
        except FileNotFoundError:
            break
        if stat.S_ISLNK(mode):
            raise PackageError("symlink is not allowed: " + str(current))
    if existing and not path_abs.exists():
        raise PackageError("path does not exist: " + str(path_abs))
    return path_abs


def _read_file(path: Path, limit: int) -> bytes:
    try:
        # Check the opened descriptor without first blocking on a FIFO. The
        # nonblocking flag has no effect on the regular files accepted below.
        flags = (os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
                 | getattr(os, "O_NONBLOCK", 0))
        fd = os.open(path, flags)
        try:
            before = os.fstat(fd)
            if not stat.S_ISREG(before.st_mode):
                raise PackageError("input must be a regular file: " + str(path))
            if before.st_size > limit:
                raise PackageError("input exceeds file limit: " + str(path))
            chunks = []
            remaining = before.st_size
            while remaining:
                chunk = os.read(fd, min(1 << 20, remaining))
                if not chunk:
                    raise PackageError("input changed during read: " + str(path))
                chunks.append(chunk)
                remaining -= len(chunk)
            after = os.fstat(fd)
            if (before.st_size, before.st_mtime_ns, before.st_ino) != (
                    after.st_size, after.st_mtime_ns, after.st_ino):
                raise PackageError("input changed during read: " + str(path))
            if os.read(fd, 1):
                raise PackageError("input grew during read: " + str(path))
            return b"".join(chunks)
        finally:
            os.close(fd)
    except OSError as error:
        raise PackageError("cannot read regular input: " + str(path)) from error


def read_journal(path: os.PathLike[str]) -> Dict[str, Any]:
    """Return valid prefix records and the first framing or payload issue."""
    try:
        journal = _safe_path(path, existing=True)
        size = journal.stat().st_size
        if size > MAX_JOURNAL_BYTES:
            return _issue("limit_exceeded", "journal exceeds file limit", [], 0, size)
        return _read_journal_bytes(_read_file(journal, MAX_JOURNAL_BYTES))
    except (PackageError, OSError) as error:
        return _issue("corrupt", str(error), [], 0, 0)


def _valid_id(value: Any) -> bool:
    return type(value) is str and bool(_SAFE_ID.fullmatch(value)) and value not in (".", "..")


def _valid_hash(value: Any) -> bool:
    return type(value) is str and bool(_HASH.fullmatch(value))


def _valid_uint(value: Any) -> bool:
    return type(value) is int and 0 <= value <= (1 << 64) - 1


def _load_context(path: Path) -> Dict[str, Any]:
    try:
        value = _parse_json(_read_file(path, MAX_METADATA_BYTES))
    except (ValueError, UnicodeError, OverflowError, RecursionError) as error:
        raise PackageError("invalid context JSON") from error
    if value.get("schema") != CONTEXT_SCHEMA:
        raise PackageError("unsupported context schema")
    if set(value) - set(CONTEXT_FIELDS) - {"schema"}:
        raise PackageError("unknown context fields")
    for name, item in value.items():
        if name == "schema":
            continue
        if name in CONTEXT_HASH_FIELDS and not _valid_hash(item):
            raise PackageError("invalid context hash: " + name)
        if name == "source_commit" and (type(item) is not str or not _COMMIT.fullmatch(item)):
            raise PackageError("invalid context source_commit")
        if name == "run_id" and not _valid_id(item):
            raise PackageError("invalid context run_id")
        if name in ("platform_os", "platform_arch") and (
                type(item) is not str or not item or len(item) > 128):
            raise PackageError("invalid context platform: " + name)
        if type(item) is str:
            try:
                item.encode("utf-8")
            except UnicodeError as error:
                raise PackageError("invalid context Unicode: " + name) from error
    return {name: value.get(name) for name in ("schema",) + CONTEXT_FIELDS}


def _load_supervisor(path: Path) -> Dict[str, Any]:
    try:
        value = _parse_json(_read_file(path, MAX_METADATA_BYTES))
    except (ValueError, UnicodeError, OverflowError, RecursionError) as error:
        raise PackageError("invalid supervisor JSON") from error
    if value.get("schema") != SUPERVISOR_SCHEMA or set(value) != {
            "schema", "run_id", "status", "exit_code", "stop_reason"}:
        raise PackageError("invalid supervisor schema or fields")
    if not _valid_id(value["run_id"]):
        raise PackageError("invalid supervisor run_id")
    if value["status"] not in ("exited", "signaled", "unknown"):
        raise PackageError("invalid supervisor status")
    if value["exit_code"] is not None and (
            type(value["exit_code"]) is not int or not -(1 << 31) <= value["exit_code"] < (1 << 31)):
        raise PackageError("invalid supervisor exit_code")
    if value["status"] != "exited" and value["exit_code"] is not None:
        raise PackageError("non-exited supervisor cannot have exit_code")
    if type(value["stop_reason"]) is not str or len(value["stop_reason"]) > 1024:
        raise PackageError("invalid supervisor stop_reason")
    try:
        value["stop_reason"].encode("utf-8")
    except UnicodeError as error:
        raise PackageError("invalid supervisor Unicode") from error
    return value


def _identity(begin: Dict[str, Any]) -> Dict[str, Any]:
    # New optional panel bindings do not invalidate packages made before the
    # panel existed. Their absence is different from a declared invalid value.
    panel_fields = {"case_catalog_sha256", "prerequisite_basis_sha256"}
    identity = {name: begin.get(name) for name in IDENTITY_FIELDS + OPTIONAL_IDENTITY_FIELDS
                if name not in panel_fields or name in begin}
    identity["native_modes"] = _native_modes(begin)
    if "native_mode_schema" in begin:
        identity["native_mode_schema"] = begin["native_mode_schema"]
    return identity


def _native_modes(begin: Dict[str, Any]) -> Dict[str, str]:
    schema = begin.get("native_mode_schema")
    try:
        declared = native_modes.fields(schema)
    except ValueError:
        declared = native_modes.LEGACY_FIELDS
    observed = set(declared) | {key for key in begin if key.startswith("MHP3RD_NATIVE_")}
    return {name: begin[name] if type(begin.get(name)) is str and begin[name] else "unknown"
            for name in sorted(observed)}


def _validate(journal: Dict[str, Any], context: Dict[str, Any],
              supervisor: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    issues: List[str] = []
    records = journal["records"]
    begin = records[0]["fields"] if records and records[0]["kind"] == 1 else {}
    end = records[-1]["fields"] if records and records[-1]["kind"] == 2 else {}
    if journal["issue"] != "none":
        issues.append("journal_" + journal["issue"])
    if not begin:
        issues.append("missing_run_begin")
    elif begin.get("schema") != "journal-v1":
        issues.append("invalid_run_begin_schema")
    if not end:
        issues.append("missing_run_end")

    for name in IDENTITY_FIELDS:
        item = begin.get(name)
        if item is None:
            issues.append("missing_identity:" + name)
        elif name == "binary_sha256" and not _valid_hash(item):
            issues.append("invalid_identity:" + name)
        elif name == "baseline_commit" and (
                type(item) is not str or not _COMMIT.fullmatch(item)):
            issues.append("invalid_identity:" + name)
        elif name in ("run_id", "batch_id", "baseline_id") and not _valid_id(item):
            issues.append("invalid_identity:" + name)
        elif name == "role" and item not in ("baseline", "candidate"):
            issues.append("invalid_identity:" + name)
        elif name not in ("binary_sha256", "baseline_commit", "run_id", "batch_id",
                          "baseline_id", "role") and (type(item) is not str or not item):
            issues.append("invalid_identity:" + name)
    if begin.get("observer_schema") not in (None, "observers-v1"):
        issues.append("unsupported_observer_schema")
    if begin.get("recording_mode") not in (None, "observational-summary"):
        issues.append("unsupported_recording_mode")
    for name in OPTIONAL_IDENTITY_FIELDS:
        item = begin.get(name)
        if item is None:
            continue
        if name == "source_commit" and (type(item) is not str or not _COMMIT.fullmatch(item)):
            issues.append("invalid_optional_identity:" + name)
        elif name == "build_config_sha256" and not _valid_hash(item):
            issues.append("invalid_optional_identity:" + name)
        elif name == "build_config" and (type(item) is not str or not item):
            issues.append("invalid_optional_identity:" + name)
    mode_schema = begin.get("native_mode_schema")
    try:
        mode_fields = native_modes.fields(mode_schema)
        for name in sorted(native_modes.extra_fields(begin, mode_schema)):
            issues.append("undeclared_native_mode:" + name)
    except ValueError:
        issues.append("unknown_native_mode_schema")
        mode_fields = native_modes.LEGACY_FIELDS
        for name in sorted(key for key in begin if key.startswith("MHP3RD_NATIVE_") and key not in mode_fields):
            issues.append("undeclared_native_mode:" + name)
    if "native_mode_schema" in begin and mode_schema is None:
        issues.append("unknown_native_mode_schema")
    for name in mode_fields:
        if type(begin.get(name)) is not str or not begin[name]:
            issues.append("missing_native_mode:" + name)
        elif begin[name] not in ("off", "0", "verify", "native"):
            issues.append("invalid_native_mode:" + name)

    if "case_catalog_sha256" in begin or "prerequisite_basis_sha256" in begin:
        catalog_hash = begin.get("case_catalog_sha256")
        basis_hash = begin.get("prerequisite_basis_sha256")
        if not _valid_hash(catalog_hash) or catalog_hash.lower() != str(context.get("case_catalog_sha256", "")).lower():
            issues.append("invalid_optional_identity:case_catalog_sha256")
        try:
            expected_basis = prerequisite_basis_sha256(context, begin.get("baseline_id"), begin.get("baseline_commit"))
        except PackageError:
            expected_basis = None
        if not _valid_hash(basis_hash) or basis_hash.lower() != expected_basis:
            issues.append("invalid_optional_identity:prerequisite_basis_sha256")

    for name in CONTEXT_FIELDS:
        if context.get(name) is None:
            issues.append("missing_context:" + name)
    runtime_rows = [row for row in records if row["fields"].get("event") == "runtime.inputs"]
    if not runtime_rows:
        issues.append("missing_runtime_inputs")
    else:
        if len(runtime_rows) != 1:
            issues.append("multiple_runtime_inputs")
        first_case = next((row for row in records if row["kind"] == 3), None)
        for runtime_row in runtime_rows:
            if runtime_row["kind"] != 8:
                issues.append("invalid_runtime_inputs:kind")
            if first_case is not None and runtime_row["sequence"] > first_case["sequence"]:
                issues.append("runtime_inputs_after_case_begin")
            runtime_fields = runtime_row["fields"]
            actual_elf = runtime_fields.get("elf_sha256")
            supported = runtime_fields.get("supported_elf")
            if type(supported) is not bool:
                issues.append("invalid_runtime_inputs:supported_elf")
            elif supported is False:
                issues.append("unsupported_runtime_elf")
            if not _valid_hash(actual_elf):
                issues.append("invalid_runtime_inputs:elf_sha256")
            else:
                if actual_elf.lower() != SUPPORTED_ELF_SHA256:
                    issues.append("runtime_elf_profile_mismatch")
                if context.get("elf_sha256") is not None and actual_elf.lower() != context["elf_sha256"].lower():
                    issues.append("runtime_elf_context_mismatch")
                if type(supported) is bool and supported != (actual_elf.lower() == SUPPORTED_ELF_SHA256):
                    issues.append("runtime_inputs_contradiction")
    if begin.get("run_id") and context.get("run_id") and begin["run_id"] != context["run_id"]:
        issues.append("context_run_id_mismatch")
    if supervisor is None:
        issues.append("completion_unknown")
    elif begin.get("run_id") and supervisor["run_id"] != begin["run_id"]:
        issues.append("supervisor_run_id_mismatch")

    context_sha256 = _sha256(_canonical_json(context))
    binding = begin.get("context_sha256")
    if binding is None:
        identity_binding = "unbound"
    elif not _valid_hash(binding) or binding.lower() != context_sha256:
        identity_binding = "mismatch"
        issues.append("context_sha256_mismatch")
    else:
        identity_binding = "bound"

    if end:
        if end.get("completed") is not True:
            issues.append("run_end_not_completed")
        if type(end.get("stop_reason")) is not str or not end["stop_reason"]:
            issues.append("invalid_run_end_stop_reason")
        elif end["stop_reason"] not in ("guest_finished", "window closed", "quit from the menu"):
            issues.append("abnormal_run_stop_reason")
        caller_count = sum(row["kind"] not in (1, 2, 12) for row in records)
        for name in ("accepted_events", "written_events", "dropped_events", "invalid_events"):
            if not _valid_uint(end.get(name)):
                issues.append("invalid_run_end_counter:" + name)
        if _valid_uint(end.get("written_events")) and end["written_events"] != caller_count:
            issues.append("written_event_count_mismatch")
        if _valid_uint(end.get("accepted_events")) and _valid_uint(end.get("written_events")) \
                and end["accepted_events"] != end["written_events"]:
            issues.append("accepted_event_count_mismatch")
        for name in ("dropped_events", "invalid_events"):
            if _valid_uint(end.get(name)) and end[name]:
                issues.append("nonzero_" + name)
    if journal["loss_seen"]:
        issues.append("recording_loss")

    health_rows = [row for row in records if row["kind"] == 8 and
                   row["fields"].get("event") == "observer.health"]
    if not health_rows:
        issues.append("missing_observer_health")
    else:
        if len(health_rows) != 1:
            issues.append("multiple_observer_health")
        last_caller = next((row for row in reversed(records) if row["kind"] not in (1, 2, 12)), None)
        if last_caller is not health_rows[-1]:
            issues.append("observer_health_not_final")
        for health_row in health_rows:
            health = health_row["fields"]
            for name in ("emission_errors", "dropped_events", "invalid_events"):
                if not _valid_uint(health.get(name)):
                    issues.append("invalid_observer_health:" + name)
                elif health[name]:
                    issues.append("nonzero_observer_" + name)
            if type(health.get("io_failed")) is not bool:
                issues.append("invalid_observer_health:io_failed")
            elif health["io_failed"]:
                issues.append("observer_io_failed")
            prior_callers = sum(row["kind"] not in (1, 2, 12)
                                for row in records[:health_row["sequence"] - 1])
            for name in ("accepted_events", "written_events"):
                if name in health and (
                        not _valid_uint(health[name]) or health[name] != prior_callers):
                    issues.append("invalid_observer_health:" + name)
    if supervisor is not None:
        if supervisor["status"] != "exited":
            issues.append("supervisor_" + supervisor["status"])
        else:
            if end and supervisor["stop_reason"] != end.get("stop_reason"):
                issues.append("supervisor_stop_reason_mismatch")
            if supervisor["exit_code"] == 0 and end.get("stop_reason") == "guest_finished":
                pass
            elif supervisor["exit_code"] == 4 and end.get("completed") is True and \
                    end.get("stop_reason") in ("window closed", "quit from the menu"):
                pass
            else:
                issues.append("abnormal_process_exit")

    issues = list(dict.fromkeys(issues))
    metadata_only_prefixes = ("missing_identity:", "invalid_identity:",
                              "invalid_optional_identity:", "missing_context:",
                              "missing_native_mode:", "invalid_native_mode:",
                              "undeclared_native_mode:",
                              "invalid_runtime_inputs:")
    metadata_only_issues = {
        "unsupported_observer_schema", "unsupported_recording_mode",
        "missing_runtime_inputs", "multiple_runtime_inputs",
        "runtime_inputs_after_case_begin", "unsupported_runtime_elf",
        "runtime_elf_profile_mismatch", "runtime_elf_context_mismatch",
        "runtime_inputs_contradiction",
        "unknown_native_mode_schema",
    }
    metadata_complete = not any(item.startswith(metadata_only_prefixes) or
                                item in metadata_only_issues for item in issues)
    return {
        "recording_complete": all(item.startswith(metadata_only_prefixes) or
                                  item in metadata_only_issues for item in issues),
        "issues": issues, "identity_binding": identity_binding,
        "metadata_complete": metadata_complete,
    }


def _jsonl(rows: List[Dict[str, Any]]) -> bytes:
    return b"".join(_canonical_json(row) + b"\n" for row in rows)


def _derived(journal: Dict[str, Any], validation: Dict[str, Any]) -> Dict[str, bytes]:
    rows = journal["records"]
    kind_counts = Counter(KIND_NAMES[row["kind"]] for row in rows)
    event_counts = Counter(row["fields"].get("event") for row in rows
                           if type(row["fields"].get("event")) is str)
    probe_latest: Dict[Tuple[str, str], Dict[str, Any]] = {}
    for row in rows:
        fields = row["fields"]
        if row["kind"] == 9 and fields.get("event") == "probe.summary":
            key = (str(fields.get("leaf", "unknown")), str(fields.get("entry", "unknown")))
            probe_latest[key] = row
    stats = {
        "kind_counts": dict(sorted(kind_counts.items())),
        "event_counts": dict(sorted(event_counts.items())),
        "probe_latest": [probe_latest[key] for key in sorted(probe_latest)],
    }
    markers = {"markers": [row for row in rows if row["kind"] in (3, 4, 5, 6)]}
    diagnostics = [row for row in rows if row["kind"] in (11, 12)]
    summary = {
        "record_count": len(rows), "valid_bytes": journal["valid_bytes"],
        "journal_issue": journal["issue"], "complete_framing": journal["complete_framing"],
        "loss_seen": journal["loss_seen"], "marker_count": len(markers["markers"]),
        "diagnostic_count": len(diagnostics), "recording_complete": validation["recording_complete"],
        "issues": validation["issues"],
    }
    return {
        "events.jsonl": _jsonl(rows),
        "statistics.json": _canonical_json(stats) + b"\n",
        "markers.json": _canonical_json(markers) + b"\n",
        "diagnostics.jsonl": _jsonl(diagnostics),
        "summary.json": _canonical_json(summary) + b"\n",
    }


def _artifact_entry(data: bytes) -> Dict[str, Any]:
    return {"size": len(data), "sha256": _sha256(data)}


def _packager_revision() -> str:
    return "source-sha256:" + _sha256(Path(__file__).read_bytes())


def _ensure_output(run_directory: Path, output_directory: Path) -> None:
    if not run_directory.is_dir() or run_directory.is_symlink():
        raise PackageError("run directory must be a real directory")
    if output_directory.exists() or output_directory.is_symlink():
        raise PackageError("output directory already exists")
    if not output_directory.parent.is_dir():
        raise PackageError("output parent directory does not exist")
    run_real = run_directory.resolve(strict=True)
    output_real = output_directory.parent.resolve(strict=True) / output_directory.name
    if output_real == run_real or run_real in output_real.parents:
        raise PackageError("output cannot be inside the input run directory")


def _publish_stage(stage: Path, output: Path) -> None:
    """Atomically rename a staged directory without replacing a late arrival."""
    if sys.platform == "darwin":
        # RENAME_EXCL is defined by Darwin's sys/stdio.h.
        library = ctypes.CDLL(None, use_errno=True)
        rename = library.renamex_np
        rename.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint)
        rename.restype = ctypes.c_int
        result = rename(os.fsencode(stage), os.fsencode(output), 0x00000004)
    elif sys.platform.startswith("linux"):
        library = ctypes.CDLL(None, use_errno=True)
        try:
            rename = library.renameat2
        except AttributeError as error:
            raise PackageError("atomic no-replace rename is unavailable") from error
        rename.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int,
                           ctypes.c_char_p, ctypes.c_uint)
        rename.restype = ctypes.c_int
        result = rename(-100, os.fsencode(stage), -100, os.fsencode(output), 1)
    elif os.name == "nt":
        # Python's Windows os.rename raises FileExistsError for an existing dst.
        os.rename(stage, output)
        return
    else:
        raise PackageError("atomic no-replace rename is unavailable")
    if result:
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code), str(output))


def package_run(run_directory: os.PathLike[str], context_path: os.PathLike[str],
                output_directory: os.PathLike[str],
                supervisor_path: Optional[os.PathLike[str]] = None) -> Dict[str, Any]:
    """Publish a fresh package atomically and return its manifest."""
    source = _safe_path(run_directory, existing=True)
    context_file = _safe_path(context_path, existing=True)
    output = _safe_path(output_directory, existing=False)
    supervisor_file = _safe_path(supervisor_path, existing=True) if supervisor_path else None
    _ensure_output(source, output)
    journal_file = _safe_path(source / "events.journal", existing=True)
    journal_bytes = _read_file(journal_file, MAX_JOURNAL_BYTES)
    journal = _read_journal_bytes(journal_bytes)
    context = _load_context(context_file)
    supervisor = _load_supervisor(supervisor_file) if supervisor_file else None
    validation = _validate(journal, context, supervisor)
    derived = _derived(journal, validation)
    artifacts = {"events.journal": journal_bytes, **derived}
    if any(len(value) > MAX_DERIVED_BYTES for value in artifacts.values()):
        raise PackageError("derived artifact exceeds limit")
    begin = journal["records"][0]["fields"] if journal["records"] else {}
    manifest = {
        "schema": PACKAGE_SCHEMA,
        "packager_revision": _packager_revision(),
        "identity": _identity(begin),
        "native_modes": _native_modes(begin),
        "context": context,
        "context_sha256": _sha256(_canonical_json(context)),
        "supervisor": supervisor,
        "journal": {name: journal[name] for name in (
            "issue", "detail", "valid_bytes", "file_bytes", "complete_framing", "loss_seen")},
        "artifacts": {name: _artifact_entry(artifacts[name]) for name in ARTIFACT_NAMES},
        "validation": validation,
    }
    stage: Optional[Path] = None
    try:
        stage = Path(tempfile.mkdtemp(prefix="." + output.name + "-stage-", dir=output.parent))
        for name in ARTIFACT_NAMES:
            destination = stage / name
            with destination.open("xb") as stream:
                stream.write(artifacts[name])
            if name == "events.journal":
                destination.chmod(0o444)
        with (stage / "manifest.json").open("xb") as stream:
            stream.write(_canonical_json(manifest) + b"\n")
        if output.exists() or output.is_symlink():
            raise PackageError("output directory appeared during packaging")
        _publish_stage(stage, output)
        stage = None
    finally:
        if stage is not None:
            shutil.rmtree(stage)
    return manifest


def load_package(path: os.PathLike[str]) -> Dict[str, Any]:
    """Recheck fixed artifacts and recompute health from the raw journal."""
    directory = _safe_path(path, existing=True)
    if not directory.is_dir() or directory.is_symlink():
        raise PackageError("package must be a real directory")
    manifest_file = _safe_path(directory / "manifest.json", existing=True)
    try:
        manifest = _parse_json(_read_file(manifest_file, MAX_METADATA_BYTES))
    except (ValueError, UnicodeError, OverflowError, RecursionError) as error:
        raise PackageError("invalid package manifest") from error
    if manifest.get("schema") != PACKAGE_SCHEMA:
        raise PackageError("unsupported package schema")
    declared = manifest.get("artifacts")
    if type(declared) is not dict or set(declared) != set(ARTIFACT_NAMES):
        raise PackageError("manifest must list exactly the fixed artifacts")
    issues: List[str] = []
    revision = manifest.get("packager_revision")
    if type(revision) is not str or not revision.startswith("source-sha256:") or \
            not _valid_hash(revision[len("source-sha256:"):]):
        issues.append("invalid_packager_revision")
    actual: Dict[str, bytes] = {}
    for name in ARTIFACT_NAMES:
        entry = declared[name]
        if type(entry) is not dict or set(entry) != {"size", "sha256"} or \
                type(entry["size"]) is not int or entry["size"] < 0 or not _valid_hash(entry["sha256"]):
            issues.append("invalid_artifact_entry:" + name)
            continue
        try:
            file_path = _safe_path(directory / name, existing=True)
            data = _read_file(file_path, MAX_JOURNAL_BYTES if name == "events.journal" else MAX_DERIVED_BYTES)
        except PackageError:
            issues.append("missing_or_unsafe_artifact:" + name)
            continue
        actual[name] = data
        if len(data) != entry["size"] or _sha256(data) != entry["sha256"].lower():
            issues.append("artifact_mismatch:" + name)
    journal = _read_journal_bytes(actual["events.journal"]) if "events.journal" in actual \
        else _issue("corrupt", "missing events.journal", [], 0, 0)
    context = manifest.get("context")
    supervisor = manifest.get("supervisor")
    if type(context) is not dict:
        raise PackageError("invalid manifest context")
    try:
        # The same strict validators apply to packaged, editable metadata.
        context = _load_context_bytes(context)
        if supervisor is not None:
            supervisor = _load_supervisor_bytes(supervisor)
    except PackageError as error:
        issues.append("invalid_manifest_metadata:" + str(error))
        context = {name: None for name in ("schema",) + CONTEXT_FIELDS}
        context["schema"] = CONTEXT_SCHEMA
        supervisor = None
    validation = _validate(journal, context, supervisor)
    if manifest.get("validation") != validation:
        issues.append("manifest_validation_mismatch")
    if manifest.get("context_sha256") != _sha256(_canonical_json(context)):
        issues.append("context_hash_mismatch")
    begin = journal["records"][0]["fields"] if journal["records"] else {}
    if manifest.get("identity") != _identity(begin):
        issues.append("manifest_identity_mismatch")
    if manifest.get("native_modes") != _native_modes(begin):
        issues.append("manifest_native_modes_mismatch")
    if manifest.get("journal") != {name: journal[name] for name in (
            "issue", "detail", "valid_bytes", "file_bytes", "complete_framing", "loss_seen")}:
        issues.append("manifest_journal_mismatch")
    expected_derived = _derived(journal, validation)
    for name, data in expected_derived.items():
        if name in actual and actual[name] != data:
            issues.append("derived_content_mismatch:" + name)
    validation["issues"] = list(dict.fromkeys(validation["issues"] + issues))
    if issues:
        validation["recording_complete"] = False
    if "invalid_packager_revision" in issues:
        validation["metadata_complete"] = False
    return {"manifest": manifest, "records": journal["records"], "validation": validation}


def _load_context_bytes(value: Dict[str, Any]) -> Dict[str, Any]:
    # Reuse the exact external-input schema without reading any path from the manifest.
    if value.get("schema") != CONTEXT_SCHEMA or set(value) != set(("schema",) + CONTEXT_FIELDS):
        raise PackageError("invalid context fields")
    for name in CONTEXT_FIELDS:
        item = value[name]
        if item is None:
            continue
        if name in CONTEXT_HASH_FIELDS and not _valid_hash(item):
            raise PackageError("invalid context hash: " + name)
        if name == "source_commit" and (type(item) is not str or not _COMMIT.fullmatch(item)):
            raise PackageError("invalid context source_commit")
        if name == "run_id" and not _valid_id(item):
            raise PackageError("invalid context run_id")
        if name in ("platform_os", "platform_arch") and (
                type(item) is not str or not item or len(item) > 128):
            raise PackageError("invalid context platform: " + name)
        if type(item) is str:
            try:
                item.encode("utf-8")
            except UnicodeError as error:
                raise PackageError("invalid context Unicode: " + name) from error
    return value


def _load_supervisor_bytes(value: Dict[str, Any]) -> Dict[str, Any]:
    if type(value) is not dict or value.get("schema") != SUPERVISOR_SCHEMA or set(value) != {
            "schema", "run_id", "status", "exit_code", "stop_reason"}:
        raise PackageError("invalid supervisor fields")
    if not _valid_id(value["run_id"]) or value["status"] not in ("exited", "signaled", "unknown"):
        raise PackageError("invalid supervisor identity or status")
    if value["exit_code"] is not None and (
            type(value["exit_code"]) is not int or not -(1 << 31) <= value["exit_code"] < (1 << 31)):
        raise PackageError("invalid supervisor exit_code")
    if value["status"] != "exited" and value["exit_code"] is not None:
        raise PackageError("non-exited supervisor cannot have exit_code")
    if type(value["stop_reason"]) is not str or len(value["stop_reason"]) > 1024:
        raise PackageError("invalid supervisor stop_reason")
    try:
        value["stop_reason"].encode("utf-8")
    except UnicodeError as error:
        raise PackageError("invalid supervisor Unicode") from error
    return value


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", required=True, type=Path)
    parser.add_argument("--context", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--supervisor", type=Path)
    args = parser.parse_args(argv)
    try:
        manifest = package_run(args.run_dir, args.context, args.output, args.supervisor)
    except (PackageError, OSError) as error:
        parser.exit(1, "run packaging failed: " + str(error) + "\n")
    print(json.dumps({"output": str(args.output), "validation": manifest["validation"]},
                     sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
