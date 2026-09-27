#!/usr/bin/env python3
"""Check owned encoded TMH views against bounded original descriptor execution."""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def check(workspace: Path, layout_report: Path, elf: Path, oracle: Path, output: Path) -> dict:
    workspace = workspace.resolve(strict=True)
    if layout_report.stat().st_size > 32 * 1024 * 1024:
        raise ValueError("TMH inventory exceeds metadata budget")
    raw = layout_report.read_bytes()
    layout = json.loads(raw)
    manifest_bytes = (workspace / "manifest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    if hashlib.sha256(manifest_bytes).hexdigest() != layout["resource_manifest_sha256"]:
        raise ValueError("Resource manifest changed since layout inventory")
    if layout.get("failures") or not 1 <= len(layout["children"]) <= 50000:
        raise ValueError("Layout contains unsupported inputs or no bounded input set")
    entries = {entry["id"]: entry for entry in manifest["entries"]}
    lines = []
    for row in layout["children"]:
        source = entries[row["entry_id"]]
        path = row["child_path"]
        if type(path) is not list or any(type(index) is not int or index < 0 for index in path):
            raise ValueError("Invalid TMH child path")
        identity = ".".join(map(str, path)) if path else "root"
        lines.append(f"{source['id']}\t{source['extracted_length']}\t{source['sha256']}\t"
                     f"{row['offset_in_decoded_entry']}\t{row['size']}\t{row['sha256']}\t{identity}\n")
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError("Output exists; preserve earlier evidence")
    for protected in (workspace / "raw-entries", workspace / "raw-disc"):
        if protected.resolve() in output.resolve().parents:
            raise ValueError("Output must not enter immutable source directories")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="tmh-view-gate-", dir=output.parent) as temporary:
        stage = Path(temporary)
        index, report = stage / "index.tsv", stage / "oracle.json"
        index.write_text("".join(lines), encoding="ascii")
        result = subprocess.run([str(oracle.resolve(strict=True)), str(elf.resolve(strict=True)),
                                 str(workspace / "raw-entries"), str(index), str(report)],
                                text=True, capture_output=True, timeout=60, check=True)
        value = json.loads(report.read_text())
        if (value.get("success") is not True or value["input_count"] != len(layout["children"]) or
                value["descriptor_calls_per_oracle"] != layout["record_count"]):
            raise ValueError("Descriptor gate did not cover the complete inventory")
        if (workspace / "manifest.json").read_bytes() != manifest_bytes:
            raise ValueError("Manifest changed during descriptor gate")
        value["recorded_at"] = datetime.now(timezone.utc).isoformat()
        value["layout_inventory_sha256"] = hashlib.sha256(raw).hexdigest()
        value["resource_manifest_sha256"] = hashlib.sha256(manifest_bytes).hexdigest()
        value["oracle_sha256"] = hashlib.sha256(oracle.read_bytes()).hexdigest()
        value["tool_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        value["limits"] = {"seconds": 60, "child_bytes": 16 * 1024 * 1024,
                           "records_per_input": 4096, "interpreter_slices_per_record": 8192}
        value["limitations"] = ["Encoded data and descriptors only; no pixel/swizzle conversion or visual acceptance.",
                                "Original descriptor uses separate source/output buffers and palette selector zero.",
                                "No game, object constructor, command builder or GPU was run."]
        final = stage / "final.json"
        with final.open("w", encoding="utf-8") as stream:
            json.dump(value, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.link(final, output)
        print(result.stdout.strip())
        return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("workspace", "layout-report", "elf", "oracle", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    check(**vars(args))


if __name__ == "__main__":
    main()
