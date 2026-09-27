#!/usr/bin/env python3
"""Insert observational lifetime checkpoints into build-local AOT copies.

The fingerprints below describe generated C++ shape, not executable identity.
The callback owner must separately validate the loaded game and overlay code.
Never run this tool in place on the ignored generated corpus.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import tempfile
from pathlib import Path


class TextureLifetimeInstrumentationError(ValueError):
    """The source shape or requested output is unsupported or unsafe."""


SCHEMA = "mhp3rd-texture-lifetime-instrumentation-v1"
_INCLUDE = '#include "native/texture_command_dispatch.hpp"'
_CALLBACK = "mhp3rd::native::texture_lifetime_checkpoint"
_RUNTIME_INCLUDE = re.compile(r'(?m)^#include "psprecomp/runtime\.hpp"(?P<newline>\r?\n)')
_SIGNATURE = re.compile(
    r"\bvoid\s+recomp_unit_(\d{4})_entry\s*\(\s*Runtime\s*&\s*"
    r"([A-Za-z_]\w*)\s*,\s*AllegrexContext\s*&\s*([A-Za-z_]\w*)\s*,"
)
_LABEL = re.compile(r"(?m)^L_([0-9A-F]{8}):(?=\r?$)")

# SHA-256 of each block after removing whitespace, from label to next label.
# These short local shape facts keep generated instructions out of tracked files.
CHECKPOINTS: dict[int, tuple[tuple[str, int, str], ...]] = {
    29: (
        ("HeapInit", 0x08879DA4, "fdd307836f4e36a578c65cd8d7d4d01146a5f30b97e6df83522e5aee3e5eddf0"),
        ("HeapReset", 0x08879D58, "5b814092dcd7a13aad2013422721376eea832f28ca0805cb997c4554a1c07f1f"),
        ("ForwardAllocate", 0x08879DB4, "a7d795f58dd987b1b06c1620820015a23d2e7a15e84c9b98d9dd8df15d7220b6"),
        ("ReverseAllocate", 0x08879F08, "688a334e5a56a4ba5fbcd8b4b25acf58d810407bfed6ef25384b82741749a130"),
        ("Free", 0x08879FF0, "834b216b3ef97b9232dba82068eeb0806d0691a504bb1eda9fa5d84092d00e6b"),
    ),
    40: (
        ("OwnerReset", 0x088A53C8, "00ee7f7859e3cd27f003b49076239ac7b5c1e6229fff99854dc6b54f776bbcd5"),
    ),
    43: (
        ("CallerTail", 0x088B0398, "bc71f78c3dbead49c251f9da9e3b76b17623fbddf2dce075ec419d3d10d78bca"),
        ("ProviderResult", 0x088B03B0, "85d9e251d6e151de1070ec5df82410d2cd14c31a18e29e2feb3168da2dcb858f"),
        ("ChildResult", 0x088B03BC, "ef186715e24eba827f0f1b0f684ff65426ecdd67002a5eb48fea750071e09da2"),
        ("CommandAllocationResult", 0x088B03E4, "ebdd09e99cab72a10a9c43c72947190d00c23195071d6683d119d2a44a5b15a3"),
        ("BuilderCall", 0x088B03F8, "95dee2882ee62c9744fcb294758557f5eca77eb033ad94cc2f0574e51fb47387"),
    ),
    46: (
        ("FactoryAllocationResult", 0x088BD07C, "90102cbc52dbfcf8f6f1d38ec776b1bf868c7ef1f23cbd392d581d0d81a7873a"),
        ("FactoryConstructorCall", 0x088BD13C, "b140b086848c19b0a1343163fc015393518f869fa18ac03c3d5bcab72bffe8ca"),
        ("FactoryConstructorResult", 0x088BD144, "4f499cfac9dca40ae0417fc48572eed7e1296b21ddc95a8071920606c941aa42"),
    ),
}

# Post-call checkpoints also pin the nearby original call, including its
# generated delay-slot statement. A result label alone cannot prove the call.
CALL_PREDECESSORS = {
    0x088B03B0: (0x088B03A8, "8b6860aa9b18795fb86b8c2e30c28c8b16b4a59a42f1b9dee776ebd2f57b9c83"),
    0x088B03BC: (0x088B03B4, "ee4642b4b132abbdc7cfdd28bc47202e11e49799e91b387be4a26d0a73c79c40"),
    0x088B03E4: (0x088B03DC, "47e8f21d76ae01d2d2b342dde25374900f16756b4ec8cfa92a79519e00ea1fdc"),
    0x088BD07C: (0x088BD074, "946649ac8d8c02a283e9923e75411f7c45e9bae94d8617ae0684881e7590ccbd"),
}


def _single(matches: list[re.Match[str]], description: str) -> re.Match[str]:
    if len(matches) != 1:
        raise TextureLifetimeInstrumentationError(
            f"expected exactly one {description}; found {len(matches)}"
        )
    return matches[0]


def _after_line(source: str, match: re.Match[str]) -> int:
    end = source.find("\n", match.end())
    if end < 0:
        raise TextureLifetimeInstrumentationError("instruction label has no following line")
    return end + 1


def instrument_source(source: str) -> tuple[str, dict]:
    """Return an instrumented source and manifest without writing files."""
    if _CALLBACK in source or _INCLUDE in source:
        raise TextureLifetimeInstrumentationError("source already has lifetime instrumentation")
    signature = _single(list(_SIGNATURE.finditer(source)), "generated unit entry signature")
    unit = int(signature.group(1))
    if unit not in CHECKPOINTS:
        raise TextureLifetimeInstrumentationError(f"unsupported generated unit {unit:04d}")
    if signature.group(2, 3) != ("rt", "ctx"):
        raise TextureLifetimeInstrumentationError("generated runtime/context names changed")

    include = _single(list(_RUNTIME_INCLUDE.finditer(source)), "runtime include")
    newline = include.group("newline")
    base = 0x08878000 + (unit - 29) * 0x4000
    registration = re.compile(
        rf"\bruntime\.register_generated_unit\({unit}u,\s*0x{base:08X}u,\s*"
        rf"16384u,\s*&recomp_unit_{unit:04d},\s*&recomp_unit_{unit:04d}_entry\);"
    )
    _single(list(registration.finditer(source)), f"unit {unit:04d} registration")

    ordered = list(_LABEL.finditer(source))
    by_address: dict[int, list[int]] = {}
    for index, match in enumerate(ordered):
        by_address.setdefault(int(match.group(1), 16), []).append(index)

    insertions: list[tuple[int, str]] = [(include.end(), _INCLUDE + newline)]
    manifest_entries = []
    for name, address, expected_hash in CHECKPOINTS[unit]:
        index = _single_index(by_address.get(address, []), f"label L_{address:08X}")
        if index == 0 or index + 1 == len(ordered):
            raise TextureLifetimeInstrumentationError(f"label L_{address:08X} has no neighbors")
        previous = int(ordered[index - 1].group(1), 16)
        following = int(ordered[index + 1].group(1), 16)
        if previous != address - 4 or following != address + 4:
            raise TextureLifetimeInstrumentationError(
                f"instruction labels around 0x{address:08X} changed"
            )
        start = _after_line(source, ordered[index])
        block = source[start:ordered[index + 1].start()]
        fingerprint = hashlib.sha256(re.sub(r"\s+", "", block).encode("utf-8")).hexdigest()
        if fingerprint != expected_hash:
            raise TextureLifetimeInstrumentationError(
                f"instruction block changed at 0x{address:08X}"
            )
        if address in CALL_PREDECESSORS:
            call_address, call_hash = CALL_PREDECESSORS[address]
            call_index = _single_index(
                by_address.get(call_address, []), f"call label L_{call_address:08X}"
            )
            if call_index != index - 2:
                raise TextureLifetimeInstrumentationError(
                    f"call layout changed before 0x{address:08X}"
                )
            call_start = _after_line(source, ordered[call_index])
            call_block = source[call_start:ordered[call_index + 1].start()]
            call_fingerprint = hashlib.sha256(
                re.sub(r"\s+", "", call_block).encode("utf-8")
            ).hexdigest()
            if call_fingerprint != call_hash:
                raise TextureLifetimeInstrumentationError(
                    f"call block changed before 0x{address:08X}"
                )
        callback = (
            f"    {_CALLBACK}(rt, ctx, "
            f"mhp3rd::native::TextureLifetimeCheckpoint::{name});{newline}"
        )
        insertions.append((start, callback))
        manifest_entries.append({"name": name, "address": f"0x{address:08X}"})

    output = source
    for offset, addition in sorted(insertions, key=lambda item: item[0], reverse=True):
        output = output[:offset] + addition + output[offset:]
    return output, {
        "schema": SCHEMA,
        "unit": f"generated_unit_{unit:04d}",
        "checkpoints": manifest_entries,
        "checkpoint_calls": len(manifest_entries),
    }


def _single_index(indexes: list[int], description: str) -> int:
    if len(indexes) != 1:
        raise TextureLifetimeInstrumentationError(
            f"expected exactly one {description}; found {len(indexes)}"
        )
    return indexes[0]


def _check_source(path: Path) -> None:
    if path.is_symlink():
        raise TextureLifetimeInstrumentationError(f"source is a symlink: {path}")
    try:
        mode = path.stat().st_mode
    except OSError as exc:
        raise TextureLifetimeInstrumentationError(f"cannot read source: {exc}") from exc
    if not stat.S_ISREG(mode):
        raise TextureLifetimeInstrumentationError(f"source is not a regular file: {path}")


def _same_file(left: Path, right: Path) -> bool:
    if left.resolve() == right.resolve():
        return True
    return left.exists() and right.exists() and os.path.samefile(left, right)


def _check_destination(path: Path, *, unit: str, manifest: bool) -> None:
    if path.is_symlink():
        raise TextureLifetimeInstrumentationError(f"destination is a symlink: {path}")
    if not path.exists():
        return
    if not stat.S_ISREG(path.stat().st_mode):
        raise TextureLifetimeInstrumentationError(f"destination is not a regular file: {path}")
    contents = path.read_bytes()
    owned = False
    if manifest:
        try:
            record = json.loads(contents)
            expected_entries = [
                {"name": name, "address": f"0x{address:08X}"}
                for name, address, _ in CHECKPOINTS[int(unit[-4:])]
            ]
            owned = (
                isinstance(record, dict)
                and set(record) == {
                    "schema", "unit", "checkpoints", "checkpoint_calls",
                    "source_sha256", "output_sha256",
                }
                and record["schema"] == SCHEMA
                and record["unit"] == unit
                and record["checkpoints"] == expected_entries
                and record["checkpoint_calls"] == len(expected_entries)
                and all(
                    isinstance(record[key], str)
                    and re.fullmatch(r"[0-9a-f]{64}", record[key])
                    for key in ("source_sha256", "output_sha256")
                )
            )
        except (UnicodeDecodeError, ValueError, AttributeError):
            pass
    else:
        try:
            previous = contents.decode("utf-8")
            # Invert only the exact lines this tool adds. Re-instrumenting the
            # reconstructed source must reproduce the whole existing output.
            stripped = re.sub(
                rf"(?m)^[ \t]*{re.escape(_CALLBACK)}\(rt, ctx, "
                r"mhp3rd::native::TextureLifetimeCheckpoint::[A-Za-z]+\);\r?\n",
                "", previous,
            )
            stripped = re.sub(rf"(?m)^{re.escape(_INCLUDE)}\r?\n", "", stripped)
            reproduced, previous_manifest = instrument_source(stripped)
            owned = reproduced == previous and previous_manifest["unit"] == unit
        except (UnicodeDecodeError, TextureLifetimeInstrumentationError):
            pass
    if not owned:
        raise TextureLifetimeInstrumentationError(f"refusing to overwrite an unrelated file: {path}")


def _atomic_write(path: Path, content: bytes) -> None:
    if path.exists() and path.read_bytes() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def instrument_file(input_path: Path, output_path: Path,
                    manifest_path: Path | None = None) -> dict:
    _check_source(input_path)
    if _same_file(input_path, output_path):
        raise TextureLifetimeInstrumentationError("source and output must be different files")
    if manifest_path is not None and (
        _same_file(input_path, manifest_path) or _same_file(output_path, manifest_path)
    ):
        raise TextureLifetimeInstrumentationError("manifest must be separate from source and output")

    source_bytes = input_path.read_bytes()
    try:
        source = source_bytes.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise TextureLifetimeInstrumentationError("generated source is not UTF-8") from exc
    output, manifest = instrument_source(source)
    output_bytes = output.encode("utf-8")
    manifest["source_sha256"] = hashlib.sha256(source_bytes).hexdigest()
    manifest["output_sha256"] = hashlib.sha256(output_bytes).hexdigest()
    _check_destination(output_path, unit=manifest["unit"], manifest=False)
    if manifest_path is not None:
        _check_destination(manifest_path, unit=manifest["unit"], manifest=True)

    _atomic_write(output_path, output_bytes)
    if manifest_path is not None:
        _atomic_write(manifest_path, (json.dumps(manifest, sort_keys=True, indent=2) + "\n").encode("utf-8"))
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="ignored or build-local AOT source")
    parser.add_argument("--output", type=Path, required=True, help="build-local instrumented copy")
    parser.add_argument("--manifest", type=Path, help="optional build-local JSON manifest")
    args = parser.parse_args(argv)
    try:
        manifest = instrument_file(args.input, args.output, args.manifest)
    except (OSError, TextureLifetimeInstrumentationError) as exc:
        parser.error(str(exc))
    print(f"instrumented {manifest['checkpoint_calls']} lifetime checkpoints in {manifest['unit']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
