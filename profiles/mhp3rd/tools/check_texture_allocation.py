#!/usr/bin/env python3
"""Run the local original allocation/caller gate with bounded, hash-bound evidence."""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ELF_HASH = "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c"
EXPECTED = {"original_calls_per_path": 168, "allocation_calls": 68, "free_calls": 60,
            "initializations_and_resets": 24, "caller_tail_cases": 16,
            "same_address_reuses": 6, "virtual_source_provider_cases": 8,
            "reverse_requested_alignment_residues": 10}


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate field: {key}")
        result[key] = value
    return result


def validate(report: dict) -> None:
    if (report.get("schema_version") != 1 or
            report.get("scope") != "original_heap_lifecycle_and_caller_tail" or
            report.get("success") is not True or report.get("elf_sha256") != ELF_HASH):
        raise ValueError("Unsupported or unsuccessful allocation report")
    for field, expected in EXPECTED.items():
        if type(report.get(field)) is not int or report[field] != expected:
            raise ValueError(f"Missing or changed allocation coverage: {field}")
    for field in ("full_ram_vram_cpu_compared", "virtual_source_provider_executed"):
        if report.get(field) is not True:
            raise ValueError(f"Missing actual execution comparison: {field}")
    for field in ("object_constructor_and_resource_loader_executed", "live_allocation_authority_installed"):
        if report.get(field) is not False:
            raise ValueError(f"Unsupported integration claim: {field}")
    steps = report.get("max_interpreter_slices")
    if type(steps) is not int or not 0 < steps <= 100000:
        raise ValueError("Interpreter budget missing or exceeded")


def check(elf: Path, oracle: Path, output: Path) -> dict:
    if output.exists() or output.is_symlink():
        raise ValueError("Refusing to overwrite evidence")
    root = Path(__file__).resolve().parents[3]
    paths = {"elf": elf.resolve(), "oracle_binary": oracle.resolve(),
             "oracle_source": root / "profiles/mhp3rd/tests/texture_allocation_oracle.cpp",
             "runner_source": Path(__file__).resolve()}
    before = {name: digest(path) for name, path in paths.items()}
    if before["elf"] != ELF_HASH:
        raise ValueError("Unsupported original executable")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="allocation-gate-", dir=output.parent) as temporary:
        staging = Path(temporary)
        raw = staging / "original.json"
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("MHP3RD_", "PSPRECOMP_"))}
        completed = subprocess.run([str(paths["oracle_binary"]), str(paths["elf"]), str(raw)],
                                   env=env, capture_output=True, text=True, timeout=60, check=False)
        if completed.returncode != 0:
            raise ValueError(f"Original gate failed: {completed.stderr[-2000:]}")
        if raw.stat().st_size > 65536:
            raise ValueError("Original report exceeds metadata budget")
        report = json.loads(raw.read_text(), object_pairs_hook=unique_object)
        validate(report)
        after = {name: digest(path) for name, path in paths.items()}
        if before != after:
            raise ValueError("Source, executable or binary changed during gate")
        final = {"schema_version": 1, "recorded_at": datetime.now(timezone.utc).isoformat(),
                 "scope": "bounded_original_allocation_provenance", "success": True,
                 "original_report": report, "original_report_sha256": digest(raw),
                 "sha256": before, "paths": {name: str(path) for name, path in paths.items()},
                 "limits": {"seconds_per_process": 60, "interpreter_slices_per_call": 100000},
                 "limitations": ["Constructed owner, heap and source inputs; no resource loader or game run.",
                    "Provider and caller segments execute original code; actual owner creation and live readiness remain unresolved.",
                    "No production authority or hook installed; only the supported original executable is certified."]}
        staged = staging / "published.json"
        staged.write_text(json.dumps(final, indent=2) + "\n")
        os.link(staged, output)
        return final


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--oracle", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        report = check(args.elf, args.oracle, args.output)
        print(f"Allocation provenance gate: {report['original_report']['original_calls_per_path']} original calls per path; passed")
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"{error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
