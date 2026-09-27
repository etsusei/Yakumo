#!/usr/bin/env python3
"""Run bounded instrumented-AOT fixtures and bind their local evidence."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ELF_HASH = "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c"


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def validate_result(result: dict) -> None:
    expected = {
        "schema_version": 1, "scope": "texture-command-instrumented-aot",
        "success": True, "cases": 32, "full_ram_vram_cpu_compared": True,
        "caller_chain_cases": 18,
        "production_authority": False, "elf_sha256": ELF_HASH,
    }
    if set(result) != set(expected) | {"max_interpreter_slices"}:
        raise ValueError("Unexpected or missing original-path evidence fields")
    for key, value in expected.items():
        if type(result[key]) is not type(value) or result[key] != value:
            raise ValueError(f"Incomplete or unsupported original-path evidence: {key}")
    slices = result["max_interpreter_slices"]
    if type(slices) is not int or not 0 < slices < 100000:
        raise ValueError("Original instruction budget was not satisfied")


def check(elf: Path, oracle: Path, manifest: Path, output: Path) -> dict:
    if output.exists() or output.is_symlink():
        raise ValueError("Output exists; use a fresh evidence path")
    elf, oracle, manifest = (path.resolve(strict=True) for path in (elf, oracle, manifest))
    profile = Path(__file__).resolve().parents[1]
    generated = manifest.with_suffix("")
    sources = {
        "elf": elf, "oracle_binary": oracle, "instrumentation_manifest": manifest,
        "instrumented_unit": generated,
        "original_unit": profile / "generated/generated_unit_0038.cpp",
        "runner": Path(__file__).resolve(),
    }
    for relative in (
        "host/native/texture_command_dispatch.hpp", "host/native/texture_command_dispatch.cpp",
        "host/native/texture_commands_bridge.hpp", "host/native/texture_commands_bridge.cpp",
        "host/resources/texture_commands.hpp", "host/resources/texture_commands.cpp",
        "tests/texture_command_dispatch_oracle.cpp", "tools/instrument_texture_commands.py",
        "cmake/TextureCommandInstrumentation.cmake", "CMakeLists.txt",
    ):
        sources[relative] = profile / relative
    identities = {name: digest(path) for name, path in sources.items()}
    shape = json.loads(manifest.read_text())
    if (identities["elf"] != ELF_HASH or
            shape.get("schema") != "mhp3rd-texture-command-instrumentation-v1" or
            shape.get("entry_calls") != 1 or shape.get("return_calls") != 2 or
            shape.get("source_sha256") != identities["original_unit"] or
            shape.get("output_sha256") != identities["instrumented_unit"]):
        raise ValueError("ELF or build-local instrumentation identity differs")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="texture-dispatch-", dir=output.parent) as temporary:
        result_path = Path(temporary) / "result.json"
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith(("MHP3RD_", "PSPRECOMP_"))}
        completed = subprocess.run([str(oracle), str(elf), str(result_path.resolve())],
                                   env=environment, capture_output=True, text=True,
                                   timeout=60, check=False)
        if completed.returncode != 0:
            raise ValueError(f"Original-path oracle failed: {completed.stderr[-2000:]}")
        result = json.loads(result_path.read_text())
        validate_result(result)
        if identities != {name: digest(path) for name, path in sources.items()}:
            raise ValueError("Source, input or binary changed during the gate")
        report = {"schema_version": 1, "scope": "texture-command-dispatch-offline-gate",
                  "success": True, "result": result, "sha256": identities,
                  "seconds_per_process": 60,
                  "limitations": [
                      "Explicit synthetic bounds; no production authority/controller or lifecycle producer.",
                      "Selected caller tail executes its real provider/accessor/allocator; owner/source inputs are constructed.",
                      "Original nop fallthrough and controlled local return do not establish a live route.",
                      "No-command Verify is a bridge fixture, not positive source-authority acceptance.",
                      "No game, real scheduler, paired delivery or other-platform coverage."]}
        staged = Path(temporary) / "report.json"
        staged.write_text(json.dumps(report, indent=2) + "\n")
        os.link(staged, output)  # Exclusive publication; do not overwrite prior evidence.
    print("Texture command dispatch: 32 entry/return and 18 original caller-chain fixtures passed")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("elf", "oracle", "manifest", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    check(**vars(parser.parse_args()))


if __name__ == "__main__":
    main()
