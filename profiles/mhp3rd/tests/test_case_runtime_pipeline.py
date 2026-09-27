"""Independent catalog hashes and real C++ case-controller journal semantics."""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from run_cases import CASE_END, collect_cases, validate_case_catalog
from run_package import package_run, load_package, prerequisite_basis_sha256
from compare_test_runs import canonical_hash, compare_runs, write_report


def catalog():
    return {"schema": "yakumo-case-catalog-v1", "cases": [{
        "id": "REC-02", "version": 1, "title": "Movement and camera",
        "steps": ["Walk, stop, and turn on the agreed route."], "checkpoints": ["route_complete"],
        "required_probes": [], "required_state_fields": ["character_loaded"], "human_acceptance": True}]}


def pipeline(binary: Path, root: Path):
    cases = catalog(); case_path = root / "cases.json"; case_path.write_text(json.dumps(cases, indent=2))
    variants = [cases, json.loads(json.dumps(cases))]
    variants[1]["cases"][0]["title"] = 'Unicode \U0001F409 "quote" \\ percent %s'
    variants[1]["cases"][0]["required_probes"] = [{"entry": 0x08877818, "min_calls": (1 << 64) - 1}]
    for index, value in enumerate(variants):
        for escaped in (False, True):
            path = root / f"catalog-{index}-{escaped}.json"
            path.write_text(json.dumps(value, ensure_ascii=escaped, indent=4), encoding="utf-8")
            result = subprocess.run([str(binary), "--catalog", str(path)], capture_output=True, text=True, timeout=10)
            assert result.returncode == 0, result.stderr
            assert json.loads(result.stdout)["sha256"] == canonical_hash(validate_case_catalog(value))
    case_hash = canonical_hash(cases)

    def run(role, mode):
        run_id = role + "-" + mode
        context = {"schema": "yakumo-run-context-v1", "run_id": run_id, "source_commit": "a" * 40,
                   "platform_os": "macos", "platform_arch": "arm64",
                   "elf_sha256": "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c",
                   "case_catalog_sha256": case_hash,
                   **{key: "a" * 64 for key in ("game_sha256", "overlay_sha256", "starting_save_sha256", "config_sha256", "build_config_sha256")}}
        context_path = root / (run_id + "-context.json");context_path.write_text(json.dumps(context))
        basis = prerequisite_basis_sha256(context, "B0", "4292eb6")
        directory = root / (run_id + "-run")
        result = subprocess.run([str(binary), "--run", str(directory), role, run_id, canonical_hash(context),
                                 str(case_path), case_hash, basis, mode], capture_output=True, text=True, timeout=20)
        assert result.returncode == 4, result.stderr
        supervisor = root / (run_id + "-supervisor.json")
        supervisor.write_text(json.dumps({"schema": "yakumo-supervisor-v1", "run_id": run_id,
                                         "status": "exited", "exit_code": 4, "stop_reason": "window closed"}))
        output = root / (run_id + "-package")
        package_run(directory, context_path, output, supervisor)
        validation = load_package(output)["validation"]
        assert validation["recording_complete"] and validation["metadata_complete"], validation
        return output

    baseline = run("baseline", "normal")
    outcomes = {}
    for mode, expected in (("normal", "observational_match"), ("interrupt", "incomplete"),
                           ("changed_config", "incomplete"),
                           ("restored_config_save", "incomplete"),
                           ("restored_config_snapshot", "incomplete"),
                           ("navigation_only", "inconclusive"),
                           ("skip", "inconclusive")):
        candidate = run("candidate", mode)
        recorded = load_package(candidate)["records"]
        case = collect_cases(recorded, cases)["cases"][0]
        if mode.startswith("restored_config_"):
            assert case["outcome"] is None and not case["lifecycle_complete"], case
            assert not any(row["kind"] == CASE_END for row in case["events"]), case
            assert any(row["fields"].get("event") == "case.interrupted" for row in case["events"]), case
            snapshots = [row for row in case["events"] if row["fields"].get("event") == "config.effective"]
            assert len(snapshots) == 2, case
            assert snapshots[0]["fields"]["settings_sha256"] != snapshots[1]["fields"]["settings_sha256"], case
        elif mode == "navigation_only":
            assert case["outcome"] == "normal" and case["lifecycle_complete"], case
            assert not any(row["fields"].get("event") == "case.interrupted" for row in case["events"]), case
        report = compare_runs(baseline, candidate, cases)
        assert report["outcome"] == expected, report
        write_report(report, root / (mode + "-report"))
        outcomes[mode] = expected
    return outcomes


if __name__ == "__main__":
    binary = Path(sys.argv[1]).resolve(strict=True)
    if len(sys.argv) > 2:
        output = Path(sys.argv[2]);output.mkdir()
        result = pipeline(binary, output.resolve())
    else:
        with tempfile.TemporaryDirectory() as directory:
            result = pipeline(binary, Path(directory).resolve())
    print(json.dumps({"case_runtime_outcomes": result, "game_launched": False}))
