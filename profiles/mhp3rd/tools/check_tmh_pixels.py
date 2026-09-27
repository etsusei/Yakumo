#!/usr/bin/env python3
"""Gate all supported TMH pixels against both frozen software decode routes.

Only immutable decoded parents are read. Each oracle process has a 60-second
limit; missing bytes beyond a root parent are recorded, never synthesized.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


MAX_METADATA = 32 * 1024 * 1024
MAX_PARENT = 256 * 1024 * 1024
MAX_CHILD = 16 * 1024 * 1024
MAX_INPUTS_PER_SHARD = 48
MAX_RECORDS_PER_SHARD = 256
PROCESS_SECONDS = 60


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_metadata(path: Path) -> tuple[bytes, dict]:
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_METADATA:
        raise ValueError(f"Missing, linked, or oversized metadata: {path}")
    raw = path.read_bytes()
    if len(raw) > MAX_METADATA:
        raise ValueError(f"Metadata grew during read: {path}")
    value = json.loads(raw)
    if type(value) is not dict:
        raise ValueError(f"Metadata must be an object: {path}")
    return raw, value


def source_hashes(oracle: Path) -> dict:
    host = Path(__file__).resolve().parent.parent / "host"
    sources = {
        "legacy_texture_decode_cpp": host / "gpu" / "texture_decode.cpp",
        "portable_pixel_decode_cpp": host / "resources" / "pixel_decode.cpp",
        "tmh_pixel_plan_cpp": host / "resources" / "tmh_pixel_plan.cpp",
        "tmh_view_cpp": host / "resources" / "tmh.cpp",
        "tmh_pixel_oracle_cpp": Path(__file__).resolve().parent.parent / "tests" / "tmh_pixel_oracle.cpp",
        "check_tmh_pixels_py": Path(__file__).resolve(),
        "tmh_pixel_oracle_binary": oracle,
    }
    return {name: digest(path.read_bytes()) for name, path in sources.items()}


def validate_and_index(layout: dict, manifest: dict) -> tuple[list[tuple[dict, str]], int]:
    if layout.get("schema_version") != 1 or layout.get("failures") or not layout.get("children"):
        raise ValueError("TMH layout is not a successful bounded inventory")
    if manifest.get("schema_version") != 1 or type(manifest.get("entries")) is not list:
        raise ValueError("Unsupported resource manifest")
    entries = {entry["id"]: entry for entry in manifest["entries"]}
    if len(entries) != len(manifest["entries"]):
        raise ValueError("Duplicate resource manifest identity")
    result: list[tuple[dict, str]] = []
    identities: set[tuple[int, str]] = set()
    total_records = 0
    for row in layout["children"]:
        ident = row["entry_id"]
        source = entries.get(ident)
        if (type(ident) is not int or not 0 <= ident < 100000 or source is None or
                source.get("output_path") != f"raw-entries/{ident:05d}.bin"):
            raise ValueError("TMH layout names an unsafe or absent raw entry")
        path = row["child_path"]
        if type(path) is not list or any(type(value) is not int or value < 0 for value in path):
            raise ValueError("Malformed TMH child path")
        child_id = ".".join(map(str, path)) if path else "root"
        if (ident, child_id) in identities:
            raise ValueError("Duplicate TMH child identity")
        identities.add((ident, child_id))
        parent_size, offset, length = source["extracted_length"], row["offset_in_decoded_entry"], row["size"]
        if (any(type(value) is not int for value in (parent_size, offset, length)) or
                not 0 <= parent_size <= MAX_PARENT or not 0 <= offset <= parent_size or
                not 0 <= length <= min(MAX_CHILD, parent_size - offset)):
            raise ValueError("TMH child span exceeds its decoded parent or a budget")
        record_count = row["count_word8"]
        if (type(record_count) is not int or not 0 <= record_count <= 4096 or
                type(row.get("records")) is not list or len(row["records"]) != record_count):
            raise ValueError("TMH record inventory is inconsistent")
        for value in (source["sha256"], row["sha256"]):
            if type(value) is not str or len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
                raise ValueError("Malformed SHA-256 identity")
        line = (f"{ident}\t{parent_size}\t{source['sha256']}\t{offset}\t{length}\t"
                f"{row['sha256']}\t{child_id}\n")
        result.append((row, line))
        total_records += record_count
    if (layout["child_count"] != len(result) or layout["record_count"] != total_records or
            len(result) > 50000):
        raise ValueError("TMH layout totals disagree with its entries")
    return result, total_records


def shard_rows(rows: list[tuple[dict, str]]) -> list[list[tuple[dict, str]]]:
    shards: list[list[tuple[dict, str]]] = []
    current: list[tuple[dict, str]] = []
    records = 0
    for row in rows:
        next_records = row[0]["count_word8"]
        if current and (len(current) >= MAX_INPUTS_PER_SHARD or
                        records + next_records > MAX_RECORDS_PER_SHARD):
            shards.append(current)
            current, records = [], 0
        current.append(row)
        records += next_records
    if current:
        shards.append(current)
    return shards


def run_oracle(oracle: Path, raw_entries: Path, index: Path, report: Path, mode: str) -> dict:
    environment = os.environ.copy()
    for name in tuple(environment):
        if name.startswith("MHP3RD_"):
            environment.pop(name)
    if mode == "slow":
        environment["MHP3RD_NO_FAST_TEXTURE_DECODE"] = "1"
    try:
        completed = subprocess.run(
            [str(oracle), str(raw_entries), str(index), mode, str(report)],
            env=environment, text=True, capture_output=True, timeout=PROCESS_SECONDS,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise ValueError(f"TMH pixel {mode} shard exceeded {PROCESS_SECONDS} seconds") from error
    if completed.returncode not in (0, 2):
        raise ValueError(f"TMH pixel {mode} shard failed ({completed.returncode}): "
                         f"{completed.stderr[-2000:]}")
    raw, value = read_metadata(report)
    if (not raw or value.get("schema_version") != 1 or
            value.get("legacy_mode") != mode or type(value.get("inputs")) is not list or
            type(value.get("counts")) is not dict or
            value.get("success") is not (completed.returncode == 0)):
        raise ValueError(f"Malformed TMH pixel {mode} shard report")
    return value


def validate_shard(value: dict, expected: list[tuple[dict, str]]) -> None:
    if len(value["inputs"]) != len(expected):
        raise ValueError("TMH pixel shard omitted inputs")
    actual_records = 0
    for produced, (layout_row, _) in zip(value["inputs"], expected):
        if (produced["entry_id"] != layout_row["entry_id"] or
                produced["child_sha256"] != layout_row["sha256"] or
                produced["offset_in_decoded_entry"] != layout_row["offset_in_decoded_entry"] or
                produced["child_size"] != layout_row["size"] or
                produced["child_id"] != (".".join(map(str, layout_row["child_path"]))
                                         if layout_row["child_path"] else "root")):
            raise ValueError("TMH pixel shard identity differs from layout")
        if len(produced["records"]) != layout_row["count_word8"]:
            raise ValueError("TMH pixel shard omitted records")
        for index, record in enumerate(produced["records"]):
            if record["record_index"] != index or record["rectangle"]["status"] != "compared":
                raise ValueError("TMH pixel shard lost a rectangle comparison")
            if record["canvas"]["status"] not in ("compared", "missing_neighbor_context"):
                raise ValueError("TMH pixel shard has an unknown canvas status")
            actual_records += 1
    if (value["counts"]["inputs"] != len(expected) or
            value["counts"]["records"] != actual_records or
            value["counts"]["rectangle_compared"] != actual_records or
            value["counts"]["canvas_compared"] +
            value["counts"]["missing_neighbor_context"] != actual_records):
        raise ValueError("TMH pixel shard totals differ from detailed coverage")


def mode_differences(fast: dict, slow: dict) -> list[dict]:
    differences = []
    for fast_input, slow_input in zip(fast["inputs"], slow["inputs"]):
        for fast_record, slow_record in zip(fast_input["records"], slow_input["records"]):
            for domain in ("rectangle", "canvas"):
                first, second = fast_record[domain], slow_record[domain]
                if first["status"] != second["status"] or (
                    first["status"] == "compared" and
                    (first["portable_rgba_sha256_le"] != second["portable_rgba_sha256_le"] or
                     first["legacy_rgba_sha256_le"] != second["legacy_rgba_sha256_le"])
                ):
                    differences.append({"entry_id": fast_input["entry_id"],
                                        "child_id": fast_input["child_id"],
                                        "record_index": fast_record["record_index"], "domain": domain,
                                        "fast_status": first["status"], "slow_status": second["status"],
                                        "fast_legacy_rgba_sha256_le": first.get("legacy_rgba_sha256_le"),
                                        "slow_legacy_rgba_sha256_le": second.get("legacy_rgba_sha256_le")})
    return differences


def check(workspace: Path, layout_report: Path, oracle: Path, output: Path) -> dict:
    workspace = workspace.resolve(strict=True)
    layout_report = layout_report.resolve(strict=True)
    oracle = oracle.resolve(strict=True)
    if oracle.is_symlink() or not oracle.is_file():
        raise ValueError("Missing TMH pixel oracle executable")
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError("Output exists; preserve earlier evidence")
    for protected in (workspace / "raw-entries", workspace / "raw-disc"):
        if output == protected or protected.resolve() in output.resolve().parents:
            raise ValueError("Output must not enter immutable source directories")
    layout_bytes, layout = read_metadata(layout_report)
    manifest_path = workspace / "manifest.json"
    manifest_bytes, manifest = read_metadata(manifest_path)
    if digest(manifest_bytes) != layout.get("resource_manifest_sha256"):
        raise ValueError("Resource manifest changed since TMH layout inventory")
    rows, records = validate_and_index(layout, manifest)
    # These counts identify the supported local image and prevent a partial
    # inventory from being mislabeled as the complete corpus gate.
    if len(rows) != 2256 or records != 8866:
        raise ValueError("TMH layout is not the complete supported corpus")
    hashes_before = source_hashes(oracle)
    output.parent.mkdir(parents=True, exist_ok=True)
    all_inputs = []
    totals: dict[str, int] = {}
    mode_disagreements = []
    complete_shards = shard_rows(rows)
    with tempfile.TemporaryDirectory(prefix="tmh-pixel-gate-", dir=output.parent) as temporary:
        stage = Path(temporary)
        for number, shard in enumerate(complete_shards):
            index = stage / f"index-{number:04d}.tsv"
            index.write_text("".join(line for _, line in shard), encoding="ascii")
            fast = run_oracle(oracle, workspace / "raw-entries", index,
                              stage / f"fast-{number:04d}.json", "fast")
            slow = run_oracle(oracle, workspace / "raw-entries", index,
                              stage / f"slow-{number:04d}.json", "slow")
            validate_shard(fast, shard)
            validate_shard(slow, shard)
            if fast["counts"] != slow["counts"]:
                raise ValueError("Fast and slow TMH pixel shard counts differ")
            mode_disagreements.extend(mode_differences(fast, slow))
            all_inputs.extend(fast["inputs"])
            for name, amount in fast["counts"].items():
                totals[name] = totals.get(name, 0) + amount
        if (manifest_path.read_bytes() != manifest_bytes or
                layout_report.read_bytes() != layout_bytes or
                source_hashes(oracle) != hashes_before):
            raise ValueError("An input manifest, layout, source, or oracle changed during the gate")
        if (totals["inputs"] != len(rows) or totals["records"] != records or
                totals["rectangle_compared"] != records or
                totals["canvas_compared"] + totals["missing_neighbor_context"] != records):
            raise ValueError("Final TMH pixel coverage is incomplete")
        expected = {"canvas_compared": 8856, "missing_neighbor_context": 10,
                    "canvas_after_image": 83, "canvas_after_tmh": 34,
                    "compatibility_bypasses": 4, "strict_rejections": 4}
        observed_profile = all(totals[name] == count for name, count in expected.items())
        success = (observed_profile and totals["pixel_mismatches"] == 0 and
                   totals["top_left_differences"] == 0 and not mode_disagreements)
        report = {
            "schema_version": 1,
            "recorded_at": datetime.now(timezone.utc).isoformat(),
            "scope": "offline_software_TMH_pixel_differential_not_hardware_or_gameplay",
            "success": success,
            "resource_manifest_sha256": digest(manifest_bytes),
            "layout_inventory_sha256": digest(layout_bytes),
            "source_sha256": hashes_before,
            "legacy_routes": ["fast", "slow"],
            "limits": {"seconds_per_process": PROCESS_SECONDS,
                       "max_inputs_per_shard": MAX_INPUTS_PER_SHARD,
                       "max_records_per_shard": MAX_RECORDS_PER_SHARD,
                       "parent_bytes": MAX_PARENT, "child_bytes": MAX_CHILD},
            "expected_supported_corpus": {"inputs": 2256, "records": 8866, **expected},
            "counts": totals,
            "mode_disagreements": mode_disagreements,
            "inputs": all_inputs,
            "limitations": [
                "Compares the current software decoder with the portable byte-window decoder; no PSP or GPU was run.",
                "The original builder's canvas dimensions are modeled from its verified power-of-two table; actual rendering and UV sampling were not observed.",
                "Canvases ending beyond the immutable decoded root entry are missing_neighbor_context; no neighboring bytes were invented.",
                "Unaligned swizzle uses an explicit legacy compatibility bypass; strict swizzle rejects those records.",
                "Top-left rectangle/canvas comparison is made only when block grouping, row sampling, and effective swizzle mapping agree.",
            ],
        }
        final = stage / "final.json"
        with final.open("w", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.link(final, output)
    print(f"TMH pixel gate: {totals['inputs']} inputs, {totals['rectangle_compared']} rectangles, "
          f"{totals['canvas_compared']} canvases, {totals['missing_neighbor_context']} missing "
          f"neighbor windows; {'passed' if success else 'FAILED'}")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--layout-report", type=Path, required=True)
    parser.add_argument("--oracle", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = check(**vars(args))
    if not result["success"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
