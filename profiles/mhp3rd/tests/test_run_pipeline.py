"""Cross-language pipeline check using the real C++ recording lifecycle."""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from compare_test_runs import canonical_hash, compare_runs, write_report
from run_package import package_run, load_package


def run_pipeline(binary: Path, root: Path):
    cases = {"schema": "yakumo-case-catalog-v1", "cases": [{
        "id": "NATIVE-01", "version": 1, "title": "Synthetic C++ package integration",
        "steps": ["Emit constructed observations through the real writer"], "checkpoints": ["checked"],
        "required_probes": [{"entry": 0x08877818, "min_calls": 1}],
        "required_state_fields": ["health_current"], "human_acceptance": True}]}
    (root / "cases.json").write_text(json.dumps(cases, indent=2))

    def produce(role, mode):
        name = role + "-" + mode
        context = {"schema": "yakumo-run-context-v1", "run_id": name, "source_commit": "a" * 40,
                   "platform_os": "macos", "platform_arch": "arm64",
                   "elf_sha256": "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c",
                   **{key: "a" * 64 for key in ("game_sha256", "overlay_sha256", "starting_save_sha256",
                                                "config_sha256", "build_config_sha256")},
                   "case_catalog_sha256": canonical_hash(cases)}
        context_path = root / (name + "-context.json")
        context_path.write_text(json.dumps(context, indent=2))
        directory = root / (name + "-run")
        process = subprocess.run([str(binary), str(directory), role, name, canonical_hash(context), mode],
                                 capture_output=True, text=True, timeout=15)
        expected = 7 if mode == "interrupt" else 5 if mode == "recorder_error" else 4
        assert process.returncode == expected, process.stderr
        supervision = {"schema": "yakumo-supervisor-v1", "run_id": name, "status": "exited",
                       "exit_code": process.returncode, "stop_reason": "abrupt_exit" if mode == "interrupt" else "window closed"}
        supervisor_path = root / (name + "-supervisor.json")
        supervisor_path.write_text(json.dumps(supervision, indent=2))
        output = root / (name + "-package")
        source_before = (directory / "events.journal").read_bytes()
        package_run(directory, context_path, output, supervisor_path)
        assert (directory / "events.journal").read_bytes() == source_before
        loaded = load_package(output)
        assert loaded["validation"]["identity_binding"] == "bound", loaded["validation"]
        assert loaded["validation"]["metadata_complete"], loaded["validation"]
        assert loaded["validation"]["recording_complete"] == (mode not in ("interrupt", "recorder_error")), loaded["validation"]
        return output

    baseline = produce("baseline", "match")
    outcomes = {}
    for mode, expected in (("match", "observational_match"), ("mismatch", "confirmed_mismatch"),
                           ("interrupt", "incomplete"), ("recorder_error", "incomplete")):
        candidate = produce("candidate", mode)
        report = compare_runs(baseline, candidate, cases)
        assert report["outcome"] == expected, report
        write_report(report, root / (mode + "-report"))
        outcomes[mode] = report["outcome"]
    return outcomes


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        raise SystemExit("Usage: test_run_pipeline.py <fixture-binary> [new-output-directory]")
    binary = Path(sys.argv[1]).resolve(strict=True)
    if len(sys.argv) == 3:
        output = Path(sys.argv[2]); output.mkdir()
        result = run_pipeline(binary, output.resolve())
    else:
        with tempfile.TemporaryDirectory() as temporary:
            result = run_pipeline(binary, Path(temporary).resolve())
    print(json.dumps({"cross_language_pipeline": result, "game_launched": False}))
