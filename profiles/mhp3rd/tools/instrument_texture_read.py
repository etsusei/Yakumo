#!/usr/bin/env python3
"""Add build-local state-8 read-attempt observations to units 0023 and 0024.

The output is used only by the bounded read-prefix oracle. In particular, the
unit 0024 copy has a test-only exit after ReadResult so the oracle cannot run
the original copy, transform, or retirement branches.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

import instrument_texture_transfer as transfer_output


class TextureReadInstrumentationError(ValueError):
    """The generated unit differs from the audited read boundary shape."""


SCHEMA = "mhp3rd-texture-read-instrumentation-v1"
DISPATCHER_INCLUDE = '#include "native/texture_command_dispatch.hpp"'
STOP_INCLUDE = '#include "texture_read_oracle_stop.hpp"'
READ_CALLBACK = "mhp3rd::native::texture_read_checkpoint"
STOP_CALLBACK = "mhp3rd::native::texture_read_oracle_stop_after_result"

# Whitespace-normalized hashes of the blocks in their actual build-local input
# stage: unit 0023 is after G1a transfer instrumentation; unit 0024 is original.
CHECKPOINTS: dict[int, tuple[tuple[str, int, str], ...]] = {
    23: (
        ("ReadHelperEntry", 0x08863608,
         "c335dac81c27e6c50baf1484f673f8b5c78a590f824539d8eef6c586576bd132"),
        ("ReadInvoke", 0x0886365C,
         "c24bc96a2ce1362824783fa320ab9ab7e06dbf6b0148f98ec33a901d6356eb2b"),
    ),
    24: (
        ("WorkerState8Entry", 0x088654D4,
         "4a2eace359d5ed402c70026fa4a560594a27abd3e2f932d84526dbb953ec6f92"),
        ("ReadResult", 0x0886551C,
         "6e6734110bff9aecf4b630659b061d7d56839ba34b26747108626280dab1f4ec"),
    ),
}

TEST_CHECKPOINTS: dict[int, tuple[tuple[str, int, str], ...]] = {
    23: (("ReadHelperEntry", 0x08863608, "    ctx.gpr[29] = ctx.gpr[29] - 16u;\n"),
         ("ReadInvoke", 0x0886365C, "    ctx.pc = 0x089656A0u;\n")),
    24: (("WorkerState8Entry", 0x088654D4, "    ctx.gpr[2] = 8u;\n"),
         ("ReadResult", 0x0886551C, "    ctx.gpr[16] = ctx.gpr[2];\n")),
}
_RUNTIME_INCLUDE = re.compile(r'(?m)^#include "psprecomp/runtime\.hpp"(?P<newline>\r?\n)')
_SIGNATURE = re.compile(
    r"\bvoid\s+recomp_unit_(\d{4})_entry\s*\(\s*Runtime\s*&\s*"
    r"([A-Za-z_]\w*)\s*,\s*AllegrexContext\s*&\s*([A-Za-z_]\w*)\s*,"
)
_LABEL = re.compile(r"(?m)^L_([0-9A-F]{8}):(?=\r?$)")


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate manifest key: {key}")
        result[key] = value
    return result


def _single(matches: list[re.Match[str]], description: str) -> re.Match[str]:
    if len(matches) != 1:
        raise TextureReadInstrumentationError(
            f"expected exactly one {description}; found {len(matches)}")
    return matches[0]


def _next_line(source: str, match: re.Match[str]) -> int:
    end = source.find("\n", match.end())
    if end < 0:
        raise TextureReadInstrumentationError("instruction label has no following line")
    return end + 1


def _snippet(unit: int, name: str, address: int) -> str:
    result = (f"    {READ_CALLBACK}(rt, ctx, "
              f"mhp3rd::native::TextureReadCheckpoint::{name});\n")
    if unit == 24 and name == "ReadResult":
        result += (
            f"    if ({STOP_CALLBACK}(rt, ctx)) {{\n"
            "        ctx.pc = 0x08001000u;\n"
            "        return;\n"
            "    }\n"
        )
    return result


def instrument_source(source: str,
                      checkpoint_specs: dict[int, tuple[tuple[str, int, str], ...]] | None = None
                      ) -> tuple[str, dict]:
    specs = CHECKPOINTS if checkpoint_specs is None else checkpoint_specs
    if READ_CALLBACK in source or STOP_CALLBACK in source:
        raise TextureReadInstrumentationError("source already has read-prefix instrumentation")
    signature = _single(list(_SIGNATURE.finditer(source)), "generated unit entry signature")
    unit = int(signature.group(1))
    if unit not in specs:
        raise TextureReadInstrumentationError(f"unsupported generated unit {unit:04d}")
    if signature.group(2, 3) != ("rt", "ctx"):
        raise TextureReadInstrumentationError("generated runtime/context names changed")
    if unit == 24 and not any(name == "ReadResult" for name, _, _ in specs[unit]):
        raise TextureReadInstrumentationError("unit 0024 read-prefix stop requires ReadResult")

    runtime_include = _single(list(_RUNTIME_INCLUDE.finditer(source)), "runtime include")
    dispatcher_matches = list(re.finditer(
        r'(?m)^#include "native/texture_command_dispatch\.hpp"\r?$', source))
    stop_matches = list(re.finditer(
        r'(?m)^#include "texture_read_oracle_stop\.hpp"\r?$', source))
    if len(dispatcher_matches) > 1 or len(stop_matches) > 1:
        raise TextureReadInstrumentationError("duplicate read-prefix include")
    dispatcher_include_added = not dispatcher_matches
    stop_include_added = unit == 24 and not stop_matches
    include_text = ""
    newline = runtime_include.group("newline")
    if dispatcher_include_added:
        include_text += DISPATCHER_INCLUDE + newline
    if stop_include_added:
        include_text += STOP_INCLUDE + newline

    registration = re.compile(
        rf"\bruntime\.register_generated_unit\({unit}u,\s*0x[0-9A-F]{{8}}u,\s*"
        rf"[0-9]+u,\s*&recomp_unit_{unit:04d},\s*&recomp_unit_{unit:04d}_entry\);"
    )
    _single(list(registration.finditer(source)), f"unit {unit:04d} registration")
    labels = list(_LABEL.finditer(source))
    by_address: dict[int, list[int]] = {}
    for index, label in enumerate(labels):
        by_address.setdefault(int(label.group(1), 16), []).append(index)

    additions: list[tuple[int, str]] = []
    normalized_checkpoints: list[dict] = []
    for name, address, expected_hash in specs[unit]:
        unit_registration = re.compile(
            rf"\bruntime\.register_function\(0x{address:08X}u,\s*&recomp_unit_{unit:04d},"
        )
        _single(list(unit_registration.finditer(source)), f"checkpoint 0x{address:08X} registration")
        indexes = by_address.get(address, [])
        if len(indexes) != 1:
            raise TextureReadInstrumentationError(
                f"expected one label L_{address:08X}; found {len(indexes)}")
        label_index = indexes[0]
        body_start = _next_line(source, labels[label_index])
        body_end = labels[label_index + 1].start() if label_index + 1 < len(labels) else len(source)
        normalized = re.sub(r"\s+", "", source[body_start:body_end]).encode("utf-8")
        actual_hash = _sha(normalized)
        if actual_hash != expected_hash:
            raise TextureReadInstrumentationError(
                f"checkpoint block changed at 0x{address:08X}: {actual_hash}")
        additions.append((body_start, _snippet(unit, name, address)))
        normalized_checkpoints.append({"name": name, "address": f"0x{address:08X}",
                                       "sha256": actual_hash})
    if unit != 24 and STOP_CALLBACK in source:
        raise TextureReadInstrumentationError("read-prefix stop is only valid in unit 0024")

    result = source
    for position, insertion in sorted(additions, reverse=True):
        result = result[:position] + insertion + result[position:]
    if include_text:
        result = result[:runtime_include.end()] + include_text + result[runtime_include.end():]
    metadata = {
        "schema": SCHEMA,
        "unit": f"generated_unit_{unit:04d}",
        "checkpoint_calls": len(normalized_checkpoints),
        "dispatcher_include_added": dispatcher_include_added,
        "stop_include_added": stop_include_added,
        "oracle_stop_after_read_result": unit == 24,
        "checkpoints": normalized_checkpoints,
    }
    return result, metadata


def _strip_owned_callbacks(source: str, metadata: dict,
                           checkpoint_specs: dict[int, tuple[tuple[str, int, str], ...]] | None = None) -> str:
    unit_text = metadata.get("unit")
    if type(unit_text) is not str or not unit_text.startswith("generated_unit_"):
        raise TextureReadInstrumentationError("read manifest unit missing")
    unit = int(unit_text[-4:])
    specs = CHECKPOINTS if checkpoint_specs is None else checkpoint_specs
    if unit not in specs:
        raise TextureReadInstrumentationError("read manifest unit unsupported")
    for name, address, _ in specs[unit]:
        label = f"L_{address:08X}:\n"
        snippet = _snippet(unit, name, address)
        marker = label + snippet
        if source.count(marker) != 1:
            raise TextureReadInstrumentationError(f"owned callback missing at 0x{address:08X}")
        source = source.replace(marker, label, 1)
    include_lines = []
    if metadata.get("dispatcher_include_added") is True:
        include_lines.append(DISPATCHER_INCLUDE)
    if metadata.get("stop_include_added") is True:
        include_lines.append(STOP_INCLUDE)
    for include in include_lines:
        if source.count(include) != 1:
            raise TextureReadInstrumentationError(f"owned include missing: {include}")
        source = source.replace(include + "\n", "", 1)
    return source


def _valid_owned_pair(output_path: Path, manifest_path: Path,
                      checkpoint_specs: dict[int, tuple[tuple[str, int, str], ...]] | None = None) -> bool:
    try:
        output = output_path.read_bytes()
        metadata = json.loads(manifest_path.read_text(encoding="utf-8"),
                              object_pairs_hook=_unique_object)
        if (metadata.get("schema") != SCHEMA or
                metadata.get("unit") not in {"generated_unit_0023", "generated_unit_0024"} or
                metadata.get("output_sha256") != _sha(output)):
            return False
        source = _strip_owned_callbacks(output.decode("utf-8"), metadata, checkpoint_specs)
        if metadata.get("source_sha256") != _sha(source.encode("utf-8")):
            return False
        reproduced, regenerated = instrument_source(source, checkpoint_specs)
        if reproduced.encode("utf-8") != output:
            return False
        return all(metadata.get(key) == value for key, value in regenerated.items())
    except (OSError, UnicodeDecodeError, json.JSONDecodeError,
            TextureReadInstrumentationError, transfer_output.TextureTransferInstrumentationError,
            TypeError, ValueError):
        return False


def publish_pair(output_path: Path, output: bytes, manifest_path: Path,
                 manifest: bytes,
                 checkpoint_specs: dict[int, tuple[tuple[str, int, str], ...]] | None = None) -> None:
    if output_path.is_symlink() or manifest_path.is_symlink():
        raise TextureReadInstrumentationError("symlinks are not allowed")
    exists = output_path.exists(), manifest_path.exists()
    if exists[0] != exists[1]:
        raise TextureReadInstrumentationError("refusing to replace incomplete read output/manifest pair")
    if all(exists):
        if (not output_path.is_file() or not manifest_path.is_file() or
                not _valid_owned_pair(output_path, manifest_path, checkpoint_specs)):
            raise TextureReadInstrumentationError(
                f"refusing to overwrite unknown or tampered read output: {output_path}")
        if output_path.read_bytes() == output and manifest_path.read_bytes() == manifest:
            return
    transfer_output._atomic_write_pair(output_path, output, manifest_path, manifest)


def instrument_file(input_path: Path, output_path: Path, manifest_path: Path) -> dict:
    if any(path.is_symlink() for path in (input_path, output_path, manifest_path)):
        raise TextureReadInstrumentationError("symlinks are not allowed")
    if not input_path.is_file():
        raise TextureReadInstrumentationError("read instrumentation input is not a regular file")
    resolved = {input_path.resolve(), output_path.resolve(), manifest_path.resolve()}
    if len(resolved) != 3:
        raise TextureReadInstrumentationError("input, output and manifest must be distinct")
    original = input_path.read_bytes()
    try:
        transformed, metadata = instrument_source(original.decode("utf-8"))
    except UnicodeDecodeError as exc:
        raise TextureReadInstrumentationError("generated source is not UTF-8") from exc
    output = transformed.encode("utf-8")
    metadata["source_sha256"] = _sha(original)
    metadata["output_sha256"] = _sha(output)
    manifest = (json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode("utf-8")
    publish_pair(output_path, output, manifest_path, manifest, CHECKPOINTS)
    return metadata


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        result = instrument_file(args.input, args.output, args.manifest)
    except (OSError, TextureReadInstrumentationError) as exc:
        parser.error(str(exc))
    print(f"instrumented {result['checkpoint_calls']} read checkpoints in {result['unit']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
