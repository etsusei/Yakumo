#!/usr/bin/env python3
"""Run the bounded native bundle/original-consumer audit on prepared resources.

Only metadata is published. Raw parents remain unchanged; every child offset
belongs to decoded parent bytes, never to an obfuscated ISO coordinate space.
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

MAX_MANIFEST = 32 * 1024 * 1024
MAX_FILE = 256 * 1024 * 1024


def manifest_index(manifest: dict) -> str:
    if not isinstance(manifest, dict) or manifest.get("schema_version") != 1:
        raise ValueError("Unsupported resource manifest")
    entries = manifest.get("entries")
    if (type(entries) is not list or not 1 <= len(entries) <= 100000 or
            manifest.get("archive", {}).get("entry_count") != len(entries)):
        raise ValueError("Invalid archive entry count")
    rows = []
    for expected, entry in enumerate(entries):
        if type(entry) is not dict or type(entry.get("id")) is not int or entry["id"] != expected:
            raise ValueError("Resource IDs must be unique and sequential")
        if entry.get("output_path") != f"raw-entries/{expected:05d}.bin":
            raise ValueError("Resource path differs from numeric identity")
        size, digest = entry.get("extracted_length"), entry.get("sha256")
        if type(size) is not int or not 0 <= size <= MAX_FILE:
            raise ValueError("Resource size exceeds audit limit")
        if type(digest) is not str or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError("Invalid resource digest")
        rows.append(f"{expected}\t{size}\t{digest}\n")
    return "".join(rows)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate manifest key")
        result[key] = value
    return result


def inspect(workspace: Path, elf: Path, oracle: Path, output: Path) -> dict:
    if workspace.is_symlink() or (workspace / "raw-entries").is_symlink():
        raise ValueError("Workspace and raw-entry directory must not be symlinks")
    workspace, elf, oracle = workspace.resolve(strict=True), elf.resolve(strict=True), oracle.resolve(strict=True)
    manifest_path = workspace / "manifest.json"
    if manifest_path.is_symlink() or manifest_path.stat().st_size > MAX_MANIFEST:
        raise ValueError("Resource manifest is linked or too large")
    raw = manifest_path.read_bytes()
    manifest = json.loads(raw, object_pairs_hook=unique_object)
    index = manifest_index(manifest)
    source_hash = manifest.get("source", {}).get("iso", {}).get("sha256")
    if type(source_hash) is not str or not re.fullmatch(r"[0-9a-f]{64}", source_hash):
        raise ValueError("Missing declared source ISO identity")
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError("Output exists; preserve prior evidence")
    # A caller cannot accidentally publish the report inside immutable inputs.
    for protected in (workspace / "raw-entries", workspace / "raw-disc"):
        if output == protected or protected in output.resolve().parents:
            raise ValueError("Output must be outside immutable resource files")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="bundle-audit-", dir=output.parent) as temporary:
        stage = Path(temporary)
        index_path = stage / "index.tsv"
        index_path.write_text(index, encoding="ascii")
        report_path = stage / "report.json"
        run = subprocess.run([str(oracle), str(elf), str(workspace / "raw-entries"),
                              str(index_path), str(report_path)], text=True, capture_output=True,
                             timeout=60, check=True)
        report = json.loads(report_path.read_text())
        if report.get("success") is not True or report.get("files_checked") != len(manifest["entries"]):
            raise ValueError("Oracle did not check the complete manifest")
        if manifest_path.read_bytes() != raw:
            raise ValueError("Resource manifest changed during audit")
        report["recorded_at"] = datetime.now(timezone.utc).isoformat()
        report["resource_manifest_sha256"] = hashlib.sha256(raw).hexdigest()
        report["oracle_sha256"] = hashlib.sha256(oracle.read_bytes()).hexdigest()
        report["source_iso_sha256"] = source_hash
        report["source_identity_scope"] = "ISO identity from the preparation manifest; raw entry bytes rehashed by this audit"
        report["limits"] = {"max_seconds": 60, "max_parent_bytes": MAX_FILE,
                            "max_entries_per_node": 4096, "max_nested_depth": 2,
                            "max_candidate_nodes": 10000, "max_child_slots": 200000,
                            "max_hashed_child_bytes": 2 << 30}
        report["limitations"] = [
            "Bounds and known marker matches do not identify every resource's meaning.",
            "Original accessors see index bytes and return scalar metadata; payload consumers were not run.",
            "Native slices retain their parent storage; the original owner/free boundary is not recovered.",
            "No game launch, live caller coverage, decoder acceptance, or game-loader replacement.",
        ]
        final = stage / "final.json"
        with final.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        # Same-directory staging keeps publication atomic and refuses a raced
        # destination without replacing any earlier report or raw resource.
        os.link(final, output)
        print(run.stdout.strip())
        return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", required=True, type=Path)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--oracle", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    inspect(args.workspace, args.elf, args.oracle, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
