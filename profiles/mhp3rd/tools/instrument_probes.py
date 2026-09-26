#!/usr/bin/env python3
"""Insert observational callbacks into local AOT copies of certified leaves.

This checks generated C++ shape only. The recorder must separately check the
loaded executable and every full leaf span before calling a probe certified.
The source corpus is derived from the user's game and must remain unmodified.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import tempfile
from dataclasses import dataclass
from pathlib import Path


class ProbeInstrumentationError(ValueError):
    """The requested source, anchor, or output is unsafe or unsupported."""


@dataclass(frozen=True)
class Leaf:
    size: int
    returns: tuple[int, ...]
    store_delay_slot: bool = False


# Addresses and return offsets are certified format facts, not game code.
LEAVES: dict[int, Leaf] = {
    0x088775AC: Leaf(100, (0x48, 0x5C)),
    0x08877818: Leaf(24, (0x10,)),
    0x08878B28: Leaf(36, (0x1C,), True),
    0x08878B4C: Leaf(36, (0x1C,), True),
    0x08879D08: Leaf(80, (0x48,)),
}

_LABEL = re.compile(r"(?m)^L_([0-9A-F]{8}):(?=\r?$)")
_ENTRY_SIGNATURE = re.compile(
    r"\bvoid\s+recomp_unit_\d+_entry\s*\(\s*Runtime\s*&\s*([A-Za-z_]\w*)\s*,\s*"
    r"AllegrexContext\s*&\s*([A-Za-z_]\w*)\s*,"
)
_RUNTIME_INCLUDE = re.compile(r'(?m)^#include "psprecomp/runtime\.hpp"(?P<newline>\r?\n)')
_ADDRESS = re.compile(r"0[xX][0-9a-fA-F]{1,8}\Z")
_INCLUDE = '#include "testing/probes.hpp"'
_ENTER = "native_probe_aot_enter"
_EXIT = "native_probe_aot_exit"
_MANIFEST_SCHEMA = "mhp3rd-probe-instrumentation-v1"


def parse_entries(value: str) -> tuple[int, ...]:
    parts = value.split(",")
    if not parts or any(not _ADDRESS.fullmatch(part) for part in parts):
        raise ProbeInstrumentationError("--entries needs comma-separated 0x-prefixed hex addresses")
    entries = tuple(int(part, 16) for part in parts)
    if len(set(entries)) != len(entries):
        raise ProbeInstrumentationError("duplicate requested probe entry")
    unknown = [entry for entry in entries if entry not in LEAVES]
    if unknown:
        raise ProbeInstrumentationError(f"unsupported probe entry 0x{unknown[0]:08X}")
    return entries


def _single(matches: list[re.Match[str]], description: str) -> re.Match[str]:
    if len(matches) != 1:
        raise ProbeInstrumentationError(f"expected exactly one {description}; found {len(matches)}")
    return matches[0]


def _line_after(source: str, match: re.Match[str]) -> int:
    end = source.find("\n", match.end())
    if end < 0:
        raise ProbeInstrumentationError("probe label has no following line")
    return end + 1


def _check_return_block(block: str, entry: int, return_address: int, leaf: Leaf) -> int:
    lines = [line.strip() for line in block.splitlines() if line.strip()]
    expected_tail = [
        "local_pc = jump_target;",
        "if (++local_transfers < 2048u) { entry_id = 0u; goto LOCAL_DISPATCH; }",
        "ctx.pc = jump_target;",
        "return;",
    ]
    if len(lines) != 6 or lines[0] != "jump_target = ctx.gpr[31];" or lines[2:] != expected_tail:
        raise ProbeInstrumentationError(
            f"return structure changed at 0x{return_address:08X} for 0x{entry:08X}"
        )
    delay_slot = lines[1]
    if leaf.store_delay_slot:
        if not (delay_slot.startswith("aot_mem.aot_store32(") and delay_slot.endswith(");")):
            raise ProbeInstrumentationError(f"expected a store delay slot at 0x{return_address:08X}")
    elif delay_slot != "// nop":
        raise ProbeInstrumentationError(f"expected a nop delay slot at 0x{return_address:08X}")
    local_pc = re.findall(r"(?m)^[ \t]*local_pc = jump_target;\r?\n", block)
    if len(local_pc) != 1:
        raise ProbeInstrumentationError(f"return transfer anchor changed at 0x{return_address:08X}")
    match = re.search(r"(?m)^[ \t]*local_pc = jump_target;\r?\n", block)
    assert match is not None
    return match.start()


def instrument_source(source: str, entries: tuple[int, ...]) -> tuple[str, dict]:
    if not entries:
        raise ProbeInstrumentationError("at least one probe entry is required")
    if len(set(entries)) != len(entries) or any(entry not in LEAVES for entry in entries):
        raise ProbeInstrumentationError("requested probe entries must be distinct supported addresses")
    if _INCLUDE in source or _ENTER in source or _EXIT in source:
        raise ProbeInstrumentationError("source is already instrumented")

    signature = _single(list(_ENTRY_SIGNATURE.finditer(source)), "generated entry signature")
    runtime_name, context_name = signature.groups()
    if context_name != "ctx":
        raise ProbeInstrumentationError("generated context name changed")
    include = _single(list(_RUNTIME_INCLUDE.finditer(source)), "runtime include")
    newline = include.group("newline")

    labels: dict[int, list[re.Match[str]]] = {}
    ordered_labels = list(_LABEL.finditer(source))
    for match in ordered_labels:
        labels.setdefault(int(match.group(1), 16), []).append(match)
    if not ordered_labels:
        raise ProbeInstrumentationError("no generated instruction labels found")
    next_label = {match.start(): ordered_labels[i + 1].start()
                  for i, match in enumerate(ordered_labels[:-1])}
    insertions: list[tuple[int, str]] = [(include.end(), _INCLUDE + newline)]
    entry_total = exit_total = 0

    for entry in entries:
        leaf = LEAVES[entry]
        boundary = entry + leaf.size
        for address in range(entry, boundary + 4, 4):
            _single(labels.get(address, []), f"label L_{address:08X}")
        first = labels[entry][0]
        last = labels[boundary][0]
        span = source[first.start():last.start()]
        if len(re.findall(r"(?m)^[ \t]*return;\r?$", span)) != len(leaf.returns):
            raise ProbeInstrumentationError(f"unexpected return count in 0x{entry:08X} span")
        if span.count("jump_target = ctx.gpr[31];") != len(leaf.returns):
            raise ProbeInstrumentationError(f"unexpected return target in 0x{entry:08X} span")

        insertions.append((
            _line_after(source, first),
            f"    mhp3rd::testing::{_ENTER}({runtime_name}, ctx, 0x{entry:08X}u);{newline}",
        ))
        entry_total += 1
        for offset in leaf.returns:
            return_address = entry + offset
            anchor = _single(labels.get(return_address, []), f"return label L_{return_address:08X}")
            block_start = _line_after(source, anchor)
            block_end = next_label.get(anchor.start())
            if block_end is None or block_end > last.start():
                raise ProbeInstrumentationError(f"return block escaped 0x{entry:08X} span")
            relative_exit = _check_return_block(
                source[block_start:block_end], entry, return_address, leaf
            )
            insertions.append((
                block_start + relative_exit,
                f"    mhp3rd::testing::{_EXIT}({runtime_name}, ctx, 0x{entry:08X}u, jump_target);{newline}",
            ))
            exit_total += 1

    output = source
    for at, addition in sorted(insertions, key=lambda item: item[0], reverse=True):
        output = output[:at] + addition + output[at:]
    return output, {
        "schema": _MANIFEST_SCHEMA,
        "entries": [
            {"address": f"0x{entry:08X}", "entry_calls": 1, "exit_calls": len(LEAVES[entry].returns)}
            for entry in entries
        ],
        "entry_calls": entry_total,
        "exit_calls": exit_total,
    }


def _check_source(path: Path) -> None:
    if path.is_symlink():
        raise ProbeInstrumentationError(f"source is a symlink: {path}")
    try:
        mode = path.stat().st_mode
    except OSError as exc:
        raise ProbeInstrumentationError(f"cannot read source: {exc}") from exc
    if not stat.S_ISREG(mode):
        raise ProbeInstrumentationError(f"source is not a regular file: {path}")


def _check_destination(path: Path, *, manifest: bool = False) -> None:
    if path.is_symlink():
        raise ProbeInstrumentationError(f"destination is a symlink: {path}")
    if not path.exists():
        return
    if not stat.S_ISREG(path.stat().st_mode):
        raise ProbeInstrumentationError(f"destination is not a regular file: {path}")
    contents = path.read_bytes()
    if manifest:
        try:
            owned = json.loads(contents).get("schema") == _MANIFEST_SCHEMA
        except (UnicodeDecodeError, ValueError, AttributeError):
            owned = False
    else:
        owned = all(marker in contents for marker in (
            _INCLUDE.encode("ascii"), _ENTER.encode("ascii"), _EXIT.encode("ascii")
        ))
    if not owned:
        raise ProbeInstrumentationError(f"refusing to overwrite an unrelated file: {path}")


def _same_file(left: Path, right: Path) -> bool:
    if left.resolve() == right.resolve():
        return True
    return left.exists() and right.exists() and os.path.samefile(left, right)


def _atomic_write(path: Path, content: bytes) -> bool:
    if path.exists() and path.read_bytes() == content:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return True


def instrument_file(input_path: Path, output_path: Path, entries: tuple[int, ...],
                    manifest_path: Path | None = None) -> dict:
    _check_source(input_path)
    if _same_file(input_path, output_path):
        raise ProbeInstrumentationError("source and output must be different files")
    if manifest_path is not None and (
        _same_file(input_path, manifest_path) or _same_file(output_path, manifest_path)
    ):
        raise ProbeInstrumentationError("manifest must be separate from source and output")
    _check_destination(output_path)
    if manifest_path is not None:
        _check_destination(manifest_path, manifest=True)

    source_bytes = input_path.read_bytes()
    try:
        source = source_bytes.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ProbeInstrumentationError("generated source is not UTF-8") from exc
    output, manifest = instrument_source(source, entries)
    output_bytes = output.encode("utf-8")
    manifest["source_sha256"] = hashlib.sha256(source_bytes).hexdigest()
    manifest["output_sha256"] = hashlib.sha256(output_bytes).hexdigest()
    _atomic_write(output_path, output_bytes)
    if manifest_path is not None:
        manifest_bytes = (json.dumps(manifest, sort_keys=True, indent=2) + "\n").encode("utf-8")
        _atomic_write(manifest_path, manifest_bytes)
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="ignored generated AOT source")
    parser.add_argument("--output", type=Path, required=True, help="build-local instrumented copy")
    parser.add_argument("--entries", required=True, help="comma-separated certified 0x addresses")
    parser.add_argument("--manifest", type=Path, help="optional build-local JSON manifest")
    args = parser.parse_args(argv)
    try:
        entries = parse_entries(args.entries)
        manifest = instrument_file(args.input, args.output, entries, args.manifest)
    except (OSError, ProbeInstrumentationError) as exc:
        parser.error(str(exc))
    print(f"instrumented {manifest['entry_calls']} entries and {manifest['exit_calls']} exits")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
