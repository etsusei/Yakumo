#!/usr/bin/env python3
"""Gate original texture-command construction on the complete local TMH corpus.

The original-code oracle runs in bounded shards on immutable decoded entries.
This tool publishes metadata only; it never writes a resource or starts a game.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


MAX_METADATA_BYTES = 32 * 1024 * 1024
MAX_PARENT_BYTES = 256 * 1024 * 1024
MAX_CHILD_BYTES = 16 * 1024 * 1024
MAX_RECORDS_PER_INPUT = 4096
MAX_INPUTS_PER_SHARD = 32
MAX_RECORDS_PER_SHARD = 128
PROCESS_SECONDS = 60
MAX_INTERPRETER_SLICES = 2_000_000
SUPPORTED_INPUTS = 2256
SUPPORTED_RECORDS = 8866
SUPPORTED_ENTRY_COUNT = 6043
SUPPORTED_ELF_SHA256 = "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c"
ORACLE_SCOPE = "original_texture_command_builder_not_pixels"
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}\Z")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_digest(path: Path, maximum_size: int | None = None) -> str:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"Missing or linked file: {path}")
    size = path.stat().st_size
    if maximum_size is not None and size > maximum_size:
        raise ValueError(f"File exceeds size budget: {path}")
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            hasher.update(chunk)
    if path.stat().st_size != size:
        raise ValueError(f"File changed while hashing: {path}")
    return hasher.hexdigest()


def unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for name, value in pairs:
        if name in result:
            raise ValueError(f"Duplicate JSON key: {name}")
        result[name] = value
    return result


def read_metadata(path: Path) -> tuple[bytes, dict]:
    if (path.is_symlink() or not path.is_file() or
            not 0 < path.stat().st_size <= MAX_METADATA_BYTES):
        raise ValueError(f"Missing, linked, empty, or oversized metadata: {path}")
    raw = path.read_bytes()
    if not 0 < len(raw) <= MAX_METADATA_BYTES:
        raise ValueError(f"Metadata changed or grew during read: {path}")
    value = json.loads(raw, object_pairs_hook=unique_object)
    if type(value) is not dict:
        raise ValueError(f"Metadata must be an object: {path}")
    return raw, value


def valid_sha256(value: object) -> bool:
    return type(value) is str and SHA256_PATTERN.fullmatch(value) is not None


def manifest_entries(manifest: dict) -> list[dict]:
    entries = manifest.get("entries")
    archive = manifest.get("archive")
    if (manifest.get("schema_version") != 1 or type(entries) is not list or
            len(entries) != SUPPORTED_ENTRY_COUNT or type(archive) is not dict or
            archive.get("entry_count") != len(entries)):
        raise ValueError("Unsupported or incomplete resource manifest")
    for ident, entry in enumerate(entries):
        if (type(entry) is not dict or type(entry.get("id")) is not int or
                entry["id"] != ident or
                entry.get("output_path") != f"raw-entries/{ident:05d}.bin" or
                type(entry.get("extracted_length")) is not int or
                not 0 <= entry["extracted_length"] <= MAX_PARENT_BYTES or
                not valid_sha256(entry.get("sha256"))):
            raise ValueError(f"Invalid source identity for raw entry {ident}")
    return entries


def layout_rows(layout: dict, entries: list[dict]) -> tuple[list[tuple[dict, str]], int]:
    children = layout.get("children")
    if (layout.get("schema_version") != 1 or layout.get("failures") != [] or
            type(children) is not list or len(children) != SUPPORTED_INPUTS or
            layout.get("child_count") != len(children) or
            layout.get("raw_entries_hashed") != len(entries) or
            layout.get("standalone_count") != 12 or
            layout.get("indexed_child_count") != SUPPORTED_INPUTS - 12):
        raise ValueError("TMH layout is not the complete supported inventory")
    rows: list[tuple[dict, str]] = []
    identities: set[tuple[int, str]] = set()
    records = 0
    origins = {"indexed_child": 0, "standalone_entry": 0}
    for row in children:
        if type(row) is not dict:
            raise ValueError("Malformed TMH child metadata")
        ident = row.get("entry_id")
        if type(ident) is not int or not 0 <= ident < len(entries):
            raise ValueError("TMH child names an absent raw entry")
        path = row.get("child_path")
        if (type(path) is not list or len(path) > 2 or
                any(type(part) is not int or not 0 <= part < 4096 for part in path)):
            raise ValueError("Malformed TMH child path")
        child_id = ".".join(map(str, path)) if path else "root"
        if (ident, child_id) in identities:
            raise ValueError("Duplicate TMH child identity")
        identities.add((ident, child_id))
        origin = row.get("origin")
        if origin not in origins or (origin == "standalone_entry") != (not path):
            raise ValueError("Inconsistent TMH child origin")
        origins[origin] += 1
        parent_size = entries[ident]["extracted_length"]
        offset, size, count = (row.get("offset_in_decoded_entry"), row.get("size"),
                               row.get("count_word8"))
        if (any(type(value) is not int for value in (offset, size, count)) or
                not 0 <= offset <= parent_size or
                not 16 <= size <= min(MAX_CHILD_BYTES, parent_size - offset) or
                not 0 <= count <= MAX_RECORDS_PER_INPUT):
            raise ValueError("TMH child span or record count exceeds a budget")
        if not valid_sha256(row.get("sha256")):
            raise ValueError("Malformed TMH child digest")
        record_rows = row.get("records")
        if (type(record_rows) is not list or len(record_rows) != count or
                any(type(record) is not dict or record.get("index") != number
                    for number, record in enumerate(record_rows))):
            raise ValueError("TMH record list differs from header count")
        line = (f"{ident}\t{parent_size}\t{entries[ident]['sha256']}\t{offset}\t"
                f"{size}\t{row['sha256']}\t{child_id}\n")
        rows.append((row, line))
        records += count
    if (records != SUPPORTED_RECORDS or layout.get("record_count") != records or
            origins != {"indexed_child": SUPPORTED_INPUTS - 12, "standalone_entry": 12}):
        raise ValueError("TMH layout totals differ from the supported corpus")
    return rows, records


def shard_rows(rows: list[tuple[dict, str]]) -> list[list[tuple[dict, str]]]:
    shards: list[list[tuple[dict, str]]] = []
    current: list[tuple[dict, str]] = []
    record_count = 0
    for row in rows:
        count = row[0]["count_word8"]
        if count > MAX_RECORDS_PER_SHARD:
            raise ValueError("One TMH input exceeds the shard record budget")
        if current and (len(current) >= MAX_INPUTS_PER_SHARD or
                        record_count + count > MAX_RECORDS_PER_SHARD):
            shards.append(current)
            current, record_count = [], 0
        current.append(row)
        record_count += count
    if current:
        shards.append(current)
    return shards


def verify_raw_entries(raw_entries: Path, rows: list[tuple[dict, str]],
                       entries: list[dict]) -> None:
    selected: dict[int, list[dict]] = {}
    for row, _ in rows:
        selected.setdefault(row["entry_id"], []).append(row)
    for ident, children in selected.items():
        source = entries[ident]
        path = raw_entries / f"{ident:05d}.bin"
        if path.is_symlink() or not path.is_file() or path.stat().st_size != source["extracted_length"]:
            raise ValueError(f"Raw entry {ident} is missing, linked, or changed in size")
        if file_digest(path, MAX_PARENT_BYTES) != source["sha256"]:
            raise ValueError(f"Raw entry {ident} differs from the resource manifest")
        with path.open("rb") as stream:
            for child in children:
                stream.seek(child["offset_in_decoded_entry"])
                data = stream.read(child["size"])
                if len(data) != child["size"] or digest(data) != child["sha256"]:
                    raise ValueError(f"TMH child {ident}/{child['child_path']} changed")


def source_paths(oracle: Path) -> dict[str, Path]:
    tools = Path(__file__).resolve().parent
    return {
        "check_texture_commands_py": Path(__file__).resolve(),
        "check_tmh_views_py": tools / "check_tmh_views.py",
        "inspect_tmh_layout_py": tools / "inspect_tmh_layout.py",
        "texture_command_oracle_cpp": tools.parent / "tests" / "texture_command_oracle.cpp",
        "texture_commands_cpp": tools.parent / "host" / "resources" / "texture_commands.cpp",
        "texture_commands_hpp": tools.parent / "host" / "resources" / "texture_commands.hpp",
        "texture_commands_bridge_cpp": tools.parent / "host" / "native" / "texture_commands_bridge.cpp",
        "texture_commands_bridge_hpp": tools.parent / "host" / "native" / "texture_commands_bridge.hpp",
        "texture_command_dispatch_cpp": tools.parent / "host" / "native" / "texture_command_dispatch.cpp",
        "texture_command_dispatch_hpp": tools.parent / "host" / "native" / "texture_command_dispatch.hpp",
        "instrument_texture_commands_py": tools / "instrument_texture_commands.py",
        "texture_command_oracle_binary": oracle,
    }


def source_hashes(oracle: Path) -> dict[str, str]:
    return {name: file_digest(path) for name, path in source_paths(oracle).items()}


def run_oracle(arguments: list[str], report_path: Path) -> tuple[bytes, dict]:
    environment = {name: value for name, value in os.environ.items()
                   if not name.startswith(("MHP3RD_", "PSPRECOMP_"))}
    try:
        completed = subprocess.run(arguments, env=environment, text=True,
                                   capture_output=True, timeout=PROCESS_SECONDS, check=False)
    except subprocess.TimeoutExpired as error:
        raise ValueError(f"Texture-command oracle exceeded {PROCESS_SECONDS} seconds") from error
    if completed.returncode != 0:
        raise ValueError(f"Texture-command oracle failed ({completed.returncode}): "
                         f"{completed.stderr[-2000:]}")
    return read_metadata(report_path)


def validate_common_report(report: dict) -> None:
    if (report.get("schema_version") != 1 or report.get("scope") != ORACLE_SCOPE or
            report.get("success") is not True or report.get("portable_core_compared") is not True or
            report.get("guest_adapter_compared") is not True or
            report.get("prepared_plan_compared") is not True):
        raise ValueError("Texture-command oracle did not report a successful supported scope")


def checked_count(report: dict, name: str) -> int:
    value = report.get(name)
    if type(value) is not int or value < 0:
        raise ValueError(f"Oracle report has invalid {name}")
    return value


def validate_synthetic(report: dict) -> None:
    validate_common_report(report)
    cases = checked_count(report, "synthetic_cases")
    if checked_count(report, "plan_calls") != checked_count(report, "builder_calls"):
        raise ValueError("Synthetic report omitted prepared plan comparisons")
    if checked_count(report, "adapter_calls") != checked_count(report, "builder_calls"):
        raise ValueError("Synthetic report did not compare the actual guest adapter")
    if checked_count(report, "portable_calls") != checked_count(report, "builder_calls"):
        raise ValueError("Synthetic report did not compare the actual portable core")
    if (checked_count(report, "input_count") != 0 or
            checked_count(report, "descriptor_records") != 0 or
            cases == 0 or checked_count(report, "builder_calls") != cases or
            ("inputs" in report and report["inputs"] != [])):
        raise ValueError("Synthetic oracle report omitted bounded cases")
    slices = checked_count(report, "max_interpreter_slices")
    if not 0 < slices <= MAX_INTERPRETER_SLICES:
        raise ValueError("Synthetic oracle exceeded the interpreter slice budget")


def validate_shard(report: dict, shard: list[tuple[dict, str]]) -> dict[str, int]:
    validate_common_report(report)
    produced = report.get("inputs")
    if type(produced) is not list or len(produced) != len(shard):
        raise ValueError("Texture-command shard omitted or added inputs")
    records = sum(row["count_word8"] for row, _ in shard)
    calls = sum(2 if row["count_word8"] else 1 for row, _ in shard)
    slots = sum(row["count_word8"] + (1 if row["count_word8"] else 0)
                for row, _ in shard)
    for name, expected in (("input_count", len(shard)),
                           ("descriptor_records", records),
                           ("builder_calls", calls),
                           ("emitted_command_slots", slots)):
        if checked_count(report, name) != expected:
            raise ValueError(f"Texture-command shard {name} differs from input inventory")
    if checked_count(report, "plan_calls") != calls:
        raise ValueError("Texture-command shard omitted prepared plan comparisons")
    if checked_count(report, "adapter_calls") != calls:
        raise ValueError("Texture-command shard omitted guest adapter calls")
    if checked_count(report, "portable_calls") != calls:
        raise ValueError("Texture-command shard omitted portable core calls")
    for actual, (expected, _) in zip(produced, shard):
        child_id = ".".join(map(str, expected["child_path"])) if expected["child_path"] else "root"
        identity = {
            "entry_id": expected["entry_id"],
            "child_id": child_id,
            "child_sha256": expected["sha256"],
            "offset_in_decoded_entry": expected["offset_in_decoded_entry"],
            "size": expected["size"],
            "records": expected["count_word8"],
        }
        if type(actual) is not dict or any(actual.get(name) != value or
                                            type(actual.get(name)) is not type(value)
                                            for name, value in identity.items()):
            raise ValueError("Texture-command shard input identity differs from layout")
        for name in ("full_commands_sha256", "partial_commands_sha256"):
            if (name in actual and actual[name] is not None and
                    not (name == "partial_commands_sha256" and identity["records"] == 0 and
                         actual[name] == "") and not valid_sha256(actual[name])):
                raise ValueError(f"Malformed per-input {name}")
    slices = checked_count(report, "max_interpreter_slices")
    if not 0 < slices <= MAX_INTERPRETER_SLICES:
        raise ValueError("Texture-command shard exceeded the interpreter slice budget")
    return {"input_count": len(shard), "descriptor_records": records,
            "builder_calls": calls, "portable_calls": calls, "adapter_calls": calls, "plan_calls": calls, "emitted_command_slots": slots,
            "max_interpreter_slices": slices}


def check(workspace: Path, layout_report: Path, elf: Path, oracle: Path, output: Path) -> dict:
    if workspace.is_symlink() or (workspace / "raw-entries").is_symlink():
        raise ValueError("Resource workspace and raw-entry directory must not be symlinks")
    workspace = workspace.resolve(strict=True)
    raw_entries = workspace / "raw-entries"
    if not raw_entries.is_dir():
        raise ValueError("Missing decoded raw-entry directory")
    for path in (layout_report, elf, oracle):
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"Missing or linked input file: {path}")
    layout_report, elf, oracle = (path.resolve(strict=True) for path in
                                  (layout_report, elf, oracle))
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError("Output exists; preserve earlier evidence")
    if output == workspace or workspace in output.resolve().parents:
        raise ValueError("Output must be outside the original resource workspace")
    output.parent.mkdir(parents=True, exist_ok=True)
    output = output.parent.resolve(strict=True) / output.name
    if output == workspace or workspace in output.parents:
        raise ValueError("Output must be outside the original resource workspace")
    manifest_path = workspace / "manifest.json"
    manifest_bytes, manifest = read_metadata(manifest_path)
    layout_bytes, layout = read_metadata(layout_report)
    manifest_sha256 = digest(manifest_bytes)
    if manifest_sha256 != layout.get("resource_manifest_sha256"):
        raise ValueError("Resource manifest differs from the TMH layout inventory")
    entries = manifest_entries(manifest)
    rows, record_count = layout_rows(layout, entries)
    elf_sha256 = file_digest(elf, MAX_METADATA_BYTES)
    if elf_sha256 != SUPPORTED_ELF_SHA256:
        raise ValueError("ELF differs from the supported original executable")
    oracle_hashes_before = source_hashes(oracle)
    verify_raw_entries(raw_entries, rows, entries)
    with tempfile.TemporaryDirectory(prefix="texture-command-gate-", dir=output.parent) as temporary:
        stage = Path(temporary)
        synthetic_path = stage / "synthetic.json"
        synthetic_bytes, synthetic = run_oracle(
            [str(oracle), "--synthetic", str(elf), str(synthetic_path)], synthetic_path)
        validate_synthetic(synthetic)
        all_inputs: list[dict] = []
        call_reports: list[dict] = []
        totals = {"input_count": 0, "descriptor_records": 0,
                  "builder_calls": 0, "portable_calls": 0, "adapter_calls": 0, "plan_calls": 0, "emitted_command_slots": 0,
                  "max_interpreter_slices": 0}
        for number, shard in enumerate(shard_rows(rows)):
            index_path = stage / f"index-{number:04d}.tsv"
            index_path.write_text("".join(line for _, line in shard), encoding="ascii")
            report_path = stage / f"report-{number:04d}.json"
            report_bytes, shard_report = run_oracle(
                [str(oracle), str(elf), str(raw_entries), str(index_path), str(report_path)],
                report_path)
            counts = validate_shard(shard_report, shard)
            for name in ("input_count", "descriptor_records", "builder_calls", "portable_calls", "adapter_calls", "plan_calls",
                         "emitted_command_slots"):
                totals[name] += counts[name]
            totals["max_interpreter_slices"] = max(totals["max_interpreter_slices"],
                                                     counts["max_interpreter_slices"])
            all_inputs.extend(shard_report["inputs"])
            call_reports.append({"shard": number, "report_sha256": digest(report_bytes),
                                 **counts})
        expected_calls = sum(2 if row["count_word8"] else 1 for row, _ in rows)
        expected_slots = sum(row["count_word8"] + (1 if row["count_word8"] else 0)
                             for row, _ in rows)
        if (totals["input_count"] != len(rows) or
                totals["descriptor_records"] != record_count or
                totals["builder_calls"] != expected_calls or
                totals["portable_calls"] != expected_calls or
                totals["adapter_calls"] != expected_calls or
                totals["plan_calls"] != expected_calls or
                totals["emitted_command_slots"] != expected_slots or
                len(all_inputs) != len(rows)):
            raise ValueError("Combined texture-command coverage is incomplete")
        verify_raw_entries(raw_entries, rows, entries)
        if (manifest_path.read_bytes() != manifest_bytes or
                layout_report.read_bytes() != layout_bytes or
                file_digest(elf, MAX_METADATA_BYTES) != elf_sha256 or
                source_hashes(oracle) != oracle_hashes_before):
            raise ValueError("An input manifest, layout, ELF, source, or oracle changed during the gate")
        report = {
            "schema_version": 1,
            "recorded_at": datetime.now(timezone.utc).isoformat(),
            "scope": ORACLE_SCOPE,
            "success": True,
            "portable_core_compared": True,
            "guest_adapter_compared": True,
            "prepared_plan_compared": True,
            **totals,
            "synthetic_result": synthetic,
            "synthetic_report_sha256": digest(synthetic_bytes),
            "oracle_calls": call_reports,
            "inputs": all_inputs,
            "paths": {"workspace": str(workspace), "raw_entries": str(raw_entries),
                      "manifest": str(manifest_path), "layout_report": str(layout_report),
                      "elf": str(elf), "oracle": str(oracle), "output": str(output),
                      **{name: str(path) for name, path in source_paths(oracle).items()}},
            "elf_sha256": elf_sha256,
            "resource_manifest_sha256": manifest_sha256,
            "layout_inventory_sha256": digest(layout_bytes),
            "source_sha256": oracle_hashes_before,
            "limits": {"seconds_per_process": PROCESS_SECONDS,
                       "max_inputs_per_shard": MAX_INPUTS_PER_SHARD,
                       "max_records_per_shard": MAX_RECORDS_PER_SHARD,
                       "interpreter_slices_per_builder_call": MAX_INTERPRETER_SLICES,
                       "parent_bytes": MAX_PARENT_BYTES,
                       "child_bytes": MAX_CHILD_BYTES,
                       "records_per_input": MAX_RECORDS_PER_INPUT},
            "limitations": [
                "Executes one identified original texture-command builder with bounded TMH inputs; no game or GPU was run.",
                "Command slots and synthetic canaries do not establish rendered pixels or visual acceptance.",
                "Only the supported original executable and complete local TMH inventory are certified.",
            ],
        }
        final = stage / "final.json"
        with final.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.link(final, output)
    print(f"Texture-command gate: {totals['input_count']} inputs, "
          f"{totals['descriptor_records']} descriptors, {totals['builder_calls']} builder calls, "
          f"{totals['emitted_command_slots']} command slots; passed")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--layout-report", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--oracle", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    check(**vars(args))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
