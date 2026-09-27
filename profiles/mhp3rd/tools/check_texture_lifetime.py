#!/usr/bin/env python3
"""Bind bounded original lifecycle observations to their local build inputs."""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import tempfile
from check_texture_allocation import digest, unique_object

EXPECTED = dict(calls_per_path=34, factory_chains=8, caller_chains=7,
                owner_reuses=2, command_releases=2, owner_resets=2, heap_resets=2,
                loss_cases=3, owner_leases=7, consumed_tickets=4, no_command_cases=1)


def validate(report):
    fixed = dict(schema_version=1, scope="original-texture-lifetime-checkpoints", success=True,
                 full_ram_vram_cpu_compared=True, transfer_readiness=False, **EXPECTED)
    if set(report) != set(fixed) | {"max_interpreter_slices"}:
        raise ValueError("Unexpected or missing lifetime evidence fields")
    for key, value in fixed.items():
        if type(report[key]) is not type(value) or report[key] != value:
            raise ValueError("Incomplete or broadened lifetime evidence: " + key)
    if type(report["max_interpreter_slices"]) is not int or not 0 < report["max_interpreter_slices"] < 2000000:
        raise ValueError("Lifetime interpreter bound not met")


def check(elf, overlay, module, oracle, build_dir, output):
    if output.exists() or output.is_symlink():
        raise ValueError("Refusing to overwrite existing evidence")
    profile = Path(__file__).resolve().parents[1]
    paths = dict(elf=elf.resolve(), lobby_image=overlay.resolve(), lobby_module=module.resolve(),
                 oracle_binary=oracle.resolve(), runner=Path(__file__).resolve(),
                 hash_helpers=Path(__file__).with_name("check_texture_allocation.py"))
    for relative in ("CMakeLists.txt", "cmake/TextureLifetimeInstrumentation.cmake",
                     "tools/instrument_texture_lifetime.py", "tests/texture_lifetime_oracle.cpp",
                     "host/native/texture_lifetime_tracker.hpp", "host/native/texture_lifetime_tracker.cpp",
                     "host/native/texture_command_dispatch.hpp", "host/native/texture_command_dispatch.cpp",
                     "host/resources/source_authority.hpp", "host/resources/source_authority.cpp"):
        paths[relative] = profile / relative
    for unit, count in (("0029", 5), ("0040", 1), ("0043", 5), ("0046", 3)):
        filename = f"generated_unit_{unit}.cpp"
        generated = build_dir / "profiles/mhp3rd/texture_lifetime_generated" / filename
        manifest = generated.with_suffix(".cpp.json")
        metadata = json.loads(manifest.read_text(), object_pairs_hook=unique_object)
        original = profile / "generated" / filename
        input_copy = build_dir / "profiles/mhp3rd/probe_generated" / filename
        if not input_copy.exists() or digest(input_copy) != metadata.get("source_sha256"):
            input_copy = original
        if (metadata.get("schema") != "mhp3rd-texture-lifetime-instrumentation-v1" or
                metadata.get("unit") != f"generated_unit_{unit}" or
                type(metadata.get("checkpoint_calls")) is not int or metadata["checkpoint_calls"] != count or
                metadata.get("source_sha256") != digest(input_copy) or
                metadata.get("output_sha256") != digest(generated)):
            raise ValueError("Build-local lifetime identity differs: " + unit)
        paths.update({unit + "_original": original, unit + "_input": input_copy,
                      unit + "_output": generated, unit + "_manifest": manifest})
    before = {name: digest(path) for name, path in paths.items()}
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="lifetime-gate-", dir=output.parent) as temporary:
        stage = Path(temporary); raw = stage / "original.json"
        env = {key: value for key, value in os.environ.items() if not key.startswith(("MHP3RD_", "PSPRECOMP_"))}
        result = subprocess.run([str(paths[key]) for key in ("oracle_binary", "elf", "lobby_image", "lobby_module")]
                                + [str(raw.resolve())], env=env, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise ValueError("Original lifetime oracle failed: " + result.stderr[-3000:])
        if raw.stat().st_size > 65536:
            raise ValueError("Lifetime metadata exceeds bound")
        report = json.loads(raw.read_text(), object_pairs_hook=unique_object); validate(report)
        if before != {name: digest(path) for name, path in paths.items()}:
            raise ValueError("Lifetime source, input or binary changed during gate")
        final = dict(schema_version=1, recorded_at=datetime.now(timezone.utc).isoformat(),
                     scope="original-lifetime-observation-and-tickets", success=True,
                     original_report=report, original_report_sha256=digest(raw), sha256=before,
                     limits=dict(seconds_per_process=60, interpreter_slices_per_call=2000000),
                     limitations=["Constructed outer factory/caller frames; actual factory tail, DSO constructor, provider, accessor, allocator and free/reset run.",
                                  "Source bytes are explicit fixture input; no transfer receipts, resource readiness or native activation.",
                                  "Thread switch-away/back is a host fault injection, not a real guest scheduler run.",
                                  "Code guard checks current executable sections and overlay code; production epoch/writer serialization remains pending.",
                                  "No application installs the tracker. No gameplay or new paired delivery."])
        staged = stage / "report.json"; staged.write_text(json.dumps(final, indent=2) + "\n")
        os.link(staged, output)
    print("Original lifecycle gate: 34 calls per path, 8 factory chains, 4 one-use tickets; passed")
    return final


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("elf", "overlay", "module", "oracle", "build-dir", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    check(**vars(parser.parse_args()))


if __name__ == "__main__":
    main()
