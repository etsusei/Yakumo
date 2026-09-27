#!/usr/bin/env python3
"""Instrument a build-local copy of the certified texture-command AOT unit.

This validates generated C++ anchors, not the loaded game's instruction bytes.
Production dispatch must check the supported executable identity separately.
The source corpus is derived from the user's game and stays unmodified.
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


class TextureInstrumentationError(ValueError):
    """The input shape or requested output is unsupported or unsafe."""


ENTRY = 0x0889E5C0
EARLY_RETURN = 0x0889E650
POSITIVE_RETURN = 0x0889E7C8
END = 0x0889E7D0

_LABEL = re.compile(r"(?m)^L_([0-9A-F]{8}):(?=\r?$)")
_ENTRY_SIGNATURE = re.compile(
    r"\bvoid\s+recomp_unit_0038_entry\s*\(\s*Runtime\s*&\s*([A-Za-z_]\w*)\s*,\s*"
    r"AllegrexContext\s*&\s*([A-Za-z_]\w*)\s*,"
)
_RUNTIME_INCLUDE = re.compile(r'(?m)^#include "psprecomp/runtime\.hpp"(?P<newline>\r?\n)')
_INCLUDE = '#include "native/texture_command_dispatch.hpp"'
_ENTRY_CALLBACK = "texture_command_entry"
_RETURN_CALLBACK = "texture_command_return"
_MANIFEST_SCHEMA = "mhp3rd-texture-command-instrumentation-v1"
_RETURN_LINES = (
    "jump_target = ctx.gpr[31];",
    "// nop",
    "local_pc = jump_target;",
    "if (++local_transfers < 2048u) { entry_id = 0u; goto LOCAL_DISPATCH; }",
    "ctx.pc = jump_target;",
    "return;",
)


def _single(matches: list[re.Match[str]], description: str) -> re.Match[str]:
    if len(matches) != 1:
        raise TextureInstrumentationError(
            f"expected exactly one {description}; found {len(matches)}"
        )
    return matches[0]


def _line_after(source: str, match: re.Match[str]) -> int:
    end = source.find("\n", match.end())
    if end < 0:
        raise TextureInstrumentationError("instruction label has no following line")
    return end + 1


def _return_anchor(source: str, label: re.Match[str], following: re.Match[str],
                   address: int) -> int:
    start = _line_after(source, label)
    block = source[start:following.start()]
    lines = [line.strip() for line in block.splitlines() if line.strip()]
    if tuple(lines) != _RETURN_LINES:
        raise TextureInstrumentationError(f"return structure changed at 0x{address:08X}")
    transfer = re.search(r"(?m)^[ \t]*local_pc = jump_target;\r?\n", block)
    if transfer is None:
        raise TextureInstrumentationError(f"return transfer missing at 0x{address:08X}")
    return start + transfer.start()


def instrument_source(source: str) -> tuple[str, dict]:
    """Return an instrumented copy and a shape manifest, without file writes."""
    if any(marker in source for marker in (_INCLUDE, _ENTRY_CALLBACK, _RETURN_CALLBACK)):
        raise TextureInstrumentationError("source is already instrumented")

    signature = _single(list(_ENTRY_SIGNATURE.finditer(source)), "unit 0038 entry signature")
    runtime_name, context_name = signature.groups()
    if context_name != "ctx":
        raise TextureInstrumentationError("generated context name changed")
    include = _single(list(_RUNTIME_INCLUDE.finditer(source)), "runtime include")
    newline = include.group("newline")

    labels: dict[int, list[re.Match[str]]] = {}
    for match in _LABEL.finditer(source):
        labels.setdefault(int(match.group(1), 16), []).append(match)
    for address in range(ENTRY, END + 4, 4):
        _single(labels.get(address, []), f"label L_{address:08X}")
    first = labels[ENTRY][0]
    last = labels[END][0]
    span = source[first.start():last.start()]
    if span.count("jump_target = ctx.gpr[31];") != 2:
        raise TextureInstrumentationError("builder return count changed")
    _single(list(re.finditer(
        r"runtime\.register_function\(0x0889E5C0u,", source
    )), "builder registration")

    entry_block = source[_line_after(source, first):labels[ENTRY + 4][0].start()]
    if tuple(line.strip() for line in entry_block.splitlines() if line.strip()) != (
        "ctx.gpr[29] = (ctx.gpr[29] + static_cast<std::uint32_t>(-80));",
        "goto L_0889E5C4;",
    ):
        raise TextureInstrumentationError("builder entry structure changed")

    # Both original returns get a completion callback. The no-command path
    # still has no source-authority permit and runs original AOT.
    early_return_at = _return_anchor(source, labels[EARLY_RETURN][0],
                                     labels[EARLY_RETURN + 4][0], EARLY_RETURN)
    return_at = _return_anchor(source, labels[POSITIVE_RETURN][0],
                               labels[POSITIVE_RETURN + 4][0], POSITIVE_RETURN)
    entry_at = _line_after(source, first)
    entry_code = (
        f"    if (mhp3rd::native::{_ENTRY_CALLBACK}({runtime_name}, ctx)) {{{newline}"
        f"        if ({runtime_name}.stopped()) return;{newline}"
        f"        local_pc = ctx.pc;{newline}"
        f"        if (++local_transfers < 2048u) {{ entry_id = 0u; goto LOCAL_DISPATCH; }}{newline}"
        f"        return;{newline}"
        f"    }}{newline}"
    )
    return_code = (
        f"    mhp3rd::native::{_RETURN_CALLBACK}({runtime_name}, ctx, jump_target);{newline}"
    )
    output = source
    for at, addition in sorted((
        (include.end(), _INCLUDE + newline),
        (entry_at, entry_code),
        (early_return_at, return_code),
        (return_at, return_code),
    ), reverse=True):
        output = output[:at] + addition + output[at:]
    return output, {
        "schema": _MANIFEST_SCHEMA,
        "unit": "generated_unit_0038",
        "entry": f"0x{ENTRY:08X}",
        "positive_return": f"0x{POSITIVE_RETURN:08X}",
        "no_command_return": f"0x{EARLY_RETURN:08X}",
        "no_command_authority": "unsupported",
        "entry_calls": 1,
        "return_calls": 2,
    }


def _check_source(path: Path) -> None:
    if path.is_symlink():
        raise TextureInstrumentationError(f"source is a symlink: {path}")
    try:
        mode = path.stat().st_mode
    except OSError as exc:
        raise TextureInstrumentationError(f"cannot read source: {exc}") from exc
    if not stat.S_ISREG(mode):
        raise TextureInstrumentationError(f"source is not a regular file: {path}")


def _check_destination(path: Path, *, manifest: bool = False) -> None:
    if path.is_symlink():
        raise TextureInstrumentationError(f"destination is a symlink: {path}")
    if not path.exists():
        return
    if not stat.S_ISREG(path.stat().st_mode):
        raise TextureInstrumentationError(f"destination is not a regular file: {path}")
    contents = path.read_bytes()
    if manifest:
        try:
            owned = json.loads(contents).get("schema") == _MANIFEST_SCHEMA
        except (UnicodeDecodeError, ValueError, AttributeError):
            owned = False
    else:
        owned = all(marker in contents for marker in (
            _INCLUDE.encode("ascii"), _ENTRY_CALLBACK.encode("ascii"),
            _RETURN_CALLBACK.encode("ascii"),
        ))
    if not owned:
        raise TextureInstrumentationError(f"refusing to overwrite an unrelated file: {path}")


def _same_file(left: Path, right: Path) -> bool:
    if left.resolve() == right.resolve():
        return True
    return left.exists() and right.exists() and os.path.samefile(left, right)


def _atomic_write(path: Path, content: bytes) -> None:
    if path.exists() and path.read_bytes() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp",
                                             dir=path.parent)
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
        raise TextureInstrumentationError("source and output must be different files")
    if manifest_path is not None and (
        _same_file(input_path, manifest_path) or _same_file(output_path, manifest_path)
    ):
        raise TextureInstrumentationError("manifest must be separate from source and output")
    _check_destination(output_path)
    if manifest_path is not None:
        _check_destination(manifest_path, manifest=True)
    source_bytes = input_path.read_bytes()
    try:
        source = source_bytes.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise TextureInstrumentationError("generated source is not UTF-8") from exc
    output, manifest = instrument_source(source)
    output_bytes = output.encode("utf-8")
    manifest["source_sha256"] = hashlib.sha256(source_bytes).hexdigest()
    manifest["output_sha256"] = hashlib.sha256(output_bytes).hexdigest()
    _atomic_write(output_path, output_bytes)
    if manifest_path is not None:
        _atomic_write(manifest_path, (json.dumps(manifest, sort_keys=True, indent=2) + "\n").encode("utf-8"))
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="ignored generated AOT source")
    parser.add_argument("--output", type=Path, required=True, help="build-local instrumented copy")
    parser.add_argument("--manifest", type=Path, help="optional build-local JSON manifest")
    args = parser.parse_args(argv)
    try:
        manifest = instrument_file(args.input, args.output, args.manifest)
    except (OSError, TextureInstrumentationError) as exc:
        parser.error(str(exc))
    print(f"instrumented {manifest['entry_calls']} entry and {manifest['return_calls']} return")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
