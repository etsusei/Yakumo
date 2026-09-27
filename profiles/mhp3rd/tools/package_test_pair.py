#!/usr/bin/env python3
"""Assemble a new, local-only Baseline/candidate macOS test pair.

This command never launches the game. It validates B0 inputs, runs the bounded
native ``--test-preflight`` check, copies the exact linked Mach-O closure, and
publishes two signed applications together by renaming one completed staging
directory. The disc, ELF, and starting save remain outside both applications.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

import register_baseline
import run_cases
import native_batch
import native_modes


PROFILE = Path(__file__).resolve().parents[1]
REPO = PROFILE.parents[1]
BUILD_SCHEMA = "yakumo-observed-build-v1"
PREFLIGHT_SCHEMA = "yakumo-test-preflight-v1"
LAUNCH_SCHEMA = "yakumo-test-launch-v1"
ROLES = ("baseline", "candidate")
NATIVE_MODES = native_modes.LEGACY_FIELDS
SHA = re.compile(r"[0-9a-f]{64}\Z", re.ASCII)
COMMIT = re.compile(r"[0-9a-f]{40}\Z", re.ASCII)
RECORDER = re.compile(r"source-sha256:[0-9a-f]{64}\Z", re.ASCII)
# The font used by the earlier complete-text Chinese game experiments.
# This is a local macOS setting, not a font redistributed in the bundles.
DEFAULT_GAME_FONT = Path("/System/Library/Fonts/STHeiti Light.ttc")
MAX_JSON = 2 * 1024 * 1024
Run = Callable[..., subprocess.CompletedProcess[str]]


class PairError(ValueError):
    """A test pair cannot be assembled without losing its evidence identity."""


@dataclass(frozen=True)
class PairInputs:
    baseline_build: Path
    candidate_build: Path
    registration: Path
    overlays: Path
    cases: Path
    launcher: Path
    python: Path
    output: Path
    work_root: Path | None = None
    repo: Path = REPO
    moltenvk: Path | None = None
    font: Path | None = None
    game_font: Path | None = None
    execution_profile: Path | None = None


def _json(path: Path) -> dict[str, Any]:
    if path.stat().st_size > MAX_JSON:
        raise PairError(f"JSON is too large: {path}")
    def unique(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise PairError(f"Duplicate JSON key {key}: {path}")
            result[key] = value
        return result
    value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique,
                       parse_constant=lambda value: (_ for _ in ()).throw(PairError("Nonfinite JSON number")))
    if type(value) is not dict:
        raise PairError(f"JSON root must be an object: {path}")
    return value


def _write_json(path: Path, value: dict[str, Any]) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def _real_file(path: Path, label: str) -> Path:
    path = Path(path).expanduser()
    if not path.is_absolute() or path.is_symlink() or not path.is_file():
        raise PairError(f"{label} must be an absolute regular file: {path}")
    return path.resolve(strict=True)


def _real_dir(path: Path, label: str) -> Path:
    path = Path(path).expanduser()
    if not path.is_absolute() or path.is_symlink() or not path.is_dir():
        raise PairError(f"{label} must be an absolute directory: {path}")
    return path.resolve(strict=True)


def _hash(path: Path) -> str:
    before = path.stat()
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    after = path.stat()
    if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) != (
            after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns):
        raise PairError(f"File changed during hashing: {path}")
    return digest.hexdigest()


def _hash_record(path: Path, expected: dict[str, Any], label: str) -> None:
    if not SHA.fullmatch(str(expected.get("sha256", ""))) or type(expected.get("size")) is not int:
        raise PairError(f"Invalid {label} fingerprint")
    if path.stat().st_size != expected["size"] or _hash(path) != expected["sha256"]:
        raise PairError(f"{label} differs from B0 registration: {path}")


def _build(path: Path, role: str, registration: dict[str, Any]) -> dict[str, Any]:
    value = _json(_real_file(path, f"{role} build manifest"))
    required = {"schema", "role", "gameplay_source_commit", "observer_commit",
                "build_config_sha256", "recorder_revision", "baseline_sealed",
                "executable", "executable_sha256", "baseline_provenance"}
    if set(value) != required or value["schema"] != BUILD_SCHEMA or value["role"] != role:
        raise PairError(f"Invalid {role} observed-build schema/role")
    for key in ("gameplay_source_commit", "observer_commit"):
        if type(value[key]) is not str or not COMMIT.fullmatch(value[key]):
            raise PairError(f"Invalid {role} {key}")
    for key in ("build_config_sha256", "executable_sha256"):
        if type(value[key]) is not str or not SHA.fullmatch(value[key]):
            raise PairError(f"Invalid {role} {key}")
    if type(value["recorder_revision"]) is not str or not RECORDER.fullmatch(value["recorder_revision"]):
        raise PairError(f"Invalid {role} recorder revision")
    if type(value["baseline_sealed"]) is not bool or value["baseline_sealed"] != (role == "baseline"):
        raise PairError(f"{role} baseline seal differs")
    executable = _real_file(Path(value["executable"]), f"{role} executable")
    if _hash(executable) != value["executable_sha256"]:
        raise PairError(f"{role} executable fingerprint differs")
    value["executable"] = str(executable)
    if role == "baseline":
        if value["gameplay_source_commit"] != registration["identity"]["source"]["commit"]:
            raise PairError("Baseline gameplay commit is not registered B0")
        record = value["baseline_provenance"]
        if type(record) is not dict or set(record) != {"path", "sha256"} or not SHA.fullmatch(str(record["sha256"])):
            raise PairError("Baseline requires a sealed source audit manifest")
        audit_path = _real_file(Path(record["path"]), "Baseline source audit")
        if _hash(audit_path) != record["sha256"]:
            raise PairError("Baseline source audit fingerprint differs")
        audit = _json(audit_path)
        if (audit.get("baseline_id") != "B0" or
                audit.get("baseline_commit") != value["gameplay_source_commit"] or
                audit.get("baseline_tree") != registration["identity"]["source"]["tree"] or
                audit.get("registered_archive_sha256") != _source_archive_hash(registration) or
                audit.get("observer_checkout_commit") != value["observer_commit"] or
                audit.get("recording_revision") != value["recorder_revision"]):
            raise PairError("Baseline audit does not bind B0 source and recorder")
        retained = audit.get("retained_b0_game_files")
        files = audit.get("files")
        if type(retained) is not list or not retained or type(files) is not list:
            raise PairError("Baseline audit omits retained B0 gameplay source")
        by_path = {item.get("path"): item for item in files if type(item) is dict}
        if len(by_path) != len(files) or any(
                path not in by_path or by_path[path].get("origin") != "B0" for path in retained):
            raise PairError("Baseline audit does not retain its B0 gameplay files")
        computed_content = hashlib.sha256((json.dumps(files, sort_keys=True, separators=(",", ":")) + "\n").encode()).hexdigest()
        if audit.get("source_content_sha256") != computed_content:
            raise PairError("Baseline audit source-content digest differs")
        if not SHA.fullmatch(str(audit.get("source_content_sha256", ""))):
            raise PairError("Baseline audit lacks a source-content identity")
        value["baseline_source_content_sha256"] = audit["source_content_sha256"]
    elif value["baseline_provenance"] is not None:
        raise PairError("Candidate may not claim sealed B0 provenance")
    return value


def _source_archive_hash(registration: dict[str, Any]) -> str:
    matches = [item for item in registration.get("archived_files", []) if item.get("path") == "source.tar"]
    if len(matches) != 1 or not SHA.fullmatch(str(matches[0].get("sha256", ""))):
        raise PairError("B0 source archive registration is invalid")
    return matches[0]["sha256"]


def _registration(path: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    path = _real_file(path, "B0 registration")
    value = _json(path)
    identity = value.get("identity")
    if (value.get("schema") != 1 or type(identity) is not dict or
            identity.get("baseline_id") != "B0" or
            not str(identity.get("source", {}).get("commit", "")).startswith(register_baseline.BASELINE_COMMIT_PREFIX)):
        raise PairError("Registration is not pinned B0")
    source_archive = path.parent / "source.tar"
    archive_record = next((record for record in value.get("archived_files", [])
                           if record.get("path") == "source.tar"), None)
    if archive_record is None:
        raise PairError("Registration lacks B0 source archive")
    _hash_record(_real_file(source_archive, "B0 source archive"), archive_record, "B0 source archive")
    for key in ("iso", "elf"):
        record = identity["inputs"][key]
        source = _real_file(Path(record["path"]), f"registered {key}")
        _hash_record(source, record, f"registered {key}")
    if identity["inputs"]["elf"]["sha256"] != register_baseline.SUPPORTED_ELF_SHA256:
        raise PairError("B0 ELF is not the supported executable")
    save = identity["starting_save"]
    snapshot = _real_dir(Path(save["snapshot_path"]), "B0 starting snapshot")
    register_baseline._verify_snapshot(snapshot, digest=save["snapshot_id"], source=save["source_path"])
    source_save = _real_dir(Path(save["source_path"]), "original starting save")
    if register_baseline._scan_tree(source_save) != save["tree"]:
        raise PairError("Original starting save changed since B0 registration")
    _hash_record(_real_file(snapshot / "manifest.json", "snapshot manifest"),
                 {"size": (snapshot / "manifest.json").stat().st_size,
                  "sha256": value["snapshot_manifest_sha256"]}, "snapshot manifest")
    return value, {"iso": str(Path(identity["inputs"]["iso"]["path"]).resolve()),
                   "elf": str(Path(identity["inputs"]["elf"]["path"]).resolve()),
                   "snapshot": str(snapshot), "source_save": str(source_save)}


def _run(cmd: list[str], *, runner: Run = subprocess.run, env: dict[str, str] | None = None,
         timeout: int = 30) -> str:
    result = runner(cmd, capture_output=True, text=True, check=False, timeout=timeout, env=env)
    if result.returncode:
        raise PairError(f"{cmd[0]} failed ({result.returncode}): {result.stderr.strip()[:500]}")
    return result.stdout


def _preflight(build: dict[str, Any], *, settings: dict[str, str],
               runner: Run = subprocess.run) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="yakumo-pair-preflight-") as directory:
        data = Path(directory)
        (data / "settings.ini").write_text(
            "".join(f"{key}={value}\n" for key, value in sorted(settings.items())), encoding="utf-8")
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("MHP3RD_", "PSPRECOMP_", "DYLD_"))}
        env["MHP3RD_DATA_DIR"] = str(data)
        output = _run([build["executable"], "--test-preflight"], runner=runner, env=env, timeout=30)
    try:
        value = json.loads(output)
    except (ValueError, UnicodeError) as error:
        raise PairError("Native preflight did not return one JSON object") from error
    required = {"schema", "recorder_revision", "configuration_sha256", "build_config_sha256",
                "gameplay_source_commit", "baseline_sealed", "renderer_compiled", "aot_probes_compiled"}
    required.add("baseline_provenance_sha256")
    if type(value) is not dict or set(value) not in (required, required | {"native_mode_schema"}) or value["schema"] != PREFLIGHT_SCHEMA:
        raise PairError("Native preflight schema differs")
    if "native_mode_schema" in value and value["native_mode_schema"] != native_modes.V2_SCHEMA:
        raise PairError("Unknown native mode schema in native preflight")
    for key in ("recorder_revision", "build_config_sha256", "gameplay_source_commit", "baseline_sealed"):
        if value[key] != build[key]:
            raise PairError(f"Native preflight differs from {build['role']} build manifest: {key}")
    if not SHA.fullmatch(str(value["configuration_sha256"])):
        raise PairError("Native preflight configuration hash is invalid")
    expected_provenance = build.get("baseline_source_content_sha256", "")
    if value["baseline_provenance_sha256"] != expected_provenance:
        raise PairError("Native preflight Baseline source provenance differs")
    if value["renderer_compiled"] is not True or value["aot_probes_compiled"] is not True:
        raise PairError("Test build lacks renderer or AOT probes")
    return value


def _otool_deps(path: Path, *, runner: Run = subprocess.run) -> list[str]:
    lines = _run(["otool", "-L", str(path)], runner=runner).splitlines()
    if not lines or not lines[0].endswith(":"):
        raise PairError(f"Could not parse Mach-O imports: {path}")
    return [line.strip().split(" (", 1)[0] for line in lines[1:] if line.strip()]


def _otool_id(path: Path, *, runner: Run = subprocess.run) -> str | None:
    """Mach-O bundles may use a .dylib filename but have no LC_ID_DYLIB."""
    lines = _run(["otool", "-D", str(path)], runner=runner).splitlines()
    if not lines or not lines[0].endswith(":"):
        raise PairError(f"Could not parse Mach-O install name: {path}")
    return lines[1].strip() if len(lines) > 1 and lines[1].strip() else None


def _rpaths(path: Path, *, runner: Run = subprocess.run) -> list[str]:
    lines = _run(["otool", "-l", str(path)], runner=runner).splitlines()
    values: list[str] = []
    for index, line in enumerate(lines):
        if line.strip() == "cmd LC_RPATH":
            for later in lines[index + 1:index + 5]:
                match = re.match(r"\s*path (\S+) \(offset \d+\)", later)
                if match:
                    values.append(match.group(1))
                    break
    return values


def _system_dependency(name: str) -> bool:
    return name.startswith(("/usr/lib/", "/System/Library/"))


def _dependency_file(name: str, source: Path, executable: Path, *, runner: Run) -> Path:
    if name.startswith("/"):
        return _real_file(Path(name).resolve(strict=True), "Mach-O dependency")
    if name.startswith("@loader_path/"):
        return _real_file((source.parent / name[len("@loader_path/"):]).resolve(strict=True), "Mach-O dependency")
    if name.startswith("@executable_path/"):
        return _real_file((executable.parent / name[len("@executable_path/"):]).resolve(strict=True), "Mach-O dependency")
    if name.startswith("@rpath/"):
        suffix = name[len("@rpath/"):]
        for rpath in _rpaths(source, runner=runner):
            candidate = (rpath.replace("@loader_path", str(source.parent))
                         .replace("@executable_path", str(executable.parent)))
            if candidate.startswith("/") and (Path(candidate) / suffix).is_file():
                return _real_file((Path(candidate) / suffix).resolve(strict=True), "Mach-O dependency")
        raise PairError(f"Unresolved @rpath dependency {name} of {source}")
    raise PairError(f"Unsupported Mach-O dependency {name} of {source}")


def dependency_closure(executable: Path, overlays: Path, moltenvk: Path, *,
                       runner: Run = subprocess.run) -> dict[str, Path]:
    """Resolve every non-system linked library from actual Mach-O imports."""
    roots = [executable, moltenvk, *sorted(overlays.glob("*.dylib"))]
    queue = roots[:]
    visited: set[Path] = set()
    libraries: dict[str, Path] = {moltenvk.name: moltenvk}
    while queue:
        source = queue.pop()
        resolved = source.resolve(strict=True)
        if resolved in visited:
            continue
        visited.add(resolved)
        identity = _otool_id(source, runner=runner)
        for index, name in enumerate(_otool_deps(source, runner=runner)):
            if identity is not None and index == 0 and name == identity:
                # A dylib's first otool -L entry is its own install name.
                continue
            if _system_dependency(name):
                continue
            dependency = _dependency_file(name, source, executable, runner=runner)
            if dependency.resolve(strict=True) == resolved:
                continue
            basename = Path(name).name
            other = libraries.get(basename)
            if other is not None and other.resolve(strict=True) != dependency.resolve(strict=True):
                if _hash(other) != _hash(dependency):
                    raise PairError(f"Conflicting Mach-O libraries named {basename}")
            else:
                libraries[basename] = dependency
            queue.append(dependency)
    return dict(sorted(libraries.items()))


def _license_files(library: Path) -> tuple[str, str, list[Path]]:
    parts = library.resolve(strict=True).parts
    if "Cellar" not in parts:
        raise PairError(f"Cannot identify an installed dependency license: {library}")
    index = parts.index("Cellar")
    if len(parts) < index + 3:
        raise PairError(f"Incomplete Homebrew dependency provenance: {library}")
    formula, version = parts[index + 1:index + 3]
    root = Path(*parts[:index + 3])
    licenses = [item for item in root.iterdir() if item.is_file() and
                item.name.lower().startswith(("license", "copying", "notice"))]
    share = root / "share" / "licenses"
    if share.is_dir():
        licenses.extend(item for item in share.rglob("*") if item.is_file())
    if not licenses:
        raise PairError(f"No installed license text for {formula} {version}")
    return formula, version, sorted(set(licenses))


def _copy_checked(source: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    before = _hash(source)
    shutil.copyfile(source, target)
    if _hash(target) != before:
        raise PairError(f"Source changed during bundle copy: {source}")


def _copy_licenses(resources: Path, libraries: dict[str, Path], font: Path) -> list[dict[str, Any]]:
    licenses = resources / "licenses"
    licenses.mkdir(parents=True, exist_ok=True)
    static = {
        "Yakumo-LICENSE.txt": REPO / "LICENSE",
        "DearImGui-LICENSE.txt": PROFILE / "third_party/imgui/LICENSE.txt",
        "tiny-AES-c-UNLICENSE.txt": PROFILE / "third_party/tiny_aes/UNLICENSE",
        "xxHash-LICENSE.txt": PROFILE / "third_party/xxhash/LICENSE",
        "NotoSansCJK-OFL.txt": font.parent.parent / "licenses/NotoSansCJK-OFL.txt",
    }
    for name, source in static.items():
        _copy_checked(_real_file(source, name), licenses / name)
    stb_header = (PROFILE / "third_party/stb_truetype.h").read_text(encoding="utf-8")
    stb_start = stb_header.rfind("ALTERNATIVE B - Public Domain")
    stb_end = stb_header.find("\n------------------------------------------------------------------------------", stb_start)
    if stb_start < 0 or stb_end < 0:
        raise PairError("stb public-domain dedication was not found in the source header")
    (licenses / "stb-PUBLIC-DOMAIN.txt").write_text(stb_header[stb_start:stb_end] + "\n", encoding="utf-8")
    provenance: dict[tuple[str, str], dict[str, Any]] = {}
    for name, source in libraries.items():
        formula, version, texts = _license_files(source)
        record = provenance.setdefault((formula, version), {"formula": formula, "installed_version": version,
                                                          "libraries": [], "license_files": []})
        record["libraries"].append({"name": name, "source": str(source), "sha256": _hash(source)})
        for text in texts:
            destination = licenses / formula / version / text.name
            if not destination.exists():
                _copy_checked(text, destination)
            elif _hash(destination) != _hash(text):
                raise PairError(f"Distinct license texts share one filename: {formula}/{version}/{text.name}")
            relative = destination.relative_to(resources).as_posix()
            if relative not in record["license_files"]:
                record["license_files"].append(relative)
    records = sorted(provenance.values(), key=lambda row: (row["formula"], row["installed_version"]))
    for record in records:
        record["libraries"].sort(key=lambda row: row["name"])
        record["license_files"].sort()
    _write_json(resources / "testing" / "dependency-provenance.json",
                {"schema": "yakumo-local-dependencies-v1", "dependencies": records})
    lines = ["# Local test bundle third-party notices", "",
             "This app is assembled from the exact locally installed libraries listed in",
             "`testing/dependency-provenance.json`. Their installed version labels and SHA-256",
             "values identify the files used in this test pair; this is not a release build.", "",
             "Source components: Dear ImGui, tiny-AES-c, stb, xxHash, and Noto Sans CJK.",
             "Their license texts are in this directory. Dynamic library licenses are in",
             "the formula/version subdirectories below.", ""]
    for record in records:
        lines.append(f"- {record['formula']} {record['installed_version']}: " +
                     ", ".join(item["name"] for item in record["libraries"]))
    (licenses / "THIRD_PARTY_NOTICES.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return records


def _moltenvk_icd(library: Path) -> dict[str, Any]:
    candidates = (
        library.parent.parent / "etc/vulkan/icd.d/MoltenVK_icd.json",
        library.parent.parent / "Resources/vulkan/icd.d/MoltenVK_icd.json",
    )
    source = next((path for path in candidates if path.is_file()), None)
    if source is None:
        raise PairError("Installed MoltenVK has no matching ICD manifest")
    value = _json(source)
    icd = value.get("ICD")
    if (type(icd) is not dict or type(value.get("file_format_version")) is not str or
            type(icd.get("api_version")) is not str or
            type(icd.get("is_portability_driver")) is not bool):
        raise PairError("Installed MoltenVK ICD manifest is malformed")
    return {"file_format_version": value["file_format_version"], "ICD": {
        "library_path": "../../../Frameworks/libMoltenVK.dylib",
        "api_version": icd["api_version"],
        "is_portability_driver": icd["is_portability_driver"],
    }}


def _rewrite_links(path: Path, libraries: dict[str, Path], framework: Path, *, runner: Run) -> None:
    identity = _otool_id(path, runner=runner)
    needs_rpath = False
    for index, name in enumerate(_otool_deps(path, runner=runner)):
        if identity is not None and index == 0 and name == identity:
            continue
        if _system_dependency(name):
            continue
        needs_rpath = True
        basename = Path(name).name
        if basename == path.name and path.parent == framework:
            continue
        if basename not in libraries:
            raise PairError(f"Dependency disappeared before bundle link rewrite: {name}")
        target = "@rpath/" + basename
        if name != target:
            _run(["install_name_tool", "-change", name, target, str(path)], runner=runner)
    if identity is not None:
        _run(["install_name_tool", "-id", "@rpath/" + path.name, str(path)], runner=runner)
    for rpath in _rpaths(path, runner=runner):
        if rpath not in ("@executable_path/../Frameworks", "@loader_path", "@loader_path/.."):
            _run(["install_name_tool", "-delete_rpath", rpath, str(path)], runner=runner)
    wanted = "@executable_path/../Frameworks" if path.parent.name == "MacOS" else (
        "@loader_path/.." if path.parent.name == "overlays" else "@loader_path")
    if needs_rpath and wanted not in _rpaths(path, runner=runner):
        _run(["install_name_tool", "-add_rpath", wanted, str(path)], runner=runner)


def _verify_links(path: Path, libraries: dict[str, Path], *, runner: Run) -> None:
    identity = _otool_id(path, runner=runner)
    for index, name in enumerate(_otool_deps(path, runner=runner)):
        if identity is not None and index == 0 and name == identity:
            if name != "@rpath/" + path.name:
                raise PairError(f"Framework dylib install name remains external: {path}: {name}")
            continue
        if _system_dependency(name):
            continue
        if name != "@rpath/" + Path(name).name or Path(name).name not in libraries:
            raise PairError(f"Bundle still links outside its dependency closure: {path}: {name}")
    for rpath in _rpaths(path, runner=runner):
        if not rpath.startswith(("@executable_path/", "@loader_path")):
            raise PairError(f"Bundle retains an external Mach-O rpath: {path}: {rpath}")


def _bundle_app(app: Path, *, role: str, build: dict[str, Any], launcher: Path,
                scripts: list[Path], cases: Path, python: Path, overlays: Path,
                libraries: dict[str, Path], font: Path, settings_hash: str,
                settings: dict[str, str],
                registration: dict[str, Any], sources: dict[str, str],
                overlay_tree: dict[str, Any], overlay_id: str,
                work_root: Path, output: Path, batch_id: str, runner: Run,
                execution_profile: dict[str, Any] | None = None,
                mode_schema: str | None = None) -> dict[str, Any]:
    contents = app / "Contents"
    macos = contents / "MacOS"
    framework = contents / "Frameworks"
    resources = contents / "Resources"
    testing = resources / "testing"
    for directory in (macos, framework / "overlays", testing,
                      resources / "fonts", resources / "vulkan/icd.d"):
        directory.mkdir(parents=True, exist_ok=True)
    game = macos / "YakumoGame"
    wrapper = macos / "YakumoTestLauncher"
    _copy_checked(Path(build["executable"]), game)
    _copy_checked(launcher, wrapper)
    game.chmod(0o755)
    wrapper.chmod(0o755)
    for item in overlay_tree["files"]:
        _copy_checked(overlays / item["path"], framework / "overlays" / item["path"])
    for name, source in libraries.items():
        _copy_checked(source, framework / name)
    for binary in [game, wrapper, *sorted(framework.glob("*.dylib")),
                   *sorted((framework / "overlays").glob("*.dylib"))]:
        _rewrite_links(binary, libraries, framework, runner=runner)
        _verify_links(binary, libraries, runner=runner)
    # Use a current local font and ICD without carrying any game resources.
    _copy_checked(font, resources / "fonts" / font.name)
    if "libMoltenVK.dylib" not in libraries:
        raise PairError("MoltenVK is missing from the bundle dependency closure")
    icd = _moltenvk_icd(libraries["libMoltenVK.dylib"])
    _write_json(resources / "vulkan/icd.d/MoltenVK_icd.json", icd)
    _copy_checked(PROFILE / "packaging/macos/Yakumo.icns", resources / "Yakumo.icns")
    _copy_licenses(resources, libraries, font)
    for script in scripts:
        _copy_checked(script, testing / script.name)
    _copy_checked(cases, testing / "cases.json")
    if execution_profile is not None:
        _write_json(testing / "execution-profile.json", execution_profile)
    (testing / "python.path").write_text(str(python) + "\n", encoding="utf-8")
    (testing / "python.sha256").write_text(_hash(python) + "\n", encoding="ascii")
    # Separate delivered batches in Launch Services as well as in window titles.
    # Reusing a role-only identifier makes old and corrected copies ambiguous.
    batch_tag = hashlib.sha256(batch_id.encode("utf-8")).hexdigest()[:12]
    display_name = "Yakumo " + role.title() + " (" + batch_id + ")"
    plist = {
        "CFBundleDevelopmentRegion": "en", "CFBundleExecutable": "YakumoTestLauncher",
        "CFBundleIconFile": "Yakumo", "CFBundleIdentifier": "io.github.teamgdb.yakumo.batch-" + batch_tag + ".test." + role,
        "CFBundleInfoDictionaryVersion": "6.0", "CFBundleName": display_name,
        "CFBundleDisplayName": display_name, "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": "0.1", "CFBundleVersion": "1",
        "LSMinimumSystemVersion": "13.0", "LSApplicationCategoryType": "public.app-category.action-games",
        "NSHighResolutionCapable": True, "LSSupportsGameMode": True,
        "NSDesktopFolderUsageDescription": "This local test reads your registered disc and save inputs.",
    }
    (contents / "Info.plist").write_bytes(plistlib.dumps(plist))
    (contents / "PkgInfo").write_bytes(b"APPL????")
    for binary in [*sorted((framework / "overlays").glob("*.dylib")),
                   *sorted(framework.glob("*.dylib")), game]:
        _run(["codesign", "--force", "--sign", "-", "--timestamp=none", str(binary)], runner=runner)
    binary_sha = _hash(game)
    installed_overlays = register_baseline._scan_tree(framework / "overlays")
    installed_overlay_id = register_baseline._tree_id(installed_overlays)
    mode_fields = native_modes.fields(mode_schema)
    if mode_schema == native_modes.V2_SCHEMA and execution_profile is None:
        raise PairError("Versioned native modes require an explicit execution profile")
    if execution_profile is not None:
        profile_schema = (native_modes.V2_SCHEMA if execution_profile["schema"] == native_batch.V2_SCHEMA else None)
        if mode_schema != profile_schema:
            raise PairError("Execution profile and binary native mode schemas differ")
    if role == "baseline":
        selected_modes = {name: "off" for name in mode_fields}
    elif execution_profile is not None:
        selected_modes = dict(execution_profile["candidate_modes"])
    else:
        selected_modes = {name: "verify" for name in mode_fields}
    if set(selected_modes) != set(mode_fields):
        raise PairError("Execution profile does not declare exactly the binary's native modes")
    config = {
        "schema": LAUNCH_SCHEMA, "role": role, "batch_id": batch_id,
        "baseline_id": "B0", "baseline_commit": registration["identity"]["source"]["commit"],
        "source_commit": build["gameplay_source_commit"],
        "binary": {"path": str(output / app.name / "Contents/MacOS/YakumoGame"), "sha256": binary_sha},
        "inputs": {key: {"path": sources[key], "sha256": registration["identity"]["inputs"][key]["sha256"]}
                   for key in ("iso", "elf")},
        "overlays": {"path": str(output / app.name / "Contents/Frameworks/overlays"),
                     "tree_id": installed_overlay_id, **installed_overlays},
        "starting_save": {"path": str(Path(sources["snapshot"]) / "files"),
                          "tree_id": registration["identity"]["starting_save"]["snapshot_id"],
                          **registration["identity"]["starting_save"]["tree"]},
        "cases": {"path": str(output / app.name / "Contents/Resources/testing/cases.json"),
                  "sha256": hashlib.sha256(json.dumps(run_cases.load_case_catalog(cases), sort_keys=True,
                                                       separators=(",", ":"), ensure_ascii=False).encode("utf-8")).hexdigest()},
        "work_root": str(work_root), "settings": dict(settings),
        "native_modes": selected_modes,
        "probe_selection": "all", "configuration_sha256": settings_hash,
        "build_config_sha256": build["build_config_sha256"],
        "recorder_revision": build["recorder_revision"],
        "baseline_provenance_sha256": build.get("baseline_source_content_sha256", ""),
    }
    if mode_schema is not None:
        config["native_mode_schema"] = mode_schema
    _write_json(testing / "launch-config.json", config)
    _run(["codesign", "--force", "--sign", "-", "--timestamp=none", str(app)], runner=runner)
    _run(["codesign", "--verify", "--deep", "--strict", str(app)], runner=runner)
    if _hash(game) != binary_sha:
        raise PairError("Application signing changed the recorded game binary")
    return config


def package_pair(inputs: PairInputs, *, runner: Run = subprocess.run,
                 enforce_local_output: bool = True) -> dict[str, Any]:
    repo = _real_dir(inputs.repo, "repository")
    output = Path(inputs.output).expanduser().absolute()
    if output.exists() or output.is_symlink() or not output.parent.is_dir():
        raise PairError("Output must be a new directory with an existing parent")
    if enforce_local_output:
        expected = repo / "out/testing/dist"
        if output != expected and expected not in output.parents:
            raise PairError("Test applications must be published under ignored out/testing/dist")
        register_baseline._require_ignored(repo, output)
    registration, sources = _registration(inputs.registration)
    baseline = _build(inputs.baseline_build, "baseline", registration)
    candidate = _build(inputs.candidate_build, "candidate", registration)
    if (baseline["observer_commit"] != candidate["observer_commit"] or
            baseline["recorder_revision"] != candidate["recorder_revision"] or
            baseline["build_config_sha256"] != candidate["build_config_sha256"]):
        raise PairError("Paired builds do not share recorder revision and build configuration")
    game_font = _real_file(inputs.game_font or DEFAULT_GAME_FONT, "game text font")
    if any(character in str(game_font) for character in "\r\n\0"):
        raise PairError("Game font path cannot contain settings delimiters")
    settings = {"ui.language": "zh-CN", "text.font": str(game_font)}
    preflight_base = _preflight(baseline, settings=settings, runner=runner)
    preflight_candidate = _preflight(candidate, settings=settings, runner=runner)
    if preflight_base["configuration_sha256"] != preflight_candidate["configuration_sha256"]:
        raise PairError("Paired builds have different effective configuration")
    mode_schema = preflight_base.get("native_mode_schema")
    if mode_schema != preflight_candidate.get("native_mode_schema"):
        raise PairError("Paired builds have different native mode schemas")
    overlays = _real_dir(inputs.overlays, "overlay directory")
    overlay_tree = register_baseline._scan_tree(overlays)
    if len(overlay_tree["files"]) != 355 or overlay_tree["dirs"] or any(
            not item["path"].endswith(".dylib") for item in overlay_tree["files"]):
        raise PairError("Supported B0 pair requires exactly 355 flat overlay dylibs")
    historical = registration["identity"].get("historical_build", {}).get("overlays_tree", {})
    recorded_names = {item["path"] for item in historical.get("files", [])}
    if recorded_names and recorded_names != {item["path"] for item in overlay_tree["files"]}:
        raise PairError("Overlay filenames differ from registered B0 overlay corpus")
    overlay_id = register_baseline._tree_id(overlay_tree)
    cases = _real_file(inputs.cases, "case catalog")
    catalog = run_cases.load_case_catalog(cases)
    if not catalog["cases"]:
        raise PairError("Case catalog is empty")
    execution_profile = (native_batch.load_profile(
        _real_file(inputs.execution_profile, "native execution profile"), catalog)
        if inputs.execution_profile is not None else None)
    expected_schema = (native_modes.V2_SCHEMA if execution_profile is not None and
                       execution_profile["schema"] == native_batch.V2_SCHEMA else None)
    if mode_schema != expected_schema:
        raise PairError("Binary native mode schema requires a matching explicit execution profile")
    launcher = _real_file(inputs.launcher, "native test launcher")
    python = _real_file(inputs.python, "external Python interpreter")
    if not os.access(python, os.X_OK):
        raise PairError("External Python interpreter is not executable")
    scripts = [_real_file(PROFILE / "tools" / name, name) for name in (
        "run_package.py", "run_cases.py", "native_batch.py", "native_modes.py", "compare_test_runs.py",
        "launch_test_run.py")]
    moltenvk = _real_file(inputs.moltenvk or Path("/opt/homebrew/lib/libMoltenVK.dylib"), "MoltenVK driver")
    font = _real_file(inputs.font or repo / "out/native-experiment/Yakumo-baseline.app/Contents/Resources/fonts/NotoSansCJKjp-Regular.otf",
                      "CJK font")
    base_closure = dependency_closure(Path(baseline["executable"]), overlays, moltenvk, runner=runner)
    candidate_closure = dependency_closure(Path(candidate["executable"]), overlays, moltenvk, runner=runner)
    if {key: _hash(value) for key, value in base_closure.items()} != {
            key: _hash(value) for key, value in candidate_closure.items()}:
        raise PairError("Paired applications resolve different dynamic library files")
    work_root = (inputs.work_root or repo / "out/testing/runs").absolute()
    if work_root == output or output in work_root.parents or work_root in output.parents:
        raise PairError("Writable run root must be outside the application output")
    batch_id = output.name if output.name != "dist" else "first-pair"
    stage = Path(tempfile.mkdtemp(prefix=".yakumo-pair-", dir=output.parent))
    try:
        configs = {}
        for role, build in (("baseline", baseline), ("candidate", candidate)):
            name = "Yakumo " + role.title() + ".app"
            configs[role] = _bundle_app(
                stage / name, role=role, build=build, launcher=launcher,
                scripts=scripts, cases=cases, python=python, overlays=overlays,
                libraries=base_closure, font=font,
                settings_hash=preflight_base["configuration_sha256"],
                settings=settings,
                registration=registration, sources=sources, overlay_tree=overlay_tree,
                overlay_id=overlay_id, work_root=work_root, output=output,
                batch_id=batch_id, runner=runner, execution_profile=execution_profile,
                mode_schema=mode_schema)
        if configs["baseline"]["binary"]["sha256"] == configs["candidate"]["binary"]["sha256"]:
            raise PairError("Baseline and candidate application binaries are identical")
        if configs["baseline"]["overlays"]["tree_id"] != configs["candidate"]["overlays"]["tree_id"]:
            raise PairError("Paired applications do not contain identical overlay libraries")
        report = {"schema": "yakumo-test-pair-v1", "baseline_id": "B0", "batch_id": batch_id,
                  "applications": {role: str(output / ("Yakumo " + role.title() + ".app")) for role in ROLES},
                  "recorder_revision": baseline["recorder_revision"],
                  "configuration_sha256": preflight_base["configuration_sha256"],
                  "build_config_sha256": baseline["build_config_sha256"],
                  "overlays_tree_id": configs["baseline"]["overlays"]["tree_id"],
                  "overlay_source_tree_id": overlay_id,
                  "python": {"path": str(python), "sha256": _hash(python)},
                  "game_font": {"path": str(game_font), "sha256": _hash(game_font)},
                  "scope": "Apple Silicon local test pair; no game launch or user acceptance"}
        if execution_profile is not None:
            report["execution_profile"] = execution_profile
            report["execution_profile_sha256"] = native_batch.profile_sha256(execution_profile)
        if mode_schema is not None:
            report["native_mode_schema"] = mode_schema
        _write_json(stage / "pair-manifest.json", report)
        if output.exists() or output.is_symlink():
            raise PairError("Output appeared during assembly")
        stage.rename(output)
        return report
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("baseline-build", "candidate-build", "registration", "overlays", "cases",
                 "launcher", "python", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--moltenvk", type=Path, help="Installed local MoltenVK dylib")
    parser.add_argument("--font", type=Path, help="Local Noto CJK font from a previous bundle")
    parser.add_argument("--game-font", type=Path,
                        help="Common game-text font (default: the verified macOS STHeiti Light face)")
    parser.add_argument("--execution-profile", type=Path,
                        help="Declared scale/copy native execution profile bound to the case catalog")
    args = parser.parse_args(argv)
    try:
        report = package_pair(PairInputs(**vars(args)))
    except (OSError, ValueError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        parser.exit(1, f"Pair packaging failed: {error}\n")
    print(json.dumps(report, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
