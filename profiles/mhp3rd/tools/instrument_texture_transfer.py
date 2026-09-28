#!/usr/bin/env python3
"""Add build-local load/enqueue/descriptor observation callbacks to two AOT units.

This edits only a build-local copy. The supported ELF/overlay identity is
validated independently by the bounded original-code oracle.
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


class TextureTransferInstrumentationError(ValueError):
    """The generated unit differs from the supported source shape."""


SCHEMA = "mhp3rd-texture-transfer-instrumentation-v1"
_INCLUDE = '#include "native/texture_command_dispatch.hpp"'
_CALLBACK = "mhp3rd::native::texture_transfer_checkpoint"
_RUNTIME_INCLUDE = re.compile(r'(?m)^#include "psprecomp/runtime\.hpp"(?P<newline>\r?\n)')
_SIGNATURE = re.compile(
    r"\bvoid\s+recomp_unit_(\d{4})_entry\s*\(\s*Runtime\s*&\s*"
    r"([A-Za-z_]\w*)\s*,\s*AllegrexContext\s*&\s*([A-Za-z_]\w*)\s*,"
)
_LABEL = re.compile(r"(?m)^L_([0-9A-F]{8}):(?=\r?$)")

# SHA-256 of each whitespace-normalized basic block from its label to the next
# label. These pin local generated-code shape without copying instruction bytes.
CHECKPOINTS: dict[int, tuple[tuple[str, int, str], ...]] = {
    23: (
        ("EnqueueEntry", 0x08863CDC, "123446281a9690d69dfbb828ea7b7c5ff53fbd0ab93bf4b232f8d5f0019c60c2"),
        ("DescriptorCommit", 0x08863DD8, "90714b0ebf92c653481359ee295c06482bdd80a4c90335296a3335abe82ef16e"),
        ("EnqueueReturn", 0x08863E28, "22522cec5bff77ff4b23c47a6a9288804dff8f1dec6b933d69dcf41dc4f6251f"),
    ),
    40: (
        ("OwnerLoadEntry", 0x088A5470, "1f2b32ea58ad7e202727d97db4598bd12264664462046578c47474ba094c0667"),
        ("OwnerLoadTail", 0x088A54EC, "b7875f371e2cac50ea499d8a9cd5eab5d3900df466e5b20fcaa7fb631ce41e9f"),
    ),
}

# Deliberately artificial fixture hashes let unit tests exercise the parser
# without copying any generated game source into Git.
TEST_CHECKPOINTS: dict[int, tuple[tuple[str, int, str, tuple[str, ...]], ...]] = {
    23: (("EnqueueEntry", 0x08863CDC, "", ("    ctx.gpr[2] = 1u;\n",)),
         ("DescriptorCommit", 0x08863DD8, "", ("    ctx.gpr[2] = 2u;\n",)),
         ("EnqueueReturn", 0x08863E28, "", ("    ctx.gpr[2] = 3u;\n",))),
    40: (("OwnerLoadEntry", 0x088A5470, "", ("    ctx.gpr[2] = 4u;\n",)),
         ("OwnerLoadTail", 0x088A54EC, "", ("    ctx.gpr[2] = 5u;\n",))),
}


def _single(matches: list[re.Match[str]], description: str) -> re.Match[str]:
    if len(matches) != 1:
        raise TextureTransferInstrumentationError(
            f"expected exactly one {description}; found {len(matches)}")
    return matches[0]


def _next_line(source: str, match: re.Match[str]) -> int:
    end = source.find("\n", match.end())
    if end < 0:
        raise TextureTransferInstrumentationError("instruction label has no following line")
    return end + 1


def instrument_source(source: str,
                     checkpoint_specs: dict[int, tuple[tuple[str, int, str], ...]] | None = None
                     ) -> tuple[str, dict]:
    """Return an instrumented copy and its manifest without writing files."""
    specs = CHECKPOINTS if checkpoint_specs is None else checkpoint_specs
    if _CALLBACK in source:
        raise TextureTransferInstrumentationError("source already has transfer instrumentation")
    signature = _single(list(_SIGNATURE.finditer(source)), "generated unit entry signature")
    unit = int(signature.group(1))
    if unit not in specs:
        raise TextureTransferInstrumentationError(f"unsupported generated unit {unit:04d}")
    if signature.group(2, 3) != ("rt", "ctx"):
        raise TextureTransferInstrumentationError("generated runtime/context names changed")

    runtime_include = _single(list(_RUNTIME_INCLUDE.finditer(source)), "runtime include")
    include_matches = list(re.finditer(r'(?m)^#include "native/texture_command_dispatch\.hpp"\r?$', source))
    if len(include_matches) > 1:
        raise TextureTransferInstrumentationError("duplicate texture command dispatcher include")
    newline = runtime_include.group("newline")
    include_insertion = (runtime_include.end(), _INCLUDE + newline) if not include_matches else None

    registration = re.compile(
        rf"\bruntime\.register_generated_unit\({unit}u,\s*0x[0-9A-F]{{8}}u,\s*"
        rf"[0-9]+u,\s*&recomp_unit_{unit:04d},\s*&recomp_unit_{unit:04d}_entry\);"
    )
    _single(list(registration.finditer(source)), f"unit {unit:04d} registration")
    for _, address, _ in specs[unit]:
        unitsig = re.compile(
            rf"\bruntime\.register_function\(0x{address:08X}u,\s*&recomp_unit_{unit:04d},"
        )
        _single(list(unitsig.finditer(source)), f"checkpoint 0x{address:08X} unit ownership")

    labels = list(_LABEL.finditer(source))
    by_address: dict[int, list[int]] = {}
    for index, match in enumerate(labels):
        by_address.setdefault(int(match.group(1), 16), []).append(index)

    additions: list[tuple[int, str]] = []
    manifest_entries = []
    for name, address, expected_hash in specs[unit]:
        indexes = by_address.get(address, [])
        if len(indexes) != 1:
            raise TextureTransferInstrumentationError(
                f"expected one label L_{address:08X}; found {len(indexes)}")
        index = indexes[0]
        if index == 0 or index + 1 == len(labels):
            raise TextureTransferInstrumentationError(f"label L_{address:08X} lacks neighbours")
        previous = int(labels[index - 1].group(1), 16)
        following = int(labels[index + 1].group(1), 16)
        if previous != address - 4 or following != address + 4:
            raise TextureTransferInstrumentationError(f"instruction labels around 0x{address:08X} changed")
        block = source[_next_line(source, labels[index]):labels[index + 1].start()]
        fingerprint = hashlib.sha256(re.sub(r"\s+", "", block).encode("utf-8")).hexdigest()
        if fingerprint != expected_hash:
            raise TextureTransferInstrumentationError(f"instruction block changed at 0x{address:08X}")
        additions.append((_next_line(source, labels[index]),
                          f"    {_CALLBACK}(rt, ctx, mhp3rd::native::TextureTransferCheckpoint::{name});{newline}"))
        manifest_entries.append({"name": name, "address": f"0x{address:08X}",
                                 "shape_sha256": fingerprint})

    if include_insertion is not None:
        additions.append(include_insertion)
    output = source
    for offset, text in sorted(additions, key=lambda item: item[0], reverse=True):
        output = output[:offset] + text + output[offset:]
    return output, {"schema": SCHEMA, "unit": f"generated_unit_{unit:04d}",
                    "checkpoints": manifest_entries, "checkpoint_calls": len(manifest_entries),
                    "include_added": include_insertion is not None}


def _unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for name, value in pairs:
        if name in result:
            raise TextureTransferInstrumentationError("duplicate key in owned manifest")
        result[name] = value
    return result


def _strip_owned_callbacks(source: str,
                           checkpoints: tuple[tuple[str, int, str], ...],
                           include_added: bool) -> str:
    stripped = source
    for name, address, _ in checkpoints:
        callback = (
            rf"(?m)^(L_{address:08X}:)(\r?\n)[ \t]*"
            rf"{re.escape(_CALLBACK)}\(rt, ctx, "
            rf"mhp3rd::native::TextureTransferCheckpoint::{re.escape(name)}\);\r?\n"
        )
        stripped, removed = re.subn(callback, r"\1\2", stripped)
        if removed != 1:
            raise TextureTransferInstrumentationError(
                f"owned output has an invalid callback at 0x{address:08X}")
    if _CALLBACK in stripped:
        raise TextureTransferInstrumentationError("owned output has an unrecognized transfer callback")
    if include_added:
        stripped, removed = re.subn(
            rf"(?m)^{re.escape(_INCLUDE)}\r?\n", "", stripped)
        if removed != 1:
            raise TextureTransferInstrumentationError("owned output has an invalid dispatcher include")
    return stripped


def _validate_owned_pair(output_path: Path, manifest_path: Path,
                         checkpoints: tuple[tuple[str, int, str], ...]) -> bool:
    try:
        output_bytes = output_path.read_bytes()
        old = json.loads(manifest_path.read_text(encoding="utf-8"),
                         object_pairs_hook=_unique_object)
        expected_calls = len(checkpoints)
        if (type(old) is not dict or old.get("schema") != SCHEMA or
                old.get("unit") not in {"generated_unit_0023", "generated_unit_0040"} or
                type(old.get("checkpoint_calls")) is not int or
                old["checkpoint_calls"] != expected_calls or
                type(old.get("source_sha256")) is not str or
                not re.fullmatch(r"[0-9a-f]{64}", old["source_sha256"]) or
                old.get("output_sha256") != hashlib.sha256(output_bytes).hexdigest()):
            return False
        unit_number = int(old["unit"][-4:])
        if unit_number not in CHECKPOINTS:
            return False
        owned_checkpoints = checkpoints
        if tuple((name, address) for name, address, _ in checkpoints) != tuple(
                (name, address) for name, address, _ in CHECKPOINTS[unit_number]):
            return False
        include_added = old.get("include_added", unit_number == 23)
        if type(include_added) is not bool:
            return False
        old_text = output_bytes.decode("utf-8")
        stripped = _strip_owned_callbacks(old_text, owned_checkpoints, include_added)
        old_input = stripped.encode("utf-8")
        if hashlib.sha256(old_input).hexdigest() != old["source_sha256"]:
            return False
        reproduced, regenerated = instrument_source(stripped, {unit_number: checkpoints})
        if reproduced.encode("utf-8") != output_bytes:
            return False
        return (regenerated["schema"] == old["schema"] and
                regenerated["unit"] == old["unit"] and
                regenerated["checkpoint_calls"] == old["checkpoint_calls"] and
                regenerated["include_added"] == include_added and
                regenerated["checkpoints"] == old.get("checkpoints"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError,
            TextureTransferInstrumentationError, TypeError, ValueError):
        return False


def _write_temp(path: Path, data: bytes) -> str:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
    except OSError:
        if os.path.exists(temporary):
            os.unlink(temporary)
        raise
    return temporary


def _atomic_write_pair(output_path: Path, output: bytes,
                       manifest_path: Path, manifest: bytes) -> None:
    temporary_files: list[str] = []
    backups: dict[Path, str] = {}
    installed: list[Path] = []
    committed = False
    try:
        temporary_output = _write_temp(output_path, output)
        temporary_files.append(temporary_output)
        temporary_manifest = _write_temp(manifest_path, manifest)
        temporary_files.append(temporary_manifest)
        for path in (output_path, manifest_path):
            if not path.exists():
                continue
            fd, backup = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".bak", dir=path.parent)
            os.close(fd)
            os.unlink(backup)
            try:
                os.replace(path, backup)
            except OSError:
                # A wrapper/filesystem can report an error after moving the
                # file. Track that case too so rollback never loses the only
                # remaining copy of the old build-local pair.
                if os.path.exists(backup):
                    backups[path] = backup
                raise
            backups[path] = backup
        os.replace(temporary_output, output_path)
        temporary_files.remove(temporary_output)
        installed.append(output_path)
        os.replace(temporary_manifest, manifest_path)
        temporary_files.remove(temporary_manifest)
        installed.append(manifest_path)
        committed = True
    except OSError as publish_error:
        rollback_errors: list[str] = []
        unresolved: set[Path] = set()
        for path in reversed(installed):
            if path.exists():
                try:
                    os.unlink(path)
                except OSError:
                    # Continue restoring the other half. os.replace below can
                    # still recover this path when an old backup exists.
                    unresolved.add(path)
        for path, backup in reversed(list(backups.items())):
            if os.path.exists(backup):
                try:
                    os.replace(backup, path)
                    unresolved.discard(path)
                except OSError as error:
                    if not os.path.exists(backup) and path.exists():
                        # The filesystem wrapper may report an error after the
                        # rename already consumed the backup. Treat the file as
                        # restored; do not claim recovery is incomplete.
                        unresolved.discard(path)
                    else:
                        rollback_errors.append(f"restore {path} from {backup}: {error}")
                        unresolved.add(path)
        retained = [(path, backup) for path, backup in backups.items()
                    if os.path.exists(backup)]
        if unresolved or retained or rollback_errors:
            recovery = [f"{path} <- {backup}" for path, backup in retained]
            recovery.extend(str(path) for path in sorted(unresolved - {p for p, _ in retained},
                                                          key=str))
            details = "; ".join(rollback_errors + [f"recovery location: {item}"
                                                    for item in recovery])
            raise OSError(f"publishing output/manifest pair failed: {publish_error}; "
                           f"rollback incomplete; preserve and recover: {details}") from publish_error
        raise
    finally:
        for temporary in temporary_files:
            if os.path.exists(temporary):
                try:
                    os.unlink(temporary)
                except OSError:
                    # Temporary files are disposable. Do not let a cleanup
                    # failure mask the publish error or trigger backup loss.
                    pass
        if committed:
            # Old contents become disposable only after both new files are in
            # place. If cleanup fails, keep the backup for manual recovery.
            for backup in backups.values():
                if os.path.exists(backup):
                    try:
                        os.unlink(backup)
                    except OSError:
                        pass


def publish_instrumented_pair(output_path: Path, output: bytes,
                              manifest_path: Path, manifest: bytes,
                              checkpoints: tuple[tuple[str, int, str], ...]) -> None:
    if output_path.is_symlink() or manifest_path.is_symlink():
        raise TextureTransferInstrumentationError("symlinks are not allowed")
    output_exists, manifest_exists = output_path.exists(), manifest_path.exists()
    if output_exists != manifest_exists:
        raise TextureTransferInstrumentationError("refusing to replace an incomplete output/manifest pair")
    if output_exists:
        if (not output_path.is_file() or not manifest_path.is_file() or
                not _validate_owned_pair(output_path, manifest_path, checkpoints)):
            raise TextureTransferInstrumentationError(
                f"refusing to overwrite an unrelated or tampered file: {output_path}")
        if output_path.read_bytes() == output and manifest_path.read_bytes() == manifest:
            return
    _atomic_write_pair(output_path, output, manifest_path, manifest)


def instrument_file(input_path: Path, output_path: Path, manifest_path: Path) -> dict:
    if input_path.is_symlink() or output_path.is_symlink() or manifest_path.is_symlink():
        raise TextureTransferInstrumentationError("symlinks are not allowed")
    if not input_path.is_file():
        raise TextureTransferInstrumentationError("instrumentation input is not a regular file")
    if input_path.resolve() in {output_path.resolve(), manifest_path.resolve()} or output_path.resolve() == manifest_path.resolve():
        raise TextureTransferInstrumentationError("input, output and manifest must be distinct files")
    original = input_path.read_bytes()
    try:
        transformed, metadata = instrument_source(original.decode("utf-8"))
    except UnicodeDecodeError as exc:
        raise TextureTransferInstrumentationError("generated source is not UTF-8") from exc
    output = transformed.encode("utf-8")
    metadata["source_sha256"] = hashlib.sha256(original).hexdigest()
    metadata["output_sha256"] = hashlib.sha256(output).hexdigest()
    manifest = (json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode("utf-8")
    unit = int(metadata["unit"][-4:])
    publish_instrumented_pair(output_path, output, manifest_path, manifest, CHECKPOINTS[unit])
    return metadata


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        result = instrument_file(args.input, args.output, args.manifest)
    except (OSError, TextureTransferInstrumentationError) as exc:
        parser.error(str(exc))
    print(f"instrumented {result['checkpoint_calls']} transfer checkpoints in {result['unit']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
