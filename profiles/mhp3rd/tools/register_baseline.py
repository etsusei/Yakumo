#!/usr/bin/env python3
"""Register the local, source-frozen B0 reference without running the game.

Only Git-tracked files from the pinned commit enter source.tar. The ISO and ELF
stay in place. The save is copied into a content-addressed, read-only snapshot.
An optional current build is retained as historical evidence, not asserted to
be a binary built from B0; paired, versioned applications belong to PAIR-002.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


BASELINE_ID = "B0"
BASELINE_COMMIT_PREFIX = "4292eb6"
SUPPORTED_ELF_SHA256 = "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c"
NATIVE_SWITCHES = {
    "MHP3RD_NATIVE_ANGLE_STEP": "off",
    "MHP3RD_NATIVE_SCALE_MATRIX": "off",
    "MHP3RD_NATIVE_TRANSLATION_MATRIX": "off",
    "MHP3RD_NATIVE_VECTOR_CONSTRUCT": "off",
    "MHP3RD_NATIVE_MATRIX_COPY": "off",
}
INTENDED_SETTINGS = {"ui.language": "zh-CN", "native_switches": NATIVE_SWITCHES}
SCHEMA = 1
CHUNK = 1024 * 1024


class RegistrationError(ValueError):
    """An input, existing registration, or snapshot failed validation."""


@dataclass(frozen=True)
class RegistrationInputs:
    repo: Path
    commit: str
    iso: Path
    elf: Path
    save_dir: Path
    output: Path
    snapshots_root: Path
    build_dir: Path | None = None
    overlays: Path | None = None
    save_archive: Path | None = None


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def _git(repo: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), *args], capture_output=True, text=True, check=False
    )
    if result.returncode:
        raise RegistrationError(f"Git command failed: {result.stderr.strip()}")
    return result.stdout.strip()


def _resolved_commit(repo: Path, commit: str, required_prefix: str) -> tuple[str, str]:
    if not re.fullmatch(r"[0-9a-fA-F]{7,40}", commit):
        raise RegistrationError("Commit must be a 7- to 40-digit hexadecimal Git revision")
    full = _git(repo, "rev-parse", "--verify", f"{commit}^{{commit}}")
    if not full.startswith(required_prefix):
        raise RegistrationError(f"B0 must resolve to the pinned {required_prefix} commit")
    return full, _git(repo, "rev-parse", "--verify", f"{full}^{{tree}}")


def _inside(path: Path, parent: Path) -> bool:
    return path == parent or parent in path.parents


def _safe_relative(name: str) -> Path:
    rel = Path(name)
    if not name or rel.is_absolute() or any(part in ("", ".", "..") for part in rel.parts):
        raise RegistrationError(f"Unsafe relative path in manifest: {name!r}")
    return rel


def _source_path(path: Path, kind: str) -> Path:
    path = Path(path).expanduser()
    if path.is_symlink():
        raise RegistrationError(f"Input may not be a symlink: {path}")
    path = path.resolve(strict=True)
    mode = path.stat().st_mode
    if kind == "file" and not stat.S_ISREG(mode):
        raise RegistrationError(f"Expected a regular file: {path}")
    if kind == "dir" and not stat.S_ISDIR(mode):
        raise RegistrationError(f"Expected a directory: {path}")
    return path


def _output_path(path: Path) -> Path:
    path = Path(path).expanduser()
    if path.is_symlink():
        raise RegistrationError(f"Output may not be a symlink: {path}")
    return path.resolve(strict=False)


def _require_ignored(repo: Path, path: Path) -> None:
    if not _inside(path, repo):
        raise RegistrationError(f"Local output must be inside the repository: {path}")
    relative = str(path.relative_to(repo))
    result = subprocess.run(
        ["git", "-C", str(repo), "check-ignore", "-q", "--", relative],
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise RegistrationError(f"Local output is not Git-ignored: {path}")


def _file_record(path: Path) -> dict[str, Any]:
    before = path.lstat()
    if not stat.S_ISREG(before.st_mode):
        raise RegistrationError(f"Expected a regular, non-symlink file: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(CHUNK):
            digest.update(chunk)
    after = path.lstat()
    if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) != (
        after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns
    ):
        raise RegistrationError(f"Input changed while it was hashed: {path}")
    return {"size": after.st_size, "sha256": digest.hexdigest()}


def _input_file(path: Path) -> dict[str, Any]:
    return {"path": str(path), **_file_record(path)}


def _scan_tree(root: Path) -> dict[str, Any]:
    if root.is_symlink() or not root.is_dir():
        raise RegistrationError(f"Expected a real directory: {root}")
    dirs: list[str] = []
    files: list[dict[str, Any]] = []

    def visit(directory: Path) -> None:
        with os.scandir(directory) as entries:
            ordered = sorted(entries, key=lambda entry: entry.name)
        for entry in ordered:
            relative = entry.path[len(str(root)) + 1 :]
            _safe_relative(relative)
            mode = entry.stat(follow_symlinks=False).st_mode
            if stat.S_ISDIR(mode):
                dirs.append(Path(relative).as_posix())
                visit(Path(entry.path))
            elif stat.S_ISREG(mode):
                files.append({"path": Path(relative).as_posix(), **_file_record(Path(entry.path))})
            else:
                raise RegistrationError(f"Save/artifact tree contains a symlink or special file: {entry.path}")

    visit(root)
    return {"dirs": sorted(dirs), "files": sorted(files, key=lambda item: item["path"])}


def _tree_id(tree: dict[str, Any]) -> str:
    data = json.dumps(tree, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def _copy_tree(source: Path, destination: Path, tree: dict[str, Any], *, readonly: bool) -> None:
    destination.mkdir()
    for dirname in tree["dirs"]:
        (destination / _safe_relative(dirname)).mkdir(parents=True, exist_ok=True)
    for record in tree["files"]:
        rel = _safe_relative(record["path"])
        target = destination / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / rel, target)
        if _file_record(target) != {key: record[key] for key in ("size", "sha256")}:
            raise RegistrationError(f"Source changed during copy: {source / rel}")
        if readonly:
            target.chmod(0o444)


def _write_json(path: Path, payload: dict[str, Any]) -> None:
    path.write_text(json.dumps(payload, indent=2, sort_keys=True, ensure_ascii=False) + "\n", encoding="utf-8")


def _verify_snapshot(snapshot: Path, *, digest: str | None = None, source: str | None = None) -> dict[str, Any]:
    if snapshot.is_symlink() or not snapshot.is_dir():
        raise RegistrationError(f"Snapshot is missing or is a symlink: {snapshot}")
    if {item.name for item in snapshot.iterdir()} != {"manifest.json", "files"}:
        raise RegistrationError(f"Snapshot contains missing or unexpected entries: {snapshot}")
    manifest = json.loads((snapshot / "manifest.json").read_text(encoding="utf-8"))
    tree = manifest.get("tree")
    if (manifest.get("schema") != SCHEMA or manifest.get("id") != snapshot.name
            or not isinstance(tree, dict) or not isinstance(manifest.get("source_path"), str)):
        raise RegistrationError(f"Snapshot manifest has an invalid identity: {snapshot}")
    if _tree_id(tree) != snapshot.name or (digest is not None and digest != snapshot.name):
        raise RegistrationError(f"Snapshot content identity differs: {snapshot}")
    if source is not None and manifest.get("source_path") != source:
        raise RegistrationError(f"Snapshot was registered from another source: {snapshot}")
    if _scan_tree(snapshot / "files") != tree:
        raise RegistrationError(f"Snapshot files do not match their manifest: {snapshot}")
    for record in tree["files"]:
        if (snapshot / "files" / _safe_relative(record["path"])).stat().st_mode & 0o222:
            raise RegistrationError(f"Snapshot file is writable: {record['path']}")
    return manifest


def _register_snapshot(root: Path, source: Path, tree: dict[str, Any]) -> Path:
    digest = _tree_id(tree)
    target = root / digest
    if target.exists() or target.is_symlink():
        _verify_snapshot(target, digest=digest, source=str(source))
        return target
    stage = Path(tempfile.mkdtemp(prefix=".save-stage-", dir=root))
    try:
        _copy_tree(source, stage / "files", tree, readonly=True)
        _write_json(
            stage / "manifest.json",
            {"schema": SCHEMA, "id": digest, "source_path": str(source), "tree": tree, "created_at_utc": _utc_now()},
        )
        if _scan_tree(stage / "files") != tree or _scan_tree(source) != tree:
            raise RegistrationError("Save changed while its snapshot was prepared")
        if target.exists() or target.is_symlink():
            _verify_snapshot(target, digest=digest, source=str(source))
        else:
            stage.rename(target)
        return target
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def _cmake_entries(cache: Path) -> dict[str, str]:
    selected: dict[str, str] = {}
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(("#", "//")) or "=" not in line or ":" not in line.split("=", 1)[0]:
            continue
        name, value = line.split("=", 1)
        key = name.split(":", 1)[0]
        if key.startswith(("PSPRECOMP_", "MHP3RD_", "SDL", "Vulkan", "FREETYPE", "PNG", "ZLIB")) or key.endswith("_VERSION") or key in {
            "CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER", "CMAKE_CXX_COMPILER_LAUNCHER",
            "CMAKE_GENERATOR", "CMAKE_OSX_ARCHITECTURES",
        }:
            selected[key] = value
    return dict(sorted(selected.items()))


def _command_version(command: list[str]) -> str | None:
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=8, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode:
        return None
    return result.stdout.strip().splitlines()[0] if result.stdout.strip() else None


def _dependency_evidence(build_dir: Path, cache_entries: dict[str, str]) -> dict[str, Any]:
    compiler_files = sorted((build_dir / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    compiler = None
    if compiler_files:
        text = compiler_files[-1].read_text(encoding="utf-8", errors="replace")
        found = re.findall(r'set\((CMAKE_CXX_COMPILER_(?:ID|VERSION))\s+"([^"]+)"\)', text)
        compiler = dict(found)
    packages = {}
    for package in ("sdl3", "vulkan", "libavcodec", "libavutil"):
        packages[package] = _command_version(["pkg-config", "--modversion", package])
    binary = build_dir / "bin/Yakumo"
    linked_libraries = None
    if sys.platform == "darwin":
        try:
            result = subprocess.run(["otool", "-L", str(binary)], capture_output=True, text=True, timeout=10)
            if result.returncode == 0:
                linked_libraries = result.stdout.strip().splitlines()[1:]
        except (OSError, subprocess.TimeoutExpired):
            pass
    return {
        "cmake": _command_version(["cmake", "--version"]),
        "ninja": _command_version(["ninja", "--version"]),
        "ccache": _command_version(["ccache", "--version"]),
        "compiler_from_build_cache": compiler,
        "package_versions_observed_now": packages,
        "linked_libraries_from_binary": linked_libraries,
        "cmake_dependency_paths_and_versions": cache_entries,
    }


def _build_evidence(build_dir: Path | None, overlays: Path | None) -> tuple[dict[str, Any], list[tuple[Path, str]]]:
    evidence: dict[str, Any] = {
        "provenance": "historical build evidence; source-to-binary provenance is unverified",
        "paired_B0_application": "pending PAIR-002",
        "build_dir": str(build_dir) if build_dir else None,
        "overlays_dir": str(overlays) if overlays else None,
        "source_files": [],
        "cmake_cache_entries": {},
        "reported_version_label": None,
    }
    copies: list[tuple[Path, str]] = []
    if build_dir:
        required = ["CMakeCache.txt", "bin/Yakumo"]
        optional = ["build.ninja", "profiles/mhp3rd/generated_version/yakumo_version.hpp"]
        for rel in required + optional:
            source = build_dir / rel
            if not source.exists():
                if rel in required:
                    raise RegistrationError(f"Build evidence is missing {source}")
                continue
            _source_path(source, "file")
            evidence["source_files"].append({"path": rel, **_file_record(source)})
            copies.append((source, f"historical-build/{rel}"))
        evidence["cmake_cache_entries"] = _cmake_entries(build_dir / "CMakeCache.txt")
        evidence["dependencies"] = _dependency_evidence(build_dir, evidence["cmake_cache_entries"])
        compiler_files = sorted((build_dir / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
        if compiler_files:
            compiler_file = compiler_files[-1]
            rel = compiler_file.relative_to(build_dir).as_posix()
            evidence["source_files"].append({"path": rel, **_file_record(compiler_file)})
            copies.append((compiler_file, f"historical-build/{rel}"))
        header = build_dir / optional[-1]
        if header.is_file():
            match = re.search(r'kYakumoVersion\s*=\s*"([^"]+)"', header.read_text(encoding="utf-8"))
            if match:
                evidence["reported_version_label"] = match.group(1)
    if overlays:
        tree = _scan_tree(overlays)
        evidence["overlays_tree"] = tree
        evidence["overlays_sha256"] = _tree_id(tree)
        for record in tree["files"]:
            copies.append((overlays / _safe_relative(record["path"]), f"historical-build/overlays/{record['path']}"))
    return evidence, copies


def _prepare(inputs: RegistrationInputs, expected_elf_sha256: str, required_commit_prefix: str) -> tuple[dict[str, Any], list[tuple[Path, str]], Path, Path]:
    repo = _source_path(inputs.repo, "dir")
    if _git(repo, "rev-parse", "--show-toplevel") != str(repo):
        raise RegistrationError("--repo must name the Git worktree root")
    output = _output_path(inputs.output)
    snapshots_root = _output_path(inputs.snapshots_root)
    if output.name != BASELINE_ID or _inside(output, snapshots_root) or _inside(snapshots_root, output):
        raise RegistrationError("B0 output and snapshot root must be separate directories")
    _require_ignored(repo, output)
    _require_ignored(repo, snapshots_root)
    iso = _source_path(inputs.iso, "file")
    elf = _source_path(inputs.elf, "file")
    save_dir = _source_path(inputs.save_dir, "dir")
    build_dir = _source_path(inputs.build_dir, "dir") if inputs.build_dir else None
    overlays = _source_path(inputs.overlays, "dir") if inputs.overlays else None
    save_archive = _source_path(inputs.save_archive, "file") if inputs.save_archive else None
    for source in (iso, elf, save_dir, build_dir, overlays, save_archive):
        if source and any(_inside(source, target) or _inside(target, source) for target in (output, snapshots_root)):
            raise RegistrationError(f"Input and output paths overlap: {source}")
    full_commit, tree = _resolved_commit(repo, inputs.commit, required_commit_prefix)
    elf_record = _input_file(elf)
    if elf_record["sha256"] != expected_elf_sha256:
        raise RegistrationError("ELF does not match the supported executable SHA-256")
    save_tree = _scan_tree(save_dir)
    save_id = _tree_id(save_tree)
    build_evidence, copies = _build_evidence(build_dir, overlays)
    identity = {
        "baseline_id": BASELINE_ID,
        "source": {"commit": full_commit, "tree": tree, "archive_kind": "git archive of tracked files"},
        "inputs": {
            "iso": _input_file(iso),
            "iso_validation": "whole-image SHA-256 only; full supported-disc validation belongs to RES-002/003",
            "elf": elf_record,
            "supported_elf_sha256": expected_elf_sha256,
            "save_archive": _input_file(save_archive) if save_archive else None,
        },
        "starting_save": {
            "source_path": str(save_dir),
            "snapshot_id": save_id,
            "snapshot_path": str(snapshots_root / save_id),
            "tree": save_tree,
        },
        "historical_build": build_evidence,
        "intended_settings": INTENDED_SETTINGS,
    }
    if save_archive:
        copies.append((save_archive, "inputs/save-archive.bin"))
    return identity, copies, output, snapshots_root


def _verify_baseline(output: Path, identity: dict[str, Any]) -> dict[str, Any]:
    if output.is_symlink() or not output.is_dir():
        raise RegistrationError(f"B0 output is missing or is a symlink: {output}")
    manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("schema") != SCHEMA or manifest.get("identity") != identity:
        raise RegistrationError("Existing B0 registration has different identities")
    inventory = manifest.get("archived_files", [])
    expected = {item["path"]: {"size": item["size"], "sha256": item["sha256"]} for item in inventory}
    if len(expected) != len(inventory) or "source.tar" not in expected:
        raise RegistrationError("B0 artifact inventory is invalid")
    actual = _scan_tree(output)
    actual_files = {item["path"]: {"size": item["size"], "sha256": item["sha256"]} for item in actual["files"]}
    actual_files.pop("manifest.json", None)
    if actual_files != expected:
        raise RegistrationError("Archived B0 source or artifact hash differs")
    snapshot = Path(identity["starting_save"]["snapshot_path"])
    _verify_snapshot(
        snapshot,
        digest=identity["starting_save"]["snapshot_id"],
        source=identity["starting_save"]["source_path"],
    )
    if _file_record(snapshot / "manifest.json")["sha256"] != manifest.get("snapshot_manifest_sha256"):
        raise RegistrationError("Starting-save snapshot manifest hash differs")
    return manifest


def register_baseline(
    inputs: RegistrationInputs,
    *,
    expected_elf_sha256: str = SUPPORTED_ELF_SHA256,
    required_commit_prefix: str = BASELINE_COMMIT_PREFIX,
) -> dict[str, Any]:
    """Create B0 once, or verify every archived output before reusing it.

    The two override arguments exist for synthetic fixture tests. The CLI
    always applies the pinned release commit and supported ELF hash.
    """
    identity, copies, output, snapshots_root = _prepare(inputs, expected_elf_sha256, required_commit_prefix)
    if output.exists() or output.is_symlink():
        return _verify_baseline(output, identity)
    output.parent.mkdir(parents=True, exist_ok=True)
    snapshots_root.mkdir(parents=True, exist_ok=True)
    save_dir = Path(identity["starting_save"]["source_path"])
    snapshot = _register_snapshot(snapshots_root, save_dir, identity["starting_save"]["tree"])
    expected_copies = {
        f"historical-build/{item['path']}": {"size": item["size"], "sha256": item["sha256"]}
        for item in identity["historical_build"]["source_files"]
    }
    for item in identity["historical_build"].get("overlays_tree", {}).get("files", []):
        expected_copies[f"historical-build/overlays/{item['path']}"] = {
            "size": item["size"], "sha256": item["sha256"]
        }
    if identity["inputs"]["save_archive"]:
        expected_copies["inputs/save-archive.bin"] = {
            key: identity["inputs"]["save_archive"][key] for key in ("size", "sha256")
        }
    stage = Path(tempfile.mkdtemp(prefix=".B0-stage-", dir=output.parent))
    try:
        source_tar = stage / "source.tar"
        with source_tar.open("wb") as stream:
            result = subprocess.run(
                ["git", "-C", str(Path(inputs.repo).resolve()), "archive", "--format=tar", identity["source"]["commit"]],
                stdout=stream,
                stderr=subprocess.PIPE,
                check=False,
            )
        if result.returncode:
            raise RegistrationError(f"Could not archive B0 source: {result.stderr.decode(errors='replace').strip()}")
        archived = [{"path": "source.tar", **_file_record(source_tar)}]
        for source, relative in copies:
            rel = _safe_relative(relative)
            target = stage / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
            copied = _file_record(target)
            if copied != expected_copies[relative] or copied != _file_record(source):
                raise RegistrationError(f"Artifact changed during copy: {source}")
            archived.append({"path": rel.as_posix(), **copied})
        if _scan_tree(save_dir) != identity["starting_save"]["tree"]:
            raise RegistrationError("Original save changed during registration")
        archived.sort(key=lambda item: item["path"])
        manifest = {
            "schema": SCHEMA,
            "registered_at_utc": _utc_now(),
            "identity": identity,
            "archived_files": archived,
            "snapshot_manifest_sha256": _file_record(snapshot / "manifest.json")["sha256"],
            "registration_host": {"platform": platform.platform(), "python": platform.python_version()},
            "scope": "source and inputs registered; no game run or correctly versioned B0 application asserted",
        }
        _write_json(stage / "manifest.json", manifest)
        if output.exists() or output.is_symlink():
            return _verify_baseline(output, identity)
        stage.rename(output)
        return _verify_baseline(output, identity)
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def copy_snapshot_to_new_run(snapshot_dir: Path, run_target: Path) -> Path:
    """Verify a snapshot and copy it only into a nonexistent run directory."""
    snapshot_dir = _source_path(snapshot_dir, "dir")
    run_target = _output_path(run_target)
    if run_target.exists() or run_target.is_symlink():
        raise RegistrationError(f"Run target already exists: {run_target}")
    manifest = _verify_snapshot(snapshot_dir)
    source = Path(manifest["source_path"])
    if _inside(run_target, snapshot_dir.parent) or _inside(run_target, source):
        raise RegistrationError("Run target overlaps the snapshot or original save")
    run_target.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".run-stage-", dir=run_target.parent))
    try:
        prepared = stage / "run"
        _copy_tree(snapshot_dir / "files", prepared, manifest["tree"], readonly=False)
        if _scan_tree(prepared) != manifest["tree"]:
            raise RegistrationError("Snapshot copy failed verification")
        if run_target.exists() or run_target.is_symlink():
            raise RegistrationError(f"Run target already exists: {run_target}")
        prepared.rename(run_target)
        return run_target
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", required=True, type=Path, help="Git worktree root")
    parser.add_argument("--commit", required=True, help="Pinned B0 revision (4292eb6 or its full hash)")
    parser.add_argument("--iso", required=True, type=Path, help="Original ISO, hashed in place; never copied")
    parser.add_argument("--elf", required=True, type=Path, help="Decrypted supported EBOOT.ELF, hashed in place")
    parser.add_argument("--save-dir", required=True, type=Path, help="Original starting-save directory; copied, never changed")
    parser.add_argument("--output", required=True, type=Path, help="Ignored out/testing/baselines/B0 directory")
    parser.add_argument("--snapshots-root", required=True, type=Path, help="Ignored analysis/testing/start-saves directory")
    parser.add_argument("--build-dir", type=Path, help="Optional current build; archived as historical, unproven evidence")
    parser.add_argument("--overlays", type=Path, help="Optional historical overlay directory to hash and archive")
    parser.add_argument("--save-archive", type=Path, help="Optional original save archive to hash and archive")
    args = parser.parse_args(argv)
    try:
        manifest = register_baseline(RegistrationInputs(**vars(args)))
    except (OSError, subprocess.SubprocessError, RegistrationError, json.JSONDecodeError) as error:
        parser.exit(1, f"Baseline registration failed: {error}\n")
    print(json.dumps({"manifest": str(Path(args.output).resolve() / "manifest.json"), "source_commit": manifest["identity"]["source"]["commit"], "save_snapshot": manifest["identity"]["starting_save"]["snapshot_path"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
