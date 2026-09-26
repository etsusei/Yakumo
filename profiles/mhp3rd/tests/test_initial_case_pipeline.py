"""Exercise the final case catalog through the real offline journal pipeline.

The fixture uses allocated zeroed guest memory and synthetic pad/time samples.
It does not execute game code, generate helper calls, or establish user acceptance.
"""
from __future__ import annotations

import json
from pathlib import Path
import platform
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from compare_test_runs import canonical_hash, compare_runs, write_report  # noqa: E402
from run_cases import collect_cases, load_case_catalog  # noqa: E402
from run_package import load_package, package_run, prerequisite_basis_sha256  # noqa: E402


EXPECTED_CASE_IDS = ("REC-01", "REC-02", "NATIVE-01", "REC-03")
SCALE_ENTRY = 0x08878B28
SUPPORTED_ELF_SHA256 = "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c"


def require(condition: bool, explanation: str) -> None:
    if not condition:
        raise AssertionError(explanation)


def run_fixture(binary: Path, catalog_path: Path, catalog_hash: str, root: Path,
                role: str, mode: str) -> Path:
    run_id = f"synthetic-{role}-{mode}"
    context = {
        "schema": "yakumo-run-context-v1", "run_id": run_id,
        "source_commit": "a" * 40,
        "platform_os": platform.system().lower(),
        "platform_arch": platform.machine().lower(),
        "elf_sha256": SUPPORTED_ELF_SHA256,
        "case_catalog_sha256": catalog_hash,
        **{key: "a" * 64 for key in (
            "game_sha256", "overlay_sha256", "starting_save_sha256",
            "config_sha256", "build_config_sha256")},
    }
    context_path = root / f"{run_id}-context.json"
    context_path.write_text(json.dumps(context), encoding="utf-8")
    basis = prerequisite_basis_sha256(context, "B0", "4292eb6")
    run_directory = root / f"{run_id}-run"
    result = subprocess.run(
        [str(binary), "--run-all", str(run_directory), role, run_id,
         canonical_hash(context), str(catalog_path), catalog_hash, basis, mode],
        capture_output=True, text=True, timeout=30, check=False,
    )
    require(result.returncode == 4,
            f"{role} {mode} fixture failed ({result.returncode}): {result.stderr}")
    supervisor = root / f"{run_id}-supervisor.json"
    supervisor.write_text(json.dumps({
        "schema": "yakumo-supervisor-v1", "run_id": run_id,
        "status": "exited", "exit_code": 4, "stop_reason": "window closed",
    }), encoding="utf-8")
    output = root / f"{run_id}-package"
    package_run(run_directory, context_path, output, supervisor)
    package = load_package(output)
    validation = package["validation"]
    require(validation["recording_complete"] and validation["metadata_complete"],
            f"{role} {mode} package is not complete: {validation}")
    require(validation["identity_binding"] == "bound",
            f"{role} {mode} package lost launch binding: {validation}")
    require(package["records"][0]["fields"].get("test_context") == "synthetic_all_case_pipeline",
            f"{role} {mode} package is not marked synthetic")
    return output


def check_complete_catalog(package_path: Path, catalog: dict) -> None:
    package = load_package(package_path)
    collected = collect_cases(package["records"], catalog)
    require(not collected["issues"], f"catalog reconstruction issues: {collected['issues']}")
    expected = catalog["cases"]
    require(len(collected["cases"]) == len(expected), "not every catalog case was recorded")
    for spec, case in zip(expected, collected["cases"]):
        require((case["case_id"], case["case_version"], case["attempt"]) ==
                (spec["id"], spec["version"], 1), f"wrong catalog identity: {case['case_id']}")
        require(case["outcome"] == "normal" and case["complete"] and case["lifecycle_complete"],
                f"catalog case did not complete normally: {spec['id']}")
        require([row["fields"]["checkpoint_id"] for row in case["checkpoints"]] == spec["checkpoints"],
                f"catalog checkpoints differ: {spec['id']}")
        require(any(row["fields"].get("event") == "input.pad" for row in case["events"]),
                f"synthetic pad sample missing: {spec['id']}")
        require(any(row["fields"].get("event") == "guest.frame" for row in case["events"]),
                f"synthetic time progression missing: {spec['id']}")


