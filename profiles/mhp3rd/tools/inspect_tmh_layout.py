#!/usr/bin/env python3
"""Inventory TMH 0.14 record spans without decoding pixels or starting the game.

Field roles remain structural observations until corroborated by the original
consumer. Payload bytes stay in their immutable decoded parents.
"""
from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile


def layout(data: bytes, max_records: int = 4096) -> dict:
    if len(data) < 16 or data[:8] != b".TMH0.14":
        raise ValueError("Unsupported TMH header or version")
    count, reserved = struct.unpack_from("<II", data, 8)
    if count > max_records or count > (len(data) - 16) // 16:
        raise ValueError("Record count exceeds the bounded parent")
    cursor = 16
    records = []
    for index in range(count):
        if cursor + 16 > len(data):
            raise ValueError("Truncated record header")
        size, word4, word8, word12 = struct.unpack_from("<4I", data, cursor)
        if size < 16 or size > len(data) - cursor:
            raise ValueError("Record span escapes TMH parent")
        blocks = []
        block_cursor = cursor + 16
        while block_cursor < cursor + size:
            if len(blocks) >= 32 or cursor + size - block_cursor < 16:
                raise ValueError("Truncated or excessive sub-blocks")
            block_size, tag, format_word, last_word = struct.unpack_from("<4I", data, block_cursor)
            if block_size < 16 or block_size > cursor + size - block_cursor:
                raise ValueError("Sub-block span escapes its record")
            blocks.append({"offset": block_cursor, "size": block_size,
                           "payload_offset": block_cursor + 16, "payload_size": block_size - 16,
                           "tag_word4": tag, "format_word8": format_word, "word12": last_word,
                           "payload_sha256": hashlib.sha256(data[block_cursor + 16:block_cursor + block_size]).hexdigest()})
            block_cursor += block_size
        records.append({"index": index, "offset": cursor, "size": size,
                        "word4": word4, "word8": word8, "word12": word12,
                        "header_word_sum_matches_block_count": word8 + word12 == len(blocks),
                        "blocks": blocks})
        cursor += size
    return {"version": "0.14", "count_word8": count, "reserved_word12": reserved,
            "coordinate_space": "TMH_child_bytes", "records": records,
            "consumed_bytes": cursor, "unindexed_tail_length": len(data) - cursor}


def children(node: dict, base: int = 0, path: tuple = (), depth: int = 0):
    if depth > 2:
        raise ValueError("Bundle inventory nesting exceeds its recorded bound")
    for child in node["children"]:
        if child["status"] == "absent":
            continue
        offset, length, index = child["offset"], child["advertised_length"], child["index"]
        if any(type(v) is not int or v < 0 for v in (offset, length, index)):
            raise ValueError("Invalid child span or identity")
        child_path = path + (index,)
        if child.get("annotation") == "TMH_marker":
            yield base + offset, length, child["sha256"], child_path
        if "nested_candidate" in child:
            yield from children(child["nested_candidate"], base + offset, child_path, depth + 1)


def inspect(workspace: Path, inventory: Path, output: Path) -> dict:
    workspace = workspace.resolve(strict=True)
    if inventory.stat().st_size > 32 * 1024 * 1024:
        raise ValueError("Inventory exceeds metadata budget")
    raw_inventory = inventory.read_bytes()
    audit = json.loads(raw_inventory)
    if audit.get("success") is not True or audit.get("schema_version") != 1:
        raise ValueError("Bundle inventory is not a successful supported audit")
    manifest = workspace / "manifest.json"
    if hashlib.sha256(manifest.read_bytes()).hexdigest() != audit["resource_manifest_sha256"]:
        raise ValueError("Prepared resource manifest differs from audited inventory")
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError("Report exists; retain prior evidence")
    for protected in (workspace / "raw-entries", workspace / "raw-disc"):
        if output == protected or protected.resolve() in output.resolve().parents:
            raise ValueError("Report must not be written into immutable resources")
    rows, failures = [], []
    variants, formats = Counter(), Counter()
    for entry in audit["entries"]:
        candidates = list(children(entry["candidate"])) if "candidate" in entry else []
        ident = entry["id"]
        if type(ident) is not int or not 0 <= ident < 100000:
            raise ValueError("Invalid resource identity")
        source = workspace / "raw-entries" / f"{ident:05d}.bin"
        if source.is_symlink() or source.stat().st_size > 256 * 1024 * 1024:
            raise ValueError("Raw parent is linked or exceeds size budget")
        with source.open("rb") as stream:
            header = stream.read(8)
            if header.startswith(b".TMH"):
                candidates.insert(0, (0, entry["size"], entry["sha256"], ()))
            if not candidates:
                digest = hashlib.sha256(header)
                size = len(header)
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    size += len(chunk)
                    digest.update(chunk)
                if size != entry["size"] or digest.hexdigest() != entry["sha256"]:
                    raise ValueError("Unselected raw parent differs from bundle audit")
                continue
        data = source.read_bytes()
        if len(data) != entry["size"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
            raise ValueError("Raw parent differs from bundle audit")
        for offset, length, digest, path in candidates:
            if len(rows) + len(failures) >= 50000 or offset > len(data) or length > len(data) - offset:
                raise ValueError("TMH candidate budget or parent span exceeded")
            child = data[offset:offset + length]
            if hashlib.sha256(child).hexdigest() != digest:
                raise ValueError("TMH child differs from bundle audit")
            identity = {"entry_id": ident, "child_path": list(path), "sha256": digest,
                        "offset_in_decoded_entry": offset, "size": length,
                        "origin": "indexed_child" if path else "standalone_entry"}
            try:
                parsed = layout(child)
            except ValueError as error:
                failures.append({**identity, "reason": str(error)})
                continue
            rows.append({**identity, **parsed})
            for record in parsed["records"]:
                variants[str((record["word4"], record["word8"], record["word12"]))] += 1
                for block in record["blocks"]:
                    formats[str((block["tag_word4"], block["format_word8"]))] += 1
    report = {"schema_version": 1, "recorded_at": datetime.now(timezone.utc).isoformat(),
              "scope": "structural_TMH_0.14_inventory_not_pixel_or_gameplay_validation",
              "bundle_inventory_sha256": hashlib.sha256(raw_inventory).hexdigest(),
              "resource_manifest_sha256": audit["resource_manifest_sha256"],
              "tool_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "children": rows, "failures": failures, "child_count": len(rows),
              "raw_entries_hashed": len(audit["entries"]),
              "standalone_count": sum(not row["child_path"] for row in rows),
              "indexed_child_count": sum(bool(row["child_path"]) for row in rows),
              "unique_child_hashes": len({row["sha256"] for row in rows}),
              "record_count": sum(row["count_word8"] for row in rows),
              "record_header_variants": dict(variants), "block_tag_format_pairs": dict(formats)}
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="tmh-audit-", dir=output.parent) as temporary:
        staged = Path(temporary) / "report.json"
        with staged.open("w", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.link(staged, output)
    print(f"TMH structural inventory: {len(rows)} children, {report['record_count']} records, {len(failures)} unsupported")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    inspect(args.workspace, args.inventory, args.output)


if __name__ == "__main__":
    main()
