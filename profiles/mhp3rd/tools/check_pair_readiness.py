#!/usr/bin/env python3
"""Verify a signed local test pair and prepare independent runs without gameplay.

Only the native launcher's --headless --prepare-only path and the C++ catalog
fixture are executed. The report distinguishes delivery readiness from manual
case coverage; no completed preparation is promoted to a gameplay pass.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import plistlib
import subprocess

import launch_test_run as launch
import run_cases
import run_package


class ReadinessError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ReadinessError(message)


def read_json(path: Path) -> dict:
    value = run_package._parse_json(run_package._read_file(path, run_package.MAX_METADATA_BYTES))
    require(type(value) is dict, "Expected a JSON object")
    return value


def digest(value: dict) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False,
                                    separators=(",", ":")).encode()).hexdigest()


def run(command: list[str], timeout: int = 30) -> subprocess.CompletedProcess:
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(("MHP3RD_", "PSPRECOMP_", "DYLD_", "PYTHON"))}
    result = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=timeout)
    require(result.returncode == 0,
            f"Readiness command failed ({result.returncode}): {result.stderr[:2000]}")
    return result


def check_contract(pair: dict, configs: dict, catalog_hash: str) -> None:
    require(pair.get("schema") == "yakumo-test-pair-v1" and pair.get("baseline_id") == "B0",
            "Unknown pair manifest")
    baseline, candidate = configs["baseline"], configs["candidate"]
    font = pair.get("game_font")
    require(type(font) is dict and bool(font.get("path")) and bool(font.get("sha256")),
            "Pair omits its verified game-text font")
    for role, config in configs.items():
        require(config["settings"].get("text.font") == font["path"],
                "App omits or changes the verified game-text font")
        require(config["role"] == role and config["baseline_id"] == "B0", "Role or baseline differs")
        require(config["cases"]["sha256"] == catalog_hash, "App has a stale case catalog")
        require(config["batch_id"] == pair["batch_id"], "App batch differs from pair manifest")
        require(config["probe_selection"] == "all", "Discovery probes must remain selected")
        expected_mode = "off" if role == "baseline" else "verify"
        require(all(mode == expected_mode for mode in config["native_modes"].values()),
                "First-pack native mode differs")
        for name in ("recorder_revision", "build_config_sha256", "configuration_sha256"):
            require(config[name] == pair[name], "Pair metadata differs: " + name)
    require(baseline["baseline_commit"] == "4292eb66ee66eab37c327575382d071addcf6249",
            "Baseline reference is not the full pinned B0 identity")
    require(baseline["source_commit"] == baseline["baseline_commit"], "Baseline gameplay identity differs")
    for key in ("baseline_commit", "settings", "work_root"):
        require(baseline[key] == candidate[key], "Paired prerequisites differ: " + key)
    for key in ("iso", "elf"):
        require(baseline[key]["path"] == candidate[key]["path"] and
                baseline[key]["sha256"] == candidate[key]["sha256"], "Original input differs: " + key)
    for key in ("starting_save", "overlays"):
        require(baseline[key]["tree_id"] == candidate[key]["tree_id"], "Input tree differs: " + key)
    require(baseline["overlays"]["tree_id"] == pair["overlays_tree_id"], "Pair overlay identity differs")
    require(baseline["binary"]["sha256"] != candidate["binary"]["sha256"],
            "Baseline and candidate are the same binary")


def check_readiness(pair_dir: Path, catalog_path: Path, fixture: Path) -> dict:
    pair_dir = pair_dir.resolve(strict=True)
    fixture = fixture.resolve(strict=True)
    catalog_path = catalog_path.resolve(strict=True)
    catalog = run_cases.load_case_catalog(catalog_path)
    expected_hash = digest(catalog)
    require([case["id"] for case in catalog["cases"]] == ["REC-01", "REC-02", "NATIVE-01", "REC-03"],
            "The first user pack must contain its four ordered cases")
    require(all(case["human_acceptance"] for case in catalog["cases"]),
            "User observations remain required for every initial case")
    pair = read_json(pair_dir / "pair-manifest.json")
    font = pair.get("game_font", {})
    font_path = launch._path(font.get("path"), "game_font.path", kind="file")
    require(launch._regular_hash(font_path)[1] == font.get("sha256"), "Game-text font content changed")
    configs = {}
    applications = {}
    for role in ("baseline", "candidate"):
        app = pair_dir / ("Yakumo " + role.title() + ".app")
        require(str(app) == pair["applications"][role], "Pair application location differs")
        plist = plistlib.loads((app / "Contents/Info.plist").read_bytes())
        require(plist["CFBundleExecutable"] == "YakumoTestLauncher", "App has the wrong entry point")
        require(plist["CFBundleIdentifier"].endswith(".test." + role), "App role identifier differs")
        run(["codesign", "--verify", "--deep", "--strict", str(app)], 60)
        config = launch._load_config(app / "Contents/Resources/testing/launch-config.json")
        require(config["binary"]["path"] == app / "Contents/MacOS/YakumoGame", "Game path escapes its app")
        require(config["cases"]["path"] == app / "Contents/Resources/testing/cases.json", "Catalog path escapes its app")
        require(run_cases.load_case_catalog(config["cases"]["path"]) == catalog, "Catalog contents differ")
        parsed = json.loads(run([str(fixture), "--catalog", str(config["cases"]["path"])]).stdout)
        require(parsed == {"sha256": expected_hash, "case_count": len(catalog["cases"])},
                "Compiled C++ and Python catalog interpretations differ")
        configs[role] = config
        applications[role] = app
    check_contract(pair, configs, expected_hash)

    outcomes = {}
    bases = []
    for role, config in configs.items():
        runs = config["work_root"] / "runs"
        before = set(runs.iterdir()) if runs.exists() else set()
        app = applications[role]
        prepared = run([str(app / "Contents/MacOS/YakumoTestLauncher"),
                        "--headless", "--prepare-only"], 120)
        added = set(runs.iterdir()) - before
        require(len(added) == 1, "Expected one fresh preparation run; another run may be active")
        directory = added.pop()
        result = read_json(directory / "result.json")
        require(result["role"] == role and result["status"] == "prepared" and
                result["stop_reason"] == "prepare_only" and result["source_inputs_unchanged"] is True,
                "Preparation result is incomplete or original inputs changed")
        require(launch.result_directory(directory / "result.json") == directory, "Result location is unbound")
        require(not (directory / "recording").exists() and not (directory / "game.stdout.log").exists(),
                "Preparation unexpectedly started recording or a game subprocess")
        context = read_json(directory / "context.json")
        bases.append(run_package.prerequisite_basis_sha256(context, "B0", config["baseline_commit"]))
        save = directory / "data/ms0/PSP/SAVEDATA/ULJM05800"
        launch._verify_tree(save, config["starting_save"]["tree"], "prepared save")
        for item in config["starting_save"]["tree"]["files"]:
            original = (config["starting_save"]["path"] / item["path"]).stat()
            copy = (save / item["path"]).stat()
            require((original.st_dev, original.st_ino) != (copy.st_dev, copy.st_ino),
                    "Writable save aliases its original")
        outcomes[role] = {"application": str(app), "run_directory": str(directory),
                          "result": result, "launcher_stdout": prepared.stdout,
                          "launcher_stderr": prepared.stderr, "save_directory": str(save)}
    require(bases[0] == bases[1], "Role preparation produced different case prerequisites")
    require(outcomes["baseline"]["run_directory"] != outcomes["candidate"]["run_directory"],
            "Roles share a writable run directory")
    for item in configs["baseline"]["starting_save"]["tree"]["files"]:
        a = (Path(outcomes["baseline"]["save_directory"]) / item["path"]).stat()
        b = (Path(outcomes["candidate"]["save_directory"]) / item["path"]).stat()
        require((a.st_dev, a.st_ino) != (b.st_dev, b.st_ino), "Roles share a writable save file")
    return {"schema": "yakumo-first-pack-readiness-v1", "status": "ready_for_user_test",
            "recorded_at": datetime.now(timezone.utc).isoformat(), "full_game_launched": False,
            "case_catalog_sha256": expected_hash, "prerequisite_basis_sha256": bases[0],
            "pair_manifest_sha256": launch._regular_hash(pair_dir / "pair-manifest.json")[1],
            "roles": outcomes,
            "limitations": ["Manual route duration is a planning estimate, not a measured play session.",
                            "Physical controls, dialog appearance and live helper coverage remain unverified.",
                            "Readiness is not gameplay acceptance; no user cases were executed."]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pair-dir", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Output exists; preserve previous readiness evidence")
    report = check_readiness(args.pair_dir, args.catalog, args.fixture)
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print("Both signed apps and the compiled catalog reader agree; independent prepare-only runs passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
