#!/usr/bin/env python3
"""Run and source-bind the bounded compiled G1b-read prefix oracle."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import tempfile

from check_texture_allocation import digest, unique_object


EXPECTED_COUNTS = {
    "scenario_start": 0,
    "scenario_total": 75,
    "scenario_count": 75,
    "descriptor_byte_resampling_cases": 12,
    "checkpoint_correlation_fault_cases": 20,
    "register_argument_fault_cases": 11,
    "cached_uncached_alias_cases": 2,
    "owner_invalidation_cases": 11,
    "capacity_loss_cases": 2,
    "incomplete_attempt_cases": 1,
    "unowned_disjoint_reads": 3,
    "rejected_read_results": 1,
    "unowned_read_results": 3,
    "retry_route_fault_cases": 1,
    "unowned_history_cases": 2,
    "partial_descriptor_overlap_fault_cases": 1,
    "active_generation_reuse_cases": 1,
}

_CHECKPOINT_IDS = ("state8", "helper", "invoke", "result")
_DESCRIPTOR_SAMPLES = (("state8", 0), ("state8", 4), ("state8", 24),
                       ("helper", 2), ("helper", 8), ("helper", 27),
                       ("invoke", 12), ("invoke", 25), ("invoke", 28),
                       ("result", 16), ("result", 20), ("result", 26))
EXPECTED_SCENARIO_IDS = tuple([
    "exact-32", "exact-slot-boundary", "short-then-full", "zero-then-full",
    "negative-then-full", "minimum-negative-then-full", "several-shorts-then-full",
    "shorts-sum-to-request", "retry-only-prefix", "overread-result", "uncached-manager",
    "unowned-disjoint-read", "unowned-history-same-address",
    "unowned-history-uncached-alias", "retry-missing-second-state8",
    "synthetic-partial-descriptor-overlap",
    "synthetic-same-byte-commit-during-active-attempt",
    "owner-interleave-owner-reset", "owner-interleave-owner-free-reuse",
    "owner-interleave-same-id-reload",
] + [f"descriptor-resample-{checkpoint}-byte-{byte}"
     for checkpoint, byte in _DESCRIPTOR_SAMPLES] + [
    f"{prefix}-{checkpoint}"
    for checkpoint in _CHECKPOINT_IDS
    for prefix in ("dropped", "duplicate", "wrong-runtime", "wrong-context", "switched-thread")
] + [
    "state8-consumer-mismatch", "state8-manager-mismatch", "state8-record-mismatch",
    "helper-manager-mismatch", "helper-fd-mismatch", "helper-record-mismatch",
    "invoke-fd-mismatch", "invoke-scratch-mismatch", "invoke-request-mismatch",
    "result-request-mismatch", "result-record-mismatch",
] + [f"cached-uncached-manager-alias-{checkpoint}"
     for checkpoint in ("state8", "helper")] + [
    f"owner-invalidated-{checkpoint}-{event}"
    for checkpoint in _CHECKPOINT_IDS
    for event in ("reset", "free")
] + ["read-attempt-capacity", "read-attempt-serial-capacity"])

EXPECTED_COUNTERS = {
    "aot_interpreter_calls": 450,
    "modeled_read_imports": 85,
    "state8_entries": 79,
    "read_helper_entries": 58,
    "read_invocations": 43,
    "read_results": 27,
    "exact_read_results": 11,
    "retry_read_results": 12,
    "read_attempt_records": 40,
    "pending_writers_observed": 75,
}

_REGISTER_FAULTS = {
    "state8-consumer-mismatch", "state8-manager-mismatch", "state8-record-mismatch",
    "helper-manager-mismatch", "helper-fd-mismatch", "helper-record-mismatch",
    "invoke-fd-mismatch", "invoke-scratch-mismatch", "invoke-request-mismatch",
    "result-request-mismatch", "result-record-mismatch",
}
_CHECKPOINT_FAULT_PREFIXES = (
    "dropped-", "duplicate-", "wrong-runtime-", "wrong-context-", "switched-thread-",
)
_EVENT_COUNTERS = (
    "unowned_disjoint_reads", "rejected_read_results", "unowned_read_results",
)
_ADDITIVE_COUNTERS = tuple(EXPECTED_COUNTERS) + _EVENT_COUNTERS
SCENARIO_SHARD_SIZE = 25


def validate(report: dict) -> None:
    fixed = {
        "schema_version": 1,
        "scope": "original-g1b-state8-read-attempt-result-prefix",
        "success": True,
        "pending_writers_retained": True,
        "full_ram_vram_cpu_compared": True,
        "read_prefix_stopped_after_result": True,
        "copy_transform_terminal_observations": 0,
        "completion_receipts": 0,
        "transfer_readiness": False,
        "healthy_authority_exact_notready": True,
        "modeled_io_imports": True,
        "event_scheduling_modeled": True,
        "game_executed": False,
    }
    allowed = (set(fixed) | set(EXPECTED_COUNTS) | set(EXPECTED_COUNTERS) |
               {"max_interpreter_slices", "scenario_ids"})
    if type(report) is not dict or set(report) != allowed:
        raise ValueError("Missing or unrecognized G1b-read evidence fields")
    for key, value in fixed.items():
        if type(report.get(key)) is not type(value) or report[key] != value:
            raise ValueError("G1b-read report exceeds its evidence scope: " + key)
    for key, value in EXPECTED_COUNTS.items():
        if type(report.get(key)) is not int or report[key] != value:
            raise ValueError("G1b-read coverage differs: " + key)
    if type(report.get("scenario_ids")) is not list or tuple(report["scenario_ids"]) != EXPECTED_SCENARIO_IDS:
        raise ValueError("G1b-read fixed scenario identities differ")
    for key, value in EXPECTED_COUNTERS.items():
        if type(report.get(key)) is not int or report[key] != value:
            raise ValueError("G1b-read fixed matrix counter differs: " + key)
    if type(report.get("max_interpreter_slices")) is not int:
        raise ValueError("G1b-read interpreter budget is missing")
    if (report["read_results"] > report["read_invocations"] or
            report["exact_read_results"] + report["retry_read_results"] +
            report["rejected_read_results"] + report["unowned_read_results"] !=
                report["read_results"] or
            report["retry_read_results"] == 0 or
            report["pending_writers_observed"] <
                report["scenario_count"] - report["unowned_disjoint_reads"] or
            report["read_invocations"] - report["unowned_disjoint_reads"] !=
                report["read_attempt_records"] or
            report["modeled_read_imports"] < report["read_invocations"]):
        raise ValueError("G1b-read outcome, retry or writer-retention totals differ")
    if not 0 < report["max_interpreter_slices"] < 2_000_000:
        raise ValueError("G1b-read interpreter budget is missing or exceeded")


def _scenario_categories(scenario_ids: list[str]) -> dict[str, int]:
    return {
        "scenario_count": len(scenario_ids),
        "descriptor_byte_resampling_cases": sum(
            name.startswith("descriptor-resample-") for name in scenario_ids),
        "checkpoint_correlation_fault_cases": sum(
            name.startswith(_CHECKPOINT_FAULT_PREFIXES) for name in scenario_ids),
        "register_argument_fault_cases": sum(name in _REGISTER_FAULTS for name in scenario_ids),
        "cached_uncached_alias_cases": sum(
            name.startswith("cached-uncached-manager-alias-") for name in scenario_ids),
        "owner_invalidation_cases": sum(
            name.startswith(("owner-interleave-", "owner-invalidated-"))
            for name in scenario_ids),
        "capacity_loss_cases": sum(name.startswith("read-attempt-") for name in scenario_ids),
        "incomplete_attempt_cases": sum(name == "dropped-result" for name in scenario_ids),
        "retry_route_fault_cases": sum(
            name == "retry-missing-second-state8" for name in scenario_ids),
        "unowned_history_cases": sum(name.startswith("unowned-history-") for name in scenario_ids),
        "partial_descriptor_overlap_fault_cases": sum(
            name == "synthetic-partial-descriptor-overlap" for name in scenario_ids),
        "active_generation_reuse_cases": sum(
            name == "synthetic-same-byte-commit-during-active-attempt"
            for name in scenario_ids),
    }


def _validate_shard(report: dict, start: int, count: int) -> None:
    expected_keys = (set(EXPECTED_COUNTS) | set(EXPECTED_COUNTERS) |
                     set(_EVENT_COUNTERS) | {"schema_version", "scope", "success",
                     "max_interpreter_slices", "scenario_ids", "scenario_total",
                     "scenario_start", "pending_writers_retained",
                     "full_ram_vram_cpu_compared", "read_prefix_stopped_after_result",
                     "copy_transform_terminal_observations", "completion_receipts",
                     "transfer_readiness", "healthy_authority_exact_notready",
                     "modeled_io_imports", "event_scheduling_modeled", "game_executed"})
    if type(report) is not dict or set(report) != expected_keys:
        raise ValueError("Missing or unrecognized G1b-read shard fields")
    fixed = {
        "schema_version": 1,
        "scope": "original-g1b-state8-read-attempt-result-prefix",
        "success": True,
        "pending_writers_retained": True,
        "full_ram_vram_cpu_compared": True,
        "read_prefix_stopped_after_result": True,
        "copy_transform_terminal_observations": 0,
        "completion_receipts": 0,
        "transfer_readiness": False,
        "healthy_authority_exact_notready": True,
        "modeled_io_imports": True,
        "event_scheduling_modeled": True,
        "game_executed": False,
    }
    for key, value in fixed.items():
        if type(report.get(key)) is not type(value) or report[key] != value:
            raise ValueError("G1b-read shard exceeds its evidence scope: " + key)
    ids = report.get("scenario_ids")
    if (type(report.get("scenario_start")) is not int or report["scenario_start"] != start or
            type(report.get("scenario_total")) is not int or
            report["scenario_total"] != len(EXPECTED_SCENARIO_IDS) or
            type(ids) is not list or tuple(ids) != EXPECTED_SCENARIO_IDS[start:start + count] or
            len(ids) != count):
        raise ValueError("G1b-read shard scenario range or identities differ")
    for key, value in _scenario_categories(ids).items():
        if report.get(key) != value:
            raise ValueError("G1b-read shard category count differs: " + key)
    integer_counters = set(EXPECTED_COUNTERS) | set(_EVENT_COUNTERS) | {"max_interpreter_slices"}
    for key in integer_counters:
        if type(report.get(key)) is not int or report[key] < 0:
            raise ValueError("G1b-read shard counter is invalid: " + key)
    if (report["read_results"] > report["read_invocations"] or
            report["exact_read_results"] + report["retry_read_results"] +
            report["rejected_read_results"] + report["unowned_read_results"] !=
                report["read_results"] or
            report["pending_writers_observed"] < count - report["unowned_disjoint_reads"] or
            report["read_invocations"] - report["unowned_disjoint_reads"] !=
                report["read_attempt_records"] or
            report["modeled_read_imports"] < report["read_invocations"] or
            not 0 < report["max_interpreter_slices"] < 2_000_000):
        raise ValueError("G1b-read shard outcome or writer totals differ")


def _merge_shards(reports: list[dict], shard_hashes: list[str]) -> dict:
    if len(reports) != len(shard_hashes) or not reports:
        raise ValueError("G1b-read shard set is incomplete")
    merged = dict(reports[0])
    merged["scenario_start"] = 0
    merged["scenario_total"] = len(EXPECTED_SCENARIO_IDS)
    merged["scenario_ids"] = []
    for key in set(EXPECTED_COUNTS) | set(_ADDITIVE_COUNTERS):
        if key not in {"scenario_start", "scenario_total", "scenario_count"}:
            merged[key] = 0
    merged["scenario_count"] = 0
    merged["max_interpreter_slices"] = 0
    merged["scenario_shards"] = []
    expected_start = 0
    for report, raw_hash in zip(reports, shard_hashes):
        count = min(SCENARIO_SHARD_SIZE, len(EXPECTED_SCENARIO_IDS) - expected_start)
        _validate_shard(report, expected_start, count)
        merged["scenario_ids"].extend(report["scenario_ids"])
        merged["scenario_count"] += report["scenario_count"]
        merged["max_interpreter_slices"] = max(
            merged["max_interpreter_slices"], report["max_interpreter_slices"])
        for key in (set(EXPECTED_COUNTS) | set(_ADDITIVE_COUNTERS)) - {
                "scenario_start", "scenario_total", "scenario_count"}:
            merged[key] += report[key]
        merged["scenario_shards"].append({
            "scenario_start": expected_start,
            "scenario_count": count,
            "raw_report_sha256": raw_hash,
        })
        expected_start += count
    if tuple(merged["scenario_ids"]) != EXPECTED_SCENARIO_IDS:
        raise ValueError("G1b-read shards have missing, duplicate or reordered scenarios")
    del merged["scenario_shards"]
    validate(merged)
    return merged


def _read_manifest(manifest_path: Path, unit: str, source: Path,
                   output: Path) -> dict:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"),
                          object_pairs_hook=unique_object)
    checkpoint_count = 2
    if (manifest.get("schema") != "mhp3rd-texture-read-instrumentation-v1" or
            manifest.get("unit") != unit or
            manifest.get("checkpoint_calls") != checkpoint_count or
            manifest.get("source_sha256") != digest(source) or
            manifest.get("output_sha256") != digest(output) or
            manifest.get("oracle_stop_after_read_result") != (unit == "generated_unit_0024")):
        raise ValueError("Build-local read instrumentation identity differs: " + unit)
    return manifest


def _manifest_source(profile: Path, build_dir: Path, output: Path,
                     manifest: Path, schema: str, unit: str,
                     checkpoint_calls: int) -> Path:
    record = json.loads(manifest.read_text(encoding="utf-8"),
                        object_pairs_hook=unique_object)
    if (record.get("schema") != schema or record.get("unit") != unit or
            record.get("checkpoint_calls") != checkpoint_calls or
            len(record.get("checkpoints", [])) != checkpoint_calls or
            not output.is_file() or digest(output) != record.get("output_sha256")):
        raise ValueError("Build-local instrumentation output/manifest differs: " + unit)
    name = unit + ".cpp"
    candidates = [build_dir / "profiles/mhp3rd" / folder / name for folder in (
        "texture_read_generated", "texture_transfer_generated",
        "texture_lifetime_generated", "texture_command_generated",
        "probe_generated", "generated")]
    candidates.append(profile / "generated" / name)
    source = next((path for path in candidates
                   if path.is_file() and digest(path) == record.get("source_sha256")), None)
    if source is None:
        raise ValueError("Could not resolve the instrumented source identity: " + unit)
    return source


def _upstream_paths(profile: Path, build_dir: Path) -> dict[str, Path]:
    paths: dict[str, Path] = {}
    for number, checkpoint_calls in (("0029", 5), ("0040", 1),
                                     ("0043", 5), ("0046", 3)):
        unit = "generated_unit_" + number
        output = build_dir / "profiles/mhp3rd/texture_lifetime_generated" / (unit + ".cpp")
        manifest = output.with_suffix(".cpp.json")
        source = _manifest_source(
            profile, build_dir, output, manifest,
            "mhp3rd-texture-lifetime-instrumentation-v1", unit, checkpoint_calls)
        paths[f"lifetime_{number}_input"] = source
        paths[f"lifetime_{number}_output"] = output
        paths[f"lifetime_{number}_manifest"] = manifest
        if source.parent.name == "probe_generated":
            probe_manifest = source.with_suffix(".cpp.json")
            probe_record = json.loads(probe_manifest.read_text(encoding="utf-8"),
                                      object_pairs_hook=unique_object)
            raw_source = profile / "generated" / (unit + ".cpp")
            if (probe_record.get("schema") != "mhp3rd-probe-instrumentation-v1" or
                    probe_record.get("output_sha256") != digest(source) or
                    probe_record.get("source_sha256") != digest(raw_source)):
                raise ValueError("Probe-to-lifetime source chain differs: " + unit)
            paths[f"probe_{number}_input"] = raw_source
            paths[f"probe_{number}_output"] = source
            paths[f"probe_{number}_manifest"] = probe_manifest
    for number, checkpoint_calls in (("0023", 3), ("0040", 2)):
        unit = "generated_unit_" + number
        output = build_dir / "profiles/mhp3rd/texture_transfer_generated" / (unit + ".cpp")
        manifest = output.with_suffix(".cpp.json")
        source = _manifest_source(
            profile, build_dir, output, manifest,
            "mhp3rd-texture-transfer-instrumentation-v1", unit, checkpoint_calls)
        paths[f"transfer_{number}_input"] = source
        paths[f"transfer_{number}_output"] = output
        paths[f"transfer_{number}_manifest"] = manifest
    return paths


def _read_paths(profile: Path, build_dir: Path) -> dict[str, Path]:
    read_dir = build_dir / "profiles/mhp3rd/texture_read_generated"
    unit23 = read_dir / "generated_unit_0023.cpp"
    unit24 = read_dir / "generated_unit_0024.cpp"
    upstream = _upstream_paths(profile, build_dir)
    transfer23 = upstream["transfer_0023_output"]
    original24 = profile / "generated/generated_unit_0024.cpp"
    _read_manifest(unit23.with_suffix(".cpp.json"), "generated_unit_0023",
                   transfer23, unit23)
    _read_manifest(unit24.with_suffix(".cpp.json"), "generated_unit_0024",
                   original24, unit24)
    return {
        **upstream,
        "transfer_0023": transfer23,
        "read_0023_input": transfer23,
        "read_0023_output": unit23,
        "read_0023_manifest": unit23.with_suffix(".cpp.json"),
        "read_0024_input": original24,
        "read_0024_output": unit24,
        "read_0024_manifest": unit24.with_suffix(".cpp.json"),
    }


def check(elf: Path, overlay: Path, module: Path, oracle: Path,
          build_dir: Path, output: Path, sanitized: bool = False) -> dict:
    if output.exists() or output.is_symlink():
        raise ValueError("Refusing to overwrite existing G1b-read evidence")
    profile = Path(__file__).resolve().parents[1]
    paths: dict[str, Path] = {
        "elf": elf.resolve(),
        "lobby_image": overlay.resolve(),
        "lobby_module": module.resolve(),
        "oracle_binary": oracle.resolve(),
        "runner": Path(__file__).resolve(),
        "hash_helpers": Path(__file__).with_name("check_texture_allocation.py").resolve(),
    }
    for relative in (
        "CMakeLists.txt", "cmake/ProbeInstrumentation.cmake",
        "cmake/TextureReadInstrumentation.cmake",
        "cmake/TextureLifetimeInstrumentation.cmake",
        "cmake/TextureTransferInstrumentation.cmake",
        "tools/instrument_probes.py", "tools/instrument_texture_lifetime.py",
        "tools/instrument_texture_read.py",
        "tools/instrument_texture_transfer.py",
        "tests/texture_lifetime_oracle.cpp", "tests/texture_read_observation_oracle.cpp",
        "tests/texture_read_oracle_stop.hpp", "host/native/texture_command_dispatch.hpp",
        "host/native/texture_command_dispatch.cpp", "host/native/texture_lifetime_tracker.hpp",
        "host/native/texture_lifetime_tracker.cpp", "host/native/texture_transfer_tracker.hpp",
        "host/native/texture_transfer_tracker.cpp", "host/resources/source_authority.hpp",
        "host/resources/source_authority.cpp"):
        paths[relative] = profile / relative
    paths.update(_read_paths(profile, build_dir.resolve()))
    cache = build_dir.resolve() / "CMakeCache.txt"
    cache_text = cache.read_text(encoding="utf-8")
    if ("MHP3RD_TEXTURE_READ_BOUNDARIES:BOOL=ON" not in cache_text or
           "MHP3RD_TEXTURE_TRANSFER_BOUNDARIES:BOOL=ON" not in cache_text or
            "MHP3RD_TEXTURE_LIFETIME_BOUNDARIES:BOOL=ON" not in cache_text or
            "MHP3RD_CERTIFIED_PROBES:BOOL=OFF" not in cache_text):
        raise ValueError("G1b-read build does not have the isolated optional boundary configuration")
    paths["build_cache"] = cache
    before = {name: digest(path) for name, path in paths.items()}
    output.parent.mkdir(parents=True, exist_ok=True)
    sanitizer_options = ("ASAN_OPTIONS=halt_on_error=1:abort_on_error=1; "
                         "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1") if sanitized else None
    with tempfile.TemporaryDirectory(prefix="texture-read-observation-", dir=output.parent) as temporary:
        stage = Path(temporary)
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("MHP3RD_", "PSPRECOMP_", "ASAN_OPTIONS", "UBSAN_OPTIONS"))}
        if sanitized:
            env["ASAN_OPTIONS"] = "halt_on_error=1:abort_on_error=1"
            env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        shard_reports: list[dict] = []
        shard_hashes: list[str] = []
        for scenario_start in range(0, len(EXPECTED_SCENARIO_IDS), SCENARIO_SHARD_SIZE):
            scenario_count = min(SCENARIO_SHARD_SIZE,
                                 len(EXPECTED_SCENARIO_IDS) - scenario_start)
            raw = stage / f"original-{scenario_start:03d}.json"
            result = subprocess.run(
                [str(paths["oracle_binary"]), str(paths["elf"]),
                 str(paths["lobby_image"]), str(paths["lobby_module"]),
                 str(raw.resolve()), str(scenario_start), str(scenario_count)],
                env=env, capture_output=True, text=True, timeout=300, check=False)
            if result.returncode:
                raise ValueError("Compiled G1b-read shard failed at scenario " +
                                 str(scenario_start) + ": " + result.stderr[-4000:])
            if not raw.is_file() or raw.stat().st_size > 65536:
                raise ValueError("G1b-read shard report is missing or exceeds metadata bound")
            shard_reports.append(json.loads(raw.read_text(encoding="utf-8"),
                                             object_pairs_hook=unique_object))
            shard_hashes.append(digest(raw))
        report = _merge_shards(shard_reports, shard_hashes)
        if before != {name: digest(path) for name, path in paths.items()}:
            raise ValueError("A G1b-read input, source, manifest, cache or binary changed during execution")
        aggregate = stage / "aggregate.json"
        aggregate.write_text(json.dumps(report, sort_keys=True, separators=(",", ":")) + "\n",
                             encoding="utf-8")
        final = {
            "schema_version": 1,
            "recorded_at": datetime.now(timezone.utc).isoformat(),
            "scope": "compiled-original-g1b-read-prefix-observation",
            "success": True,
            "original_report": report,
            "original_report_sha256": digest(aggregate),
            "scenario_shards": [
                {"scenario_start": start,
                 "scenario_count": min(SCENARIO_SHARD_SIZE,
                     len(EXPECTED_SCENARIO_IDS) - start),
                 "raw_report_sha256": raw_hash}
                for start, raw_hash in zip(
                    range(0, len(EXPECTED_SCENARIO_IDS), SCENARIO_SHARD_SIZE), shard_hashes)
            ],
            "sha256": before,
            "sanitized": sanitized,
            "sanitizer_options": sanitizer_options,
            "limitations": [
                "The sceIoRead import and event scheduling are modeled; file identity, open and seek are not exercised.",
                "The state-8 worker remains synchronous in the oracle; concurrent scheduler interleavings are not proven.",
                "Each test stops after ReadResult before the original comparison, copy, transform or retirement path.",
                "An exact positive syscall result is only one observed attempt, not proof of completed target bytes.",
                "Pending destination writers remain live and source readiness remains exactly NotReady.",
                "The fixed scenario matrix ran in contiguous shards; their ordered IDs were checked for complete, duplicate-free coverage under one source/binary identity.",
            ],
        }
        staged = stage / "published.json"
        staged.write_text(json.dumps(final, indent=2) + "\n", encoding="utf-8")
        os.link(staged, output)
    print("G1b-read source-bound prefix oracle passed: "
          f"report_sha256={final['original_report_sha256']} "
          f"oracle_sha256={before['oracle_binary']} "
          f"scenarios={report['scenario_count']} "
          f"shards={len(final['scenario_shards'])} "
          f"descriptor_bytes={report['descriptor_byte_resampling_cases']} "
          f"attempts={report['read_attempt_records']} "
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