def pipeline(binary: Path, catalog_path: Path, root: Path) -> dict:
    catalog = load_case_catalog(catalog_path)
    require(tuple(spec["id"] for spec in catalog["cases"]) == EXPECTED_CASE_IDS,
            "final catalog case IDs or order differ")
    native = catalog["cases"][2]
    require(native["required_probes"] == [{"entry": SCALE_ENTRY, "min_calls": 1}],
            "NATIVE-01 must require one real scale helper call")
    catalog_hash = canonical_hash(catalog)
    hash_result = subprocess.run(
        [str(binary), "--catalog", str(catalog_path)],
        capture_output=True, text=True, timeout=10, check=False,
    )
    require(hash_result.returncode == 0, f"C++ catalog rejected final JSON: {hash_result.stderr}")
    native_hash = json.loads(hash_result.stdout)
    require(native_hash == {"sha256": catalog_hash, "case_count": len(catalog["cases"])},
            "Python and C++ canonical catalog hashes disagree")

    baseline = run_fixture(binary, catalog_path, catalog_hash, root, "baseline", "normal")
    candidate = run_fixture(binary, catalog_path, catalog_hash, root, "candidate", "normal")
    check_complete_catalog(baseline, catalog)
    check_complete_catalog(candidate, catalog)
    normal_report = compare_runs(baseline, candidate, catalog)
    require(not normal_report["compatibility_issues"],
            f"paired synthetic packages are incompatible: {normal_report['compatibility_issues']}")
    require(all(not issues for issues in normal_report["case_lifecycle_issues"].values()),
            "normal catalog lifecycle has reconstruction issues")
    require(len(normal_report["cases"]) == len(catalog["cases"]), "comparison lost catalog cases")
    require(normal_report["outcome"] == "not_covered", "synthetic native coverage must remain missing")
    require({case["case_id"]: case["outcome"] for case in normal_report["cases"]} == {
                "REC-01": "observational_match", "REC-02": "observational_match",
                "NATIVE-01": "not_covered", "REC-03": "observational_match"},
            "synthetic case comparisons changed unexpectedly")
    for case in normal_report["cases"]:
        if "character_loaded" in next(spec for spec in catalog["cases"]
                                      if spec["id"] == case["case_id"])["required_state_fields"]:
            require(all(sample["values"]["character_loaded"] is False
                        for samples in case["checkpoint_states"].values()
                        for sample in samples.values()),
                    "synthetic zeroed memory was mistaken for a loaded village")
    native_result = next(case for case in normal_report["cases"] if case["case_id"] == "NATIVE-01")
    require(native_result["outcome"] == "not_covered", "NATIVE-01 gained false coverage")
    require(native_result["reference_verification"]["outcome"] == "not_covered",
            "NATIVE-01 gained false reference verification")
    require(len(native_result["probes"]) == 1 and
            all(native_result["probes"][0][role]["status"] == "not_covered" and
                native_result["probes"][0][role]["delta"].get("completed") == 0
                for role in ("baseline", "candidate")),
            "synthetic pad/time samples were mistaken for helper calls")
    require(all(case.get("input_alignment") == "same_observed_stream"
                for case in normal_report["cases"]),
            "paired synthetic pad streams differ")
    write_report(normal_report, root / "normal-report")

    interrupted = run_fixture(binary, catalog_path, catalog_hash, root, "candidate", "interrupt")
    interrupted_records = load_package(interrupted)["records"]
    reconstructed = collect_cases(interrupted_records, catalog)
    interrupted_index = next(index for index, spec in enumerate(catalog["cases"])
                             if len(spec["checkpoints"]) > 1)
    interrupted_spec = catalog["cases"][interrupted_index]
    require(len(reconstructed["cases"]) == interrupted_index + 1,
            "interrupted run recorded unexpected later cases")
    interrupted_case = reconstructed["cases"][-1]
    require(interrupted_case["case_id"] == interrupted_spec["id"] and
            interrupted_case["end_sequence"] is None and
            interrupted_case["outcome"] is None and
            not interrupted_case["lifecycle_complete"] and
            not interrupted_case["complete"],
            "closing midcase fabricated a normal CaseEnd")
    require([row["fields"]["checkpoint_id"] for row in interrupted_case["checkpoints"]] ==
            interrupted_spec["checkpoints"][:1], "interruption did not occur after one checkpoint")
    require(interrupted_case["missing_checkpoints"] == interrupted_spec["checkpoints"][1:],
            "interrupted case did not retain the pending checkpoints")
    require(any(row["fields"].get("event") == "case.interrupted"
                for row in interrupted_case["events"]), "interrupted marker missing")
    require(not any(row["kind"] == 4 and row["fields"].get("case_id") == interrupted_spec["id"]
                    for row in interrupted_records), "interrupted case has a CaseEnd")
    interruption_report = compare_runs(baseline, interrupted, catalog)
    require(interruption_report["outcome"] == "incomplete",
            "midcase closure was not reported incomplete")
    require(not interruption_report["compatibility_issues"],
            "interrupted package lost paired binding")
    write_report(interruption_report, root / "interrupted-report")

    result = {
        "schema": "yakumo-synthetic-initial-case-pipeline-v1",
        "synthetic": True,
        "game_launched": False,
        "catalog_sha256": catalog_hash,
        "case_ids": list(EXPECTED_CASE_IDS),
        "case_versions": {spec["id"]: spec["version"] for spec in catalog["cases"]},
        "normal_comparison_outcome": normal_report["outcome"],
        "case_comparison_outcomes": {case["case_id"]: case["outcome"] for case in normal_report["cases"]},
        "interrupted_case_id": interrupted_spec["id"],
        "interrupted_comparison_outcome": interruption_report["outcome"],
        "native_required_calls_observed": 0,
        "limitations": [
            "No game, SDL window, game resources, or native helper code was run.",
            "Synthetic normal markers do not establish physical input, gameplay, or human acceptance.",
        ],
    }
    (root / "pipeline-summary.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        raise SystemExit("usage: test_initial_case_pipeline.py FIXTURE CATALOG [NEW_OUTPUT_DIRECTORY]")
    fixture = Path(sys.argv[1]).resolve(strict=True)
    case_catalog = Path(sys.argv[2]).resolve(strict=True)
    if len(sys.argv) == 4:
        output = Path(sys.argv[3]).resolve()
        output.mkdir(parents=True)
        print(json.dumps(pipeline(fixture, case_catalog, output)))
    else:
        with tempfile.TemporaryDirectory() as directory:
            print(json.dumps(pipeline(fixture, case_catalog, Path(directory).resolve())))
