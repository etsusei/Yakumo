#!/usr/bin/env python3
"""Run the bounded compiled G1a gate and bind its private inputs and outputs."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import tempfile

from check_texture_allocation import digest, unique_object


EXPECTED = {
    "aot_interpreter_calls": 117,
    "normal_loads": 4,
    "duplicate_resource_id_loads": 3,
    "descriptor_generations": 5,
    "queued_returns": 4,
    "controlled_ring_slot_reuses": 3,
    "unowned_descriptor_reuses": 1,
    "unowned_enqueues": 1,
    "descriptor_ring_reuses": 4,
    "raw_pointer_reuses": 3,
    "cached_uncached_alias_reuses": 1,
    "pending_writers_retained": 4,
    "fault_cases": 14,
    "capacity_loss_cases": 2,
    "range_fault_cases": 4,
    "zero_length_fault_cases": 1,
    "slot_boundary_cases": 2,
    "over_slot_rejections": 1,
    "rounded_transform_footprint_cases": 1,
    "stale_ctx_pc_cases": 1,
}


def validate(report: dict) -> None:
    fixed = {
        "schema_version": 1,
        "scope": "original-g1a-load-enqueue-descriptor-generation",
        "success": True,
        "full_ram_vram_cpu_compared": True,
        "healthy_authority_exact_notready": True,
        "transfer_readiness": False,
        "event_notification_modeled": True,
        "game_executed": False,
    }
    allowed = set(fixed) | set(EXPECTED) | {"max_interpreter_slices"}
    if type(report) is not dict or set(report) != allowed:
        raise ValueError("Missing or unrecognized G1a evidence fields")
    for key, value in fixed.items():
        if type(report.get(key)) is not type(value) or report[key] != value:
            raise ValueError("G1a report exceeds its evidence scope: " + key)
    for key, value in EXPECTED.items():
        if type(report.get(key)) is not int or report[key] != value:
            raise ValueError("G1a coverage differs: " + key)
    steps = report.get("max_interpreter_slices")
    if type(steps) is not int or not 0 < steps < 2_000_000:
        raise ValueError("G1a interpreter budget is missing or exceeded")


def _manifest(path: Path, schema: str, unit: str, calls: int,
              source: Path, output: Path) -> dict:
    manifest = json.loads(path.read_text(), object_pairs_hook=unique_object)
    if (manifest.get("schema") != schema or manifest.get("unit") != unit or
            type(manifest.get("checkpoint_calls")) is not int or
            manifest["checkpoint_calls"] != calls or
            manifest.get("source_sha256") != digest(source) or
            manifest.get("output_sha256") != digest(output)):
        raise ValueError("Build-local instrumentation identity differs: " + unit)
    return manifest


def _instrumented_paths(profile: Path, build_dir: Path) -> dict[str, Path]:
    paths: dict[str, Path] = {}
    prior_dirs = ("texture_lifetime_generated", "texture_command_generated",
                  "probe_generated", "generated")
    for number, count in (("0029", 5), ("0040", 1), ("0043", 5), ("0046", 3)):
        name = f"generated_unit_{number}.cpp"
        output = build_dir / "profiles/mhp3rd/texture_lifetime_generated" / name
        manifest = output.with_suffix(".cpp.json")
        lifetime = json.loads(manifest.read_text(), object_pairs_hook=unique_object)
        candidates = [build_dir / "profiles/mhp3rd" / directory / name
                      for directory in prior_dirs]
        candidates.append(profile / "generated" / name)
        source = next((candidate for candidate in candidates
                       if candidate.is_file() and
                       digest(candidate) == lifetime.get("source_sha256")), None)
        if source is None:
            raise ValueError("Could not resolve lifetime instrumentation input: " + number)
        _manifest(manifest, "mhp3rd-texture-lifetime-instrumentation-v1",
                  f"generated_unit_{number}", count, source, output)
        paths.update({f"lifetime_{number}_input": source,
                      f"lifetime_{number}_output": output,
                      f"lifetime_{number}_manifest": manifest})

    for number, count in (("0023", 3), ("0040", 2)):
        name = f"generated_unit_{number}.cpp"
        output = build_dir / "profiles/mhp3rd/texture_transfer_generated" / name
        manifest = output.with_suffix(".cpp.json")
        transfer = json.loads(manifest.read_text(), object_pairs_hook=unique_object)
        candidates = [build_dir / "profiles/mhp3rd" / directory / name
                      for directory in prior_dirs]
        candidates.append(profile / "generated" / name)
        source = next((candidate for candidate in candidates
                       if candidate.is_file() and
                       digest(candidate) == transfer.get("source_sha256")), None)
        if source is None:
            raise ValueError("Could not resolve transfer instrumentation input: " + number)
        _manifest(manifest, "mhp3rd-texture-transfer-instrumentation-v1",
                  f"generated_unit_{number}", count, source, output)
        paths.update({f"transfer_{number}_input": source,
                      f"transfer_{number}_output": output,
                      f"transfer_{number}_manifest": manifest})
    return paths


def check(elf: Path, overlay: Path, module: Path, oracle: Path,
          build_dir: Path, output: Path, sanitized: bool = False) -> dict:
    if output.exists() or output.is_symlink():
        raise ValueError("Refusing to overwrite existing G1a evidence")
    profile = Path(__file__).resolve().parents[1]
    root = profile.parents[1]
    paths: dict[str, Path] = {
        "elf": elf.resolve(), "lobby_image": overlay.resolve(),
        "lobby_module": module.resolve(), "oracle_binary": oracle.resolve(),
        "runner": Path(__file__).resolve(),
        "hash_helpers": Path(__file__).with_name("check_texture_allocation.py").resolve(),
    }
    for relative in (
        "CMakeLists.txt", "cmake/TextureLifetimeInstrumentation.cmake",
        "cmake/TextureTransferInstrumentation.cmake", "tools/instrument_texture_lifetime.py",
        "tools/instrument_texture_transfer.py", "tests/texture_lifetime_oracle.cpp",
        "tests/texture_transfer_observation_oracle.cpp", "host/native/texture_command_dispatch.hpp",
        "host/native/texture_command_dispatch.cpp", "host/native/texture_lifetime_tracker.hpp",
        "host/native/texture_lifetime_tracker.cpp", "host/native/texture_transfer_tracker.hpp",
        "host/native/texture_transfer_tracker.cpp", "host/resources/source_authority.hpp",
        "host/resources/source_authority.cpp"):
        paths[relative] = profile / relative
    paths.update(_instrumented_paths(profile, build_dir.resolve()))
    before = {name: digest(path) for name, path in paths.items()}
    output.parent.mkdir(parents=True, exist_ok=True)
    sanitizer_options = ("ASAN_OPTIONS=halt_on_error=1:abort_on_error=1; "
                         "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1") if sanitized else None
    with tempfile.TemporaryDirectory(prefix="texture-transfer-observation-", dir=output.parent) as temporary:
        stage = Path(temporary)
        raw = stage / "original.json"
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("MHP3RD_", "PSPRECOMP_", "ASAN_OPTIONS", "UBSAN_OPTIONS"))}
        if sanitized:
            env["ASAN_OPTIONS"] = "halt_on_error=1:abort_on_error=1"
            env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        result = subprocess.run(
            [str(paths["oracle_binary"]), str(paths["elf"]), str(paths["lobby_image"]),
             str(paths["lobby_module"]), str(raw.resolve())],
            env=env, capture_output=True, text=True, timeout=300, check=False)
        if result.returncode:
            raise ValueError("Compiled G1a oracle failed: " + result.stderr[-3000:])
        if not raw.is_file() or raw.stat().st_size > 65536:
            raise ValueError("G1a report is missing or exceeds metadata bound")
        report = json.loads(raw.read_text(), object_pairs_hook=unique_object)
        validate(report)
        if before != {name: digest(path) for name, path in paths.items()}:
            raise ValueError("A G1a input, source, manifest or binary changed during execution")
        final = {
            "schema_version": 1,
            "recorded_at": datetime.now(timezone.utc).isoformat(),
            "scope": "compiled-original-g1a-observation",
            "success": True,
            "original_report": report,
            "original_report_sha256": digest(raw),
            "sha256": before,
            "paths": {key: str(value) for key, value in paths.items()},
            "sanitizer_options": sanitizer_options,
            "limitations": [
                "Only owner load/provider, manager enqueue and descriptor generation are observed.",
                "The PSP event-notification import is modeled; no asynchronous worker or game is run.",
                "Requested descriptor lengths do not certify bytes loaded into the owner slot.",
                "Pending destination writers remain live; no completion, quiescence or source readiness is published.",
                "Code epochs, production writer exclusion and the Off/Verify/Native controller remain unfinished.",
            ],
        }
        staged = stage / "published.json"
        staged.write_text(json.dumps(final, indent=2) + "\n")
        os.link(staged, output)
    print("G1a source-bound oracle passed: "
          f"report_sha256={final['original_report_sha256']} "
          f"oracle_sha256={before['oracle_binary']} "
          f"identities={len(before)} faults={report['fault_cases']} "
          f"capacity_losses={report['capacity_loss_cases']} "
          f"range_faults={report['range_fault_cases']} "
          "transfer_readiness=false")
    return final


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("elf", "overlay", "module", "oracle", "build-dir", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--sanitized", action="store_true")
    args = parser.parse_args()
    try:
        check(**vars(args))
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
