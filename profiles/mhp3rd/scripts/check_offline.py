#!/usr/bin/env python3
"""Run the bounded OFF-006 gate without starting the game.

The report and command logs stay under an ignored local out/ directory. Each
invocation gets a new run directory, while report.json names the latest run.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import signal
import stat
import subprocess
import sys
import tempfile
import time
from typing import Any
import uuid


REPO = Path(__file__).resolve().parents[3]
BUILD_TARGETS = (
    "psprecomp_tests",
    "mhp3rd_savedata_tests",
    "mhp3rd_native_angle_tests",
    "mhp3rd_native_scale_tests",
    "mhp3rd_native_translation_matrix_tests",
    "mhp3rd_native_vector_construct_tests",
    "mhp3rd_native_matrix_copy_tests",
    "mhp3rd_iso_image_tests",
    "mhp3rd_psmf_demuxer_tests",
)
PYTHON_TESTS = ("mhp3rd_baseline_registration_tests", "mhp3rd_resource_preparation_tests")
CTEST_NAMES = BUILD_TARGETS + PYTHON_TESTS
NATIVE_TESTS = BUILD_TARGETS[2:7]
CTEST_PATTERN = "^(" + "|".join(CTEST_NAMES) + ")$"
CASE_LABELS = {
    "mhp3rd_native_angle_tests": ("Differential cases", "Native angle failures"),
    "mhp3rd_native_scale_tests": ("Scale differential cases", "Native scale failures"),
    "mhp3rd_native_translation_matrix_tests": ("Translation differential cases", "Native translation failures"),
    "mhp3rd_native_vector_construct_tests": ("Vector constructor differential cases", "Native vector constructor failures"),
    "mhp3rd_native_matrix_copy_tests": ("Matrix-copy local ELF differential cases", "Native matrix-copy failures"),
}
CERTIFIED_MINIMUMS = {
    "mhp3rd_native_angle_tests": {"cases": 806432},
    "mhp3rd_native_scale_tests": {"cases": 100512, "prefix_fallbacks": 10000},
    "mhp3rd_native_translation_matrix_tests": {"cases": 100512, "prefix_fallbacks": 10000},
    "mhp3rd_native_vector_construct_tests": {"cases": 100512},
    "mhp3rd_native_matrix_copy_tests": {"cases": 100348},
}
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
CHUNK = 1024 * 1024
BUILD_TIMEOUT_SECONDS = 3600
CTEST_TIMEOUT_SECONDS = 900
NATIVE_TIMEOUT_SECONDS = 900


class GateError(Exception):
    def __init__(self, message: str, *, status: str = "incomplete") -> None:
        super().__init__(message)
        self.status = status


def _now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def _absolute(path: Path) -> Path:
    lexical = Path(os.path.abspath(Path(path).expanduser()))
    # macOS commonly has /var -> /private/var. Canonicalize ancestors while
    # keeping the final component visible to the symlink check below.
    return Path(os.path.realpath(lexical.parent)) / lexical.name


def _inside(path: Path, parent: Path) -> bool:
    return path == parent or parent in path.parents


def _reject_symlink_components(path: Path) -> None:
    for part in (path, *path.parents):
        if part.is_symlink():
            raise GateError(f"Symlink path is not allowed: {part}")


def _safe_output(repo: Path, output: Path, build: Path, baseline: Path, elf: Path) -> None:
    _reject_symlink_components(output)
    if not _inside(output, repo / "out") or output == repo / "out":
        raise GateError("Output must be a directory below the repository's ignored out/ directory")
    for other in (build, baseline, elf):
        if _inside(output, other) or _inside(other, output):
            raise GateError(f"Output overlaps a source or input path: {other}")
    check = subprocess.run(
        ["git", "-C", str(repo), "check-ignore", "--no-index", "-q", "--", str(output)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10, check=False,
    )
    if check.returncode:
        raise GateError("Output directory is not Git-ignored")
    if output.exists() and not output.is_dir():
        raise GateError("Output path exists and is not a directory")
    latest = output / "report.json"
    if latest.is_symlink() or (latest.exists() and not latest.is_file()):
        raise GateError("Existing latest report is not a regular file")
    if latest.exists():
        try:
            existing = json.loads(latest.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise GateError("Existing latest report is not a valid OFF-006 report") from exc
        if (not isinstance(existing, dict) or existing.get("schema") != 1
                or existing.get("gate") != "OFF-006"):
            raise GateError("Existing latest report is not owned by the OFF-006 runner")


def _file_record(path: Path) -> dict[str, Any]:
    _reject_symlink_components(path)
    try:
        before = path.lstat()
    except OSError as exc:
        raise GateError(f"Missing input file: {path}") from exc
    if not stat.S_ISREG(before.st_mode):
        raise GateError(f"Input is not a regular file: {path}")
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while data := stream.read(CHUNK):
                digest.update(data)
        after = path.lstat()
    except OSError as exc:
        raise GateError(f"Could not hash input: {path}: {exc}") from exc
    identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
    if identity(before) != identity(after):
        raise GateError(f"Input changed while hashing: {path}", status="failed")
    return {"size": after.st_size, "sha256": digest.hexdigest()}


def _safe_relative(name: str) -> Path:
    path = Path(name)
    if (not name or path.is_absolute() or "\\" in name
            or any(part in ("", ".", "..") for part in name.split("/"))):
        raise GateError(f"Unsafe save path in baseline manifest: {name!r}")
    return path


def _scan_tree(root: Path) -> dict[str, Any]:
    _reject_symlink_components(root)
    if not root.is_dir():
        raise GateError(f"Missing source save directory: {root}")
    dirs: list[str] = []
    files: list[dict[str, Any]] = []

    def visit(directory: Path) -> None:
        try:
            with os.scandir(directory) as entries:
                ordered = sorted(entries, key=lambda entry: entry.name)
        except OSError as exc:
            raise GateError(f"Could not read source save directory: {directory}: {exc}") from exc
        for entry in ordered:
            path = Path(entry.path)
            rel = path.relative_to(root).as_posix()
            _safe_relative(rel)
            mode = entry.stat(follow_symlinks=False).st_mode
            if stat.S_ISDIR(mode):
                dirs.append(rel)
                visit(path)
            elif stat.S_ISREG(mode):
                files.append({"path": rel, **_file_record(path)})
            else:
                raise GateError(f"Source save contains a symlink or special file: {path}")

    visit(root)
    return {"dirs": sorted(dirs), "files": sorted(files, key=lambda entry: entry["path"])}


def _manifest_input(record: Any, name: str) -> tuple[Path, dict[str, Any]]:
    if not isinstance(record, dict) or not isinstance(record.get("path"), str):
        raise GateError(f"B0 {name} fingerprint is missing")
    expected = {"size": record.get("size"), "sha256": record.get("sha256")}
    if (not isinstance(expected["size"], int) or expected["size"] < 0
            or not isinstance(expected["sha256"], str) or not SHA256.fullmatch(expected["sha256"])):
        raise GateError(f"B0 {name} fingerprint is invalid")
    return _absolute(Path(record["path"])), expected


def _load_baseline(path: Path) -> dict[str, Any]:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
        identity = manifest["identity"]
        inputs = identity["inputs"]
        saved = identity["starting_save"]
        if manifest["schema"] != 1 or identity["baseline_id"] != "B0":
            raise GateError("Expected a schema 1 B0 registration")
        for key in ("iso", "elf"):
            _manifest_input(inputs[key], key)
        if inputs.get("save_archive") is not None:
            _manifest_input(inputs["save_archive"], "save_archive")
        _, elf_record = _manifest_input(inputs["elf"], "elf")
        if inputs["supported_elf_sha256"] != elf_record["sha256"]:
            raise GateError("B0 supported ELF fingerprint differs from its registered ELF")
        if not isinstance(saved["source_path"], str):
            raise GateError("B0 starting-save source path is invalid")
        tree = saved["tree"]
        if not isinstance(tree["dirs"], list) or not isinstance(tree["files"], list):
            raise GateError("B0 starting-save tree is invalid")
        for dirname in tree["dirs"]:
            _safe_relative(dirname)
        for record in tree["files"]:
            _safe_relative(record["path"])
            _manifest_input({"path": record["path"], "size": record["size"], "sha256": record["sha256"]}, "save file")
        return manifest
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError) as exc:
        raise GateError(f"Cannot read valid B0 registration: {exc}") from exc


def _input_paths(manifest: dict[str, Any], supplied_elf: Path) -> list[Path]:
    identity = manifest["identity"]
    paths = [_manifest_input(identity["inputs"][name], name)[0] for name in ("iso", "elf")]
    if identity["inputs"].get("save_archive") is not None:
        paths.append(_manifest_input(identity["inputs"]["save_archive"], "save_archive")[0])
    paths.extend((_absolute(Path(identity["starting_save"]["source_path"])), supplied_elf))
    return paths


def _verify_inputs(manifest: dict[str, Any], supplied_elf: Path, *, strict: bool = True) -> dict[str, Any]:
    identity = manifest["identity"]
    records: dict[str, Any] = {}
    mismatches: list[str] = []
    for name in ("iso", "elf"):
        path, expected = _manifest_input(identity["inputs"][name], name)
        actual = _file_record(path)
        records[name] = {"path": str(path), **actual, "matches_B0": actual == expected}
        if actual != expected:
            mismatches.append(name)
    if identity["inputs"].get("save_archive") is None:
        records["save_archive"] = {"registered": False, "reason": "No save archive was supplied at B0 registration"}
    else:
        path, expected = _manifest_input(identity["inputs"]["save_archive"], "save_archive")
        actual = _file_record(path)
        records["save_archive"] = {"registered": True, "path": str(path), **actual,
                                   "matches_B0": actual == expected}
        if actual != expected:
            mismatches.append("save_archive")
    expected_elf = _manifest_input(identity["inputs"]["elf"], "elf")[1]
    supplied = ({key: records["elf"][key] for key in ("size", "sha256")}
                if supplied_elf == Path(records["elf"]["path"]) else _file_record(supplied_elf))
    records["supplied_elf"] = {"path": str(supplied_elf), **supplied,
                               "matches_B0": supplied == expected_elf}
    if supplied != expected_elf:
        mismatches.append("supplied_elf")
    source = _absolute(Path(identity["starting_save"]["source_path"]))
    tree = _scan_tree(source)
    records["starting_save"] = {"source_path": str(source), "tree": tree,
                                "matches_B0": tree == identity["starting_save"]["tree"]}
    if not records["starting_save"]["matches_B0"]:
        mismatches.append("starting_save")
    records["mismatches"] = mismatches
    if strict and mismatches:
        raise GateError(f"Original inputs do not match B0: {', '.join(mismatches)}", status="failed")
    return records


def _atomic_json(path: Path, data: dict[str, Any]) -> None:
    if path.is_symlink():
        raise GateError(f"Refusing to replace symlink: {path}")
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent,
                                     prefix=".report-", suffix=".tmp", delete=False) as stream:
        temporary = Path(stream.name)
        try:
            json.dump(data, stream, indent=2, sort_keys=True, ensure_ascii=True)
            stream.write("\n")
        except BaseException:
            temporary.unlink(missing_ok=True)
            raise
    os.replace(temporary, path)


def _run_command(report: dict[str, Any], run_dir: Path, name: str,
                 argv: list[str], timeout: float) -> str:
    log = run_dir / f"{len(report['commands']) + 1:02d}-{name}.log"
    record: dict[str, Any] = {"name": name, "argv": argv, "timeout_seconds": timeout,
                              "started_at_utc": _now(), "log": str(log),
                              "exit_status": None, "timed_out": False}
    report["commands"].append(record)
    began = time.monotonic()
    try:
        with log.open("xb") as stream:
            process = subprocess.Popen(argv, stdout=stream, stderr=subprocess.STDOUT,
                                       start_new_session=(os.name == "posix"))
            try:
                record["exit_status"] = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                record["timed_out"] = True
                if os.name == "posix":
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                else:
                    process.kill()
                process.wait()
    except OSError as exc:
        record["launch_error"] = str(exc)
        raise GateError(f"Could not execute {name}: {exc}") from exc
    finally:
        record["finished_at_utc"] = _now()
        record["duration_seconds"] = round(time.monotonic() - began, 3)
        if log.exists():
            record["log_sha256"] = _file_record(log)["sha256"]
    if record["timed_out"]:
        raise GateError(f"{name} timed out after {timeout} seconds", status="failed")
    if record["exit_status"] != 0:
        raise GateError(f"{name} exited with status {record['exit_status']}", status="failed")
    return log.read_text(encoding="utf-8", errors="replace")


def _source_evidence(report: dict[str, Any], run_dir: Path, repo: Path) -> None:
    commit = _run_command(report, run_dir, "git-head",
                          ["git", "-C", str(repo), "rev-parse", "HEAD"], 10).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise GateError("Git HEAD is not a full commit hash")
    status = _run_command(report, run_dir, "git-status",
                          ["git", "-C", str(repo), "status", "--porcelain", "--untracked-files=normal"], 10)
    _run_command(report, run_dir, "git-diff",
                 ["git", "-C", str(repo), "diff", "HEAD", "--binary", "--"], 30)
    diff_sha256 = report["commands"][-1]["log_sha256"]
    _run_command(report, run_dir, "git-source-files",
                 ["git", "-C", str(repo), "ls-files", "--cached", "--others", "--exclude-standard", "-z"], 30)
    raw = Path(report["commands"][-1]["log"]).read_bytes()
    if raw and not raw.endswith(b"\0"):
        raise GateError("Git source listing is incomplete")
    files: list[dict[str, Any]] = []
    for raw_name in sorted(set(raw.split(b"\0")) - {b""}):
        relative = _safe_relative(os.fsdecode(raw_name))
        path = repo / relative
        if not _inside(path, repo):
            raise GateError(f"Git listed a source file outside the repository: {relative}")
        if path.is_symlink():
            target = os.readlink(path).encode("utf-8", errors="surrogateescape")
            files.append({"path": relative.as_posix(), "kind": "symlink",
                          "size": len(target), "sha256": _digest_bytes(target)})
        elif path.is_file():
            files.append({"path": relative.as_posix(), "kind": "file", **_file_record(path)})
        elif not path.exists():
            files.append({"path": relative.as_posix(), "kind": "deleted"})
        else:
            raise GateError(f"Git source path is not a regular file or symlink: {relative}")
    content = json.dumps(files, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
    report["source"] = {"commit": commit, "dirty": bool(status.strip()),
                        "status_porcelain": status.splitlines(),
                        "status_sha256": _digest_bytes(status.encode("utf-8")),
                        "diff_head_binary_sha256": diff_sha256,
                        "content_identity_sha256": _digest_bytes(content),
                        "files": files}


def _digest_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _build_identity(repo: Path, build: Path) -> dict[str, Any]:
    path = build / "CMakeCache.txt"
    record = _file_record(path)
    entries: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"^(CMAKE_HOME_DIRECTORY|PSPRECOMP_PROFILE|PSPRECOMP_BUILD_TESTS|CMAKE_BUILD_TYPE):[^=]*=(.*)$", line)
        if match:
            entries[match[1]] = match[2]
    if ("CMAKE_HOME_DIRECTORY" not in entries
            or _absolute(Path(entries["CMAKE_HOME_DIRECTORY"])) != repo
            or entries.get("PSPRECOMP_PROFILE") != "mhp3rd"
            or entries.get("PSPRECOMP_BUILD_TESTS", "").upper() not in ("ON", "TRUE", "1")):
        raise GateError("Build directory is not configured for this repository's MHP3 test profile")
    if _file_record(path) != record:
        raise GateError("CMakeCache changed while recording build identity", status="failed")
    return {"cache_path": str(path), **record, "entries": entries}


def _validate_ctest_listing(data: dict[str, Any], repo: Path, build: Path) -> None:
    tests = data.get("tests")
    if not isinstance(tests, list) or len(tests) != len(CTEST_NAMES):
        raise GateError("CTest did not register all 11 required offline tests")
    names = [item.get("name") for item in tests]
    if len(set(names)) != len(CTEST_NAMES) or set(names) != set(CTEST_NAMES):
        raise GateError("CTest required test names are missing or duplicated")
    for item in tests:
        name = item["name"]
        command = item.get("command")
        if not isinstance(command, list) or not command:
            raise GateError(f"CTest has no command for {name}")
        if name in BUILD_TARGETS:
            expected = build / ("psprecomp_tests" if name == "psprecomp_tests" else f"bin/{name}")
            if len(command) != 1 or _absolute(Path(command[0])) != expected:
                raise GateError(f"CTest command for {name} is not its expected offline test binary")
        else:
            pattern = "test_baseline_registration.py" if name == PYTHON_TESTS[0] else "test_resource_preparation.py"
            expected_args = ["-m", "unittest", "discover", "-s", str(repo / "profiles/mhp3rd/tests"),
                             "-p", pattern, "-v"]
            if (not Path(command[0]).name.lower().startswith("python")
                    or command[1:] != expected_args):
                raise GateError(f"CTest command for {name} is not the expected synthetic Python suite")


def _ctest_summary(output: str) -> dict[str, int]:
    match = re.search(r"100% tests passed,\s*(\d+) tests failed out of (\d+)", output)
    if match:
        failed, total = map(int, match.groups())
    else:
        # Newer CTest versions omit the zero-failures clause on success.
        compact = re.search(r"(?m)^100% tests passed out of (\d+)\s*$", output)
        if not compact:
            raise GateError("CTest did not print a complete test summary", status="failed")
        failed, total = 0, int(compact.group(1))
    if failed or total != len(CTEST_NAMES):
        raise GateError(f"CTest completed {total} tests with {failed} failures; 11 passes required", status="failed")
    return {"executed": total, "failed": failed}


def _native_summary(name: str, output: str) -> dict[str, int]:
    cases_label, failures_label = CASE_LABELS[name]
    cases = re.findall(rf"(?m)^{re.escape(cases_label)}: (\d+)(?:, prefix fallbacks: (\d+))?\s*$", output)
    failures = re.findall(rf"(?m)^{re.escape(failures_label)}: (\d+)\s*$", output)
    if len(cases) != 1 or len(failures) != 1:
        raise GateError(f"{name} did not report original-ELF cases and failures", status="failed")
    result = {"cases": int(cases[0][0]), "failures": int(failures[0])}
    if cases[0][1]:
        result["prefix_fallbacks"] = int(cases[0][1])
    if result["failures"]:
        raise GateError(f"{name} reported nonzero failures", status="failed")
    for key, minimum in CERTIFIED_MINIMUMS[name].items():
        if result.get(key, 0) < minimum:
            raise GateError(f"{name} reported {key} below certified minimum {minimum}", status="failed")
    return result


def run_gate(*, repo: Path, build_dir: Path, elf: Path, baseline: Path,
             output_dir: Path, build_timeout: float = BUILD_TIMEOUT_SECONDS,
             ctest_timeout: float = CTEST_TIMEOUT_SECONDS,
             native_timeout: float = NATIVE_TIMEOUT_SECONDS) -> dict[str, Any]:
    """Run one local gate and return its persisted schema-1 report."""
    repo = _absolute(repo)
    build = _absolute(build_dir)
    supplied_elf = _absolute(elf)
    baseline = _absolute(baseline)
    output = _absolute(output_dir)
    _safe_output(repo, output, build, baseline, supplied_elf)
    # Read the identity before creating any output, so a registered input that
    # happens to live below out/ cannot be mistaken for a report location.
    try:
        preflight_manifest = _load_baseline(baseline)
    except GateError:
        preflight_manifest = None
    if preflight_manifest is not None:
        if _inside(build, baseline) or _inside(baseline, build):
            raise GateError(f"Build directory overlaps B0 manifest: {baseline}")
        for source in _input_paths(preflight_manifest, supplied_elf):
            if _inside(output, source) or _inside(source, output):
                raise GateError(f"Output overlaps an original input: {source}")
            if _inside(build, source) or _inside(source, build):
                raise GateError(f"Build directory overlaps an original input: {source}")
    output.mkdir(parents=True, exist_ok=True)
    run_dir = output / (_now().replace(":", "").replace("-", "") + "-" + uuid.uuid4().hex[:12])
    run_dir.mkdir()
    report: dict[str, Any] = {
        "schema": 1, "gate": "OFF-006", "status": "incomplete", "started_at_utc": _now(),
        "finished_at_utc": None, "run_dir": str(run_dir),
        "platform": {"system": platform.system(), "release": platform.release(),
                     "machine": platform.machine(), "python": platform.python_version()},
        "configuration": {"build_dir": str(build), "elf": str(supplied_elf),
                          "baseline": str(baseline), "output_dir": str(output)},
        "source": None, "build_configuration": {"before": None, "after": None},
        "baseline_identity": None, "inputs": {"before": None, "after": None},
        "commands": [], "binary_hashes": {},
        "coverage_tiers": {
            "synthetic_and_unit": {"registered": [], "executed": 0, "failed": None},
            "original_elf_differential": {"certified_minimums": CERTIFIED_MINIMUMS, "results": {}},
            "live_gameplay": {"status": "pending_user_paired_acceptance", "calls_observed": None},
        },
        "limitations": [
            "Offline helper cases do not establish in-game call coverage or full-game correctness.",
            "ISO and PSMF synthetic tests do not establish movie, audio, or display fidelity.",
            "No game boot, menu navigation, user gameplay, or other-platform check is part of this gate.",
        ],
        "errors": [],
    }
    manifest = None
    before_verified = False
    baseline_digest = None
    try:
        _source_evidence(report, run_dir, repo)
        baseline_digest = _file_record(baseline)
        manifest = _load_baseline(baseline)
        paths = _input_paths(manifest, supplied_elf)
        for source in paths:
            if _inside(output, source) or _inside(source, output):
                raise GateError(f"Output overlaps an original input: {source}")
        report["baseline_identity"] = {"baseline_id": "B0", "manifest_sha256": baseline_digest["sha256"],
                                       "registered_source_commit": manifest["identity"]["source"]["commit"]}
        report["inputs"]["before"] = _verify_inputs(manifest, supplied_elf)
        before_verified = True
        if not build.is_dir():
            raise GateError(f"Build directory is missing: {build}")
        report["build_configuration"]["before"] = _build_identity(repo, build)

        build_argv = ["cmake", "--build", str(build), "-j2", "--target", *BUILD_TARGETS]
        _run_command(report, run_dir, "build-offline-tests", build_argv, build_timeout)
        report["build_configuration"]["after"] = _build_identity(repo, build)

        listing = _run_command(report, run_dir, "ctest-list",
                               ["ctest", "--test-dir", str(build), "--show-only=json-v1", "-R", CTEST_PATTERN],
                               ctest_timeout)
        try:
            listed = json.loads(listing)
        except json.JSONDecodeError as exc:
            raise GateError(f"CTest listing is not JSON: {exc}") from exc
        _validate_ctest_listing(listed, repo, build)
        report["coverage_tiers"]["synthetic_and_unit"]["registered"] = sorted(CTEST_NAMES)

        for name in BUILD_TARGETS:
            path = build / ("psprecomp_tests" if name == "psprecomp_tests" else f"bin/{name}")
            report["binary_hashes"][name] = {"path": str(path), **_file_record(path)}

        ctest_output = _run_command(report, run_dir, "ctest-offline",
                                    ["ctest", "--test-dir", str(build), "--output-on-failure",
                                     "--timeout", "120", "--no-tests=error", "-R", CTEST_PATTERN],
                                    ctest_timeout)
        report["coverage_tiers"]["synthetic_and_unit"].update(_ctest_summary(ctest_output))

        for name in NATIVE_TESTS:
            binary = build / "bin" / name
            output_text = _run_command(report, run_dir, name,
                                       [str(binary), str(supplied_elf)], native_timeout)
            report["coverage_tiers"]["original_elf_differential"]["results"][name] = _native_summary(name, output_text)

        report["status"] = "pass"
    except GateError as exc:
        report["status"] = exc.status
        report["errors"].append(str(exc))
    except (OSError, KeyError, TypeError, ValueError) as exc:
        report["status"] = "incomplete"
        report["errors"].append(f"Unexpected gate prerequisite or report error: {exc}")
    finally:
        if report["build_configuration"]["before"] is not None and report["build_configuration"]["after"] is None:
            try:
                report["build_configuration"]["after"] = _build_identity(repo, build)
            except GateError as exc:
                report["status"] = "failed"
                report["errors"].append(str(exc))
        if before_verified and manifest is not None:
            try:
                report["inputs"]["after"] = _verify_inputs(manifest, supplied_elf, strict=False)
                if _file_record(baseline) != baseline_digest:
                    raise GateError("B0 manifest changed during the gate", status="failed")
                if report["inputs"]["after"] != report["inputs"]["before"]:
                    changed = report["inputs"]["after"]["mismatches"]
                    raise GateError(f"Original input identity changed during the gate: {', '.join(changed)}",
                                    status="failed")
            except GateError as exc:
                report["status"] = "failed"
                report["errors"].append(str(exc))
        report["finished_at_utc"] = _now()
        _atomic_json(run_dir / "report.json", report)
        _atomic_json(output / "report.json", report)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("out/resource-validation"))
    parser.add_argument("--elf", required=True, type=Path, help="registered supported local EBOOT.ELF")
    parser.add_argument("--baseline", type=Path, default=Path("out/testing/baselines/B0/manifest.json"))
    parser.add_argument("--output-dir", type=Path, default=Path("out/testing/offline"))
    args = parser.parse_args(argv)
    try:
        report = run_gate(repo=REPO, build_dir=args.build_dir, elf=args.elf,
                          baseline=args.baseline, output_dir=args.output_dir)
    except GateError as exc:
        print(f"Offline gate could not create a safe report: {exc}", file=sys.stderr)
        return 2
    print(f"OFF-006 {report['status']}: {report['run_dir']}/report.json")
    for error in report["errors"]:
        print(error, file=sys.stderr)
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
