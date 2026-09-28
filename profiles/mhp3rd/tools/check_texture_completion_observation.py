#!/usr/bin/env python3
"""Run and source-bind the bounded G1c inline completion oracle."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


ELF_SHA256 = "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c"
OVERLAY_SHA256 = "c34bf34f5e71993f5f2d20cdc39ec1b965f64b66d46f8d7672b449fba64b5aca"
MODULE_SHA256 = "35d381ffb06ff45f7357d3ef1634719bcfd4d5810eba1de9b32ca0443b62f538"

INTEGRATION_REQUIRED = {
    "schema_version": 1,
    "scope": "compiled-g1c-texture-completion-integration",
    "success": True,
    "compiled_integration": True,
    "synthetic_completion_sequence_used": False,
    "fixture_supplied_successful_retirement": False,
    "transfer_readiness": False,
    "full_ram_vram_cpu_compared": True,
    "game_executed": False,
}
INTEGRATION_COUNT_FIELDS = (
    "source_completion_receipts", "writer_released", "writer_retained",
    "cpu_comparisons", "ram_comparisons", "vram_comparisons",
)


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def validate(report: dict) -> None:
    required = {
        "schema_version": 1,
        "scope": "original_g1c_inline_completion_observation",
        "success": True,
        "full_ram_vram_cpu_compared": True,
        "imports_modeled": True,
        "event_scheduling_modeled": True,
        "transform_worker_executed": True,
        "game_executed": False,
    }
    for key, value in required.items():
        if report.get(key) != value:
            raise ValueError(f"G1c report scope differs: {key}")
    for key in ("cases", "completion_event_cases", "completion_failed_cases",
                "copy_worker_cases", "transform_worker_cases", "rounded_write_cases"):
        if type(report.get(key)) is not int or report[key] < 0:
            raise ValueError(f"G1c counter is invalid: {key}")
    if report["cases"] != 64 or report["completion_event_cases"] <= 0 or \
            report["completion_failed_cases"] != 0 or report["transform_worker_cases"] == 0:
        raise ValueError("G1c completion coverage differs")


def validate_tracker_smoke(report: dict) -> None:
    required = {
        "schema_version": 1,
        "scope": "synthetic-g1c-completion-tracker-smoke",
        "success": True,
        "completion_started": 1,
        "completion_completed": 1,
        "live_writers": 0,
        "authority_failure": "None",
        "transfer_readiness": False,
        "source_completion_receipts": 0,
        "native_invocations": 0,
        "game_executed": False,
    }
    for key, value in required.items():
        if report.get(key) != value:
            raise ValueError(f"G1c tracker smoke differs: {key}")


def validate_integration(report: dict) -> None:
    """Validate the executable-owned real integration evidence contract.

    The runner checks the scope and refuses to manufacture any integration
    result. All outcome values copied into the published artifact below come
    from this report produced by the compiled fixture.
    """
    if type(report) is not dict:
        raise ValueError("G1c integration report is not an object")
    for key, value in INTEGRATION_REQUIRED.items():
        if report.get(key) != value:
            raise ValueError(f"G1c integration scope differs: {key}")
    for key in INTEGRATION_COUNT_FIELDS:
        if type(report.get(key)) is not int or report[key] < 0:
            raise ValueError(f"G1c integration counter is invalid: {key}")
    if report["writer_released"] == 0:
        raise ValueError("G1c integration did not release a writer")
    if report["cpu_comparisons"] == 0 or report["ram_comparisons"] == 0 or \
            report["vram_comparisons"] == 0:
        raise ValueError("G1c integration comparison coverage is empty")
    named_cases = report.get("named_cases")
    if type(named_cases) is not list or not named_cases or \
            any(type(case) is not str or not case for case in named_cases) or \
            len(set(named_cases)) != len(named_cases):
        raise ValueError("G1c integration named cases are invalid")


def check(elf: Path, overlay: Path, module: Path, encoded: Path, decoded: Path,
          oracle: Path | None, build_dir: Path, output: Path, sanitized: bool = False,
          integration_oracle: Path | None = None,
          decoder_source: Path | None = None,
          integration_fixture: Path | None = None,
          stop_header: Path | None = None) -> dict:
    integration = integration_oracle is not None
    if integration and oracle is not None:
        raise ValueError("Pass either --oracle or --integration-oracle, not both")
    if not integration and oracle is None:
        raise ValueError("An oracle executable is required")
    if integration and (decoder_source is None or integration_fixture is None):
        raise ValueError("Integration mode requires --decoder-source and --integration-fixture")
    if output.exists() or output.is_symlink():
        raise ValueError("Refusing to overwrite existing G1c evidence")
    if digest(elf) != ELF_SHA256 or digest(overlay) != OVERLAY_SHA256 or \
            digest(module) != MODULE_SHA256:
        raise ValueError("Unsupported original input identity")
    if not encoded.is_file() or not decoded.is_file():
        raise ValueError("Missing private transform fixtures")
    profile = Path(__file__).resolve().parents[1]
    selected_oracle = integration_oracle if integration else oracle
    paths = {
        "elf": elf.resolve(),
        "overlay": overlay.resolve(),
        "module": module.resolve(),
        "encoded": encoded.resolve(),
        "decoded": decoded.resolve(),
        "oracle": selected_oracle.resolve(),
        "runner": Path(__file__).resolve(),
        "completion_header": profile / "host/native/texture_completion_progress.hpp",
        "dispatch_header": profile / "host/native/texture_command_dispatch.hpp",
        "dispatch_cpp": profile / "host/native/texture_command_dispatch.cpp",
        "tracker_header": profile / "host/native/texture_transfer_tracker.hpp",
        "tracker_cpp": profile / "host/native/texture_transfer_tracker.cpp",
        "completion_cpp": profile / "host/native/texture_transfer_completion.cpp",
        "completion_oracle_cpp": profile / "tests/texture_completion_observation_oracle.cpp",
        "lifetime_oracle_cpp": profile / "tests/texture_lifetime_oracle.cpp",
        "tracker_smoke_cpp": profile / "tests/texture_completion_tracker_smoke.cpp",
        "transfer_oracle_cpp": profile / "tests/texture_transfer_oracle.cpp",
        "completion_cmake": profile / "cmake/TextureCompletionInstrumentation.cmake",
        "completion_instrumenter": profile / "tools/instrument_texture_completion.py",
        "read_instrumenter": profile / "tools/instrument_texture_read.py",
        "cache": build_dir.resolve() / "CMakeCache.txt",
        "completion_stop_header": (stop_header or
            profile / "tests/texture_completion_oracle_stop.hpp").resolve(),
        "completion_output_0023": build_dir /
            "profiles/mhp3rd/texture_completion_generated/generated_unit_0023.cpp",
        "completion_output_0024": build_dir /
            "profiles/mhp3rd/texture_completion_generated/generated_unit_0024.cpp",
        "completion_manifest_0023": build_dir / "profiles/mhp3rd/texture_completion_generated/generated_unit_0023.cpp.json",
        "completion_manifest_0024": build_dir / "profiles/mhp3rd/texture_completion_generated/generated_unit_0024.cpp.json",
    }
    if not integration:
        paths["tracker_smoke"] = (build_dir / "bin" /
            ("mhp3rd_texture_completion_tracker_smoke_sanitized"
             if sanitized else "mhp3rd_texture_completion_tracker_smoke")).resolve()
    else:
        paths["integration_oracle"] = integration_oracle.resolve()
        paths["decoder_source"] = decoder_source.resolve()
        # The fixture is the source translation unit compiled into the
        # integration executable. It is an identity input for the report, not
        # a runtime argument supplied to that executable.
        paths["integration_fixture"] = integration_fixture.resolve()
    for name, path in paths.items():
        if not path.is_file() or path.is_symlink():
            raise ValueError(f"Missing G1c identity input: {name}")
    cache = paths["cache"].read_text(encoding="utf-8")
    required_options = (
        "MHP3RD_TEXTURE_COMPLETION_BOUNDARIES:BOOL=ON",
        "MHP3RD_TEXTURE_TRANSFER_BOUNDARIES:BOOL=ON",
        "MHP3RD_TEXTURE_READ_BOUNDARIES:BOOL=ON",
        "MHP3RD_TEXTURE_LIFETIME_BOUNDARIES:BOOL=ON",
        "MHP3RD_CERTIFIED_PROBES:BOOL=OFF",
    )
    if any(option not in cache for option in required_options):
        raise ValueError("G1c build options are not isolated")
    before = {name: digest(path) for name, path in paths.items()}
    output.parent.mkdir(parents=True, exist_ok=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MHP3RD_", "PSPRECOMP_", "ASAN_OPTIONS", "UBSAN_OPTIONS"))}
    if sanitized:
        env["ASAN_OPTIONS"] = "halt_on_error=1:abort_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    with tempfile.TemporaryDirectory(prefix="texture-completion-", dir=output.parent) as tmp:
        raw = Path(tmp) / "original.json"
        smoke_raw = None
        smoke_report = None
        if not integration:
            smoke_raw = Path(tmp) / "tracker-smoke.json"
            smoke = subprocess.run([
                str(paths["tracker_smoke"]), str(elf), str(overlay), str(module), str(smoke_raw)
            ], env=env, capture_output=True, text=True, timeout=300, check=False)
            if smoke.returncode:
                raise ValueError("G1c tracker smoke failed: " + smoke.stderr[-4000:])
            smoke_report = json.loads(smoke_raw.read_text(encoding="utf-8"))
            validate_tracker_smoke(smoke_report)
        run = subprocess.run([str(selected_oracle), str(elf), str(overlay), str(module),
                              str(raw), str(encoded), str(decoded)],
                             env=env, capture_output=True, text=True, timeout=300, check=False)
        if run.returncode:
            raise ValueError("G1c oracle failed: " + run.stderr[-4000:])
        report = json.loads(raw.read_text(encoding="utf-8"))
        if integration:
            validate_integration(report)
        else:
            validate(report)
        if before != {name: digest(path) for name, path in paths.items()}:
            raise ValueError("G1c identity input changed during execution")
        if integration:
            published = {
                "schema_version": 1,
                "recorded_at": datetime.now(timezone.utc).isoformat(),
                "scope": "compiled-g1c-texture-completion-integration",
                "success": True,
                "compiled_integration": report["compiled_integration"],
                "synthetic_completion_sequence_used": report[
                    "synthetic_completion_sequence_used"],
                "fixture_supplied_successful_retirement": report[
                    "fixture_supplied_successful_retirement"],
                "transfer_readiness": report["transfer_readiness"],
                "source_completion_receipts": report["source_completion_receipts"],
                "named_cases": report["named_cases"],
                "writer_released": report["writer_released"],
                "writer_retained": report["writer_retained"],
                "cpu_comparisons": report["cpu_comparisons"],
                "ram_comparisons": report["ram_comparisons"],
                "vram_comparisons": report["vram_comparisons"],
                "full_ram_vram_cpu_compared": report["full_ram_vram_cpu_compared"],
                "integration_report": report,
                "integration_report_sha256": digest(raw),
                "sha256": before,
                "sanitized": sanitized,
                "sanitizer_options": (
                    "ASAN_OPTIONS=halt_on_error=1:abort_on_error=1; "
                    "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1") if sanitized else None,
                "limitations": [
                    "The integration fixture owns the completion report; this runner does not synthesize completion events.",
                    "SourceAuthority remains NotReady until the executable reports a future source receipt contract.",
                    "The application target and Native mode remain unchanged.",
                ],
            }
        else:
            published = {
                "schema_version": 1,
                "recorded_at": datetime.now(timezone.utc).isoformat(),
                "scope": "compiled-original-g1c-inline-completion-observation",
                "success": True,
                "original_report": report,
                "tracker_smoke_report": smoke_report,
                "original_report_sha256": digest(raw),
                "tracker_smoke_report_sha256": digest(smoke_raw),
                "sha256": before,
                "sanitized": sanitized,
                "sanitizer_options": (
                    "ASAN_OPTIONS=halt_on_error=1:abort_on_error=1; "
                    "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1") if sanitized else None,
                "limitations": [
                    "I/O and scheduler behavior are modeled; file identity/open/seek are not proven.",
                    "The completion callback observes the original path; Native application mode remains off.",
                    "Unsupported copy-worker routes retain their writer and are not counted as completed.",
                    "A completed external writer does not establish SourceAuthority source readiness.",
                ],
            }
        staged = Path(tmp) / "published.json"
        staged.write_text(json.dumps(published, indent=2) + "\n", encoding="utf-8")
        os.link(staged, output)
    if integration:
        print(f"G1c integration oracle passed: cases={len(report['named_cases'])} "
              f"released={report['writer_released']} retained={report['writer_retained']} "
              f"source_receipts={report['source_completion_receipts']} "
              f"transfer_readiness={report['transfer_readiness']}")
    else:
        print(f"G1c completion oracle passed: cases={report['cases']} "
              f"events={report['completion_event_cases']} "
              f"tracker_completed={smoke_report['completion_completed']} "
              f"unsupported_copy={report['copy_worker_cases']} "
              "transfer_readiness=false")
    return published


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in ("elf", "overlay", "module", "encoded", "decoded", "build-dir", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--oracle", type=Path)
    parser.add_argument("--integration-oracle", type=Path,
                        help="Run the real completion integration executable")
    parser.add_argument("--decoder-source", type=Path,
                        help="Final decoder implementation source bound to integration evidence")
    parser.add_argument("--integration-fixture", type=Path,
                        help="Fixture source bound to the compiled integration executable; identity only")
    parser.add_argument("--stop-header", type=Path,
                        help="Completion terminal-stop header to hash")
    parser.add_argument("--sanitized", action="store_true")
    args = parser.parse_args()
    try:
        check(**vars(args))
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
