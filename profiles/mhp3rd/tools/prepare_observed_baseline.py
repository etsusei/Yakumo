#!/usr/bin/env python3
"""Stage a B0-origin source tree with the reviewed shared observation layer.

This command only prepares source. It does not build or launch the game. The
paired build must still select MHP3RD_BASELINE_B0 and verify its binary output.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import shutil
import stat
import subprocess
import tarfile
import tempfile
from pathlib import Path, PurePosixPath


PINNED_B0 = "4292eb66ee66eab37c327575382d071addcf6249"
SCHEMA = "yakumo-observed-b0-source-v1"
PROFILE = "profiles/mhp3rd/"

# Only these revisions are taken from the observation checkout. All other
# tracked files, including the reusable runtime and game-facing parsers, come
# from the registered B0 archive. Mixed main/CMake/HLE seams are reviewed in
# docs/BASELINE_OBSERVATION_BUILD.md and sealed by MHP3RD_BASELINE_B0.
OBSERVATION_FILES = (
    "profiles/mhp3rd/CMakeLists.txt",
    "profiles/mhp3rd/cmake/ProbeInstrumentation.cmake",
    "profiles/mhp3rd/host/main.cpp",
    "profiles/mhp3rd/host/camera/camera_input.cpp",
    "profiles/mhp3rd/host/gpu/texture_decode_policy.hpp",
    "profiles/mhp3rd/host/gpu/vulkan_renderer.cpp",
    "profiles/mhp3rd/host/gpu/vulkan_renderer.hpp",
    "profiles/mhp3rd/host/hle/control_delivery.cpp",
    "profiles/mhp3rd/host/hle/control_delivery.hpp",
    "profiles/mhp3rd/host/hle/hle_media.cpp",
    "profiles/mhp3rd/host/hle/hle_system.cpp",
    "profiles/mhp3rd/host/native/bridge_contracts.hpp",  # Offline harness only.
    "profiles/mhp3rd/host/native/contracts.hpp",         # Baseline mode parsing.
    "profiles/mhp3rd/host/native/mode_registry.hpp",     # Shared mode names; B0 remains off.
    "profiles/mhp3rd/host/native/vector_metric_dispatch.hpp",  # Same-unit observation seam.
    "profiles/mhp3rd/host/native/vector_metric_dispatch.cpp",
    "profiles/mhp3rd/host/overlays.cpp",
    "profiles/mhp3rd/host/overlays.hpp",
    "profiles/mhp3rd/host/settings/settings.cpp",
    "profiles/mhp3rd/host/settings/settings.hpp",
    "profiles/mhp3rd/host/ui/input_script.cpp",
    "profiles/mhp3rd/host/ui/layer.cpp",
    "profiles/mhp3rd/host/ui/menu.cpp",
    "profiles/mhp3rd/host/ui/test_session_screen.cpp",
    "profiles/mhp3rd/host/ui/test_session_screen.hpp",
    "profiles/mhp3rd/host/ui/text_input.cpp",
    "profiles/mhp3rd/host/ui/translations/zh_cn.inc",
    "profiles/mhp3rd/scripts/check_offline.py",
    "profiles/mhp3rd/tools/compare_test_runs.py",
    "profiles/mhp3rd/tools/native_batch.py",
    "profiles/mhp3rd/tools/native_modes.py",
    "profiles/mhp3rd/tools/texture_decode_policy.py",
    "profiles/mhp3rd/tools/instrument_probes.py",
    "profiles/mhp3rd/tools/prepare_observed_baseline.py",
    "profiles/mhp3rd/tools/prepare_resources.py",
    "profiles/mhp3rd/tools/register_baseline.py",
    "profiles/mhp3rd/tools/resource_crosscheck.cpp",
    "profiles/mhp3rd/tools/run_cases.py",
    "profiles/mhp3rd/tools/run_package.py",
    "profiles/mhp3rd/tests/aot_probe_tests.cpp",
    "profiles/mhp3rd/tests/case_catalog_tests.cpp",
    "profiles/mhp3rd/tests/case_controller_tests.cpp",
    "profiles/mhp3rd/tests/case_runtime_fixture.cpp",
    "profiles/mhp3rd/tests/control_delivery_tests.cpp",
    "profiles/mhp3rd/tests/game_observer_tests.cpp",
    "profiles/mhp3rd/tests/iso_image_tests.cpp",
    "profiles/mhp3rd/tests/observation_hook_tests.cpp",
    "profiles/mhp3rd/tests/test_observed_baseline.py",
    "profiles/mhp3rd/tests/overlay_observation_tests.cpp",
    "profiles/mhp3rd/tests/probe_tests.cpp",
    "profiles/mhp3rd/tests/psmf_demuxer_tests.cpp",
    "profiles/mhp3rd/tests/run_package_fixture.cpp",
    "profiles/mhp3rd/tests/runtime_diagnostics_tests.cpp",
    "profiles/mhp3rd/tests/runtime_recording_tests.cpp",
    "profiles/mhp3rd/tests/sdl_observer_tests.cpp",
    "profiles/mhp3rd/tests/session_recorder_tests.cpp",
    "profiles/mhp3rd/tests/state_observation_tests.cpp",
    "profiles/mhp3rd/tests/test_baseline_registration.py",
    "profiles/mhp3rd/tests/test_case_runtime_pipeline.py",
    "profiles/mhp3rd/tests/test_offline_gate.py",
    "profiles/mhp3rd/tests/test_probe_instrumentation.py",
    "profiles/mhp3rd/tests/test_resource_preparation.py",
    "profiles/mhp3rd/tests/test_run_cases.py",
    "profiles/mhp3rd/tests/test_run_comparison.py",
    "profiles/mhp3rd/tests/test_run_package.py",
    "profiles/mhp3rd/tests/test_run_pipeline.py",
    "profiles/mhp3rd/tests/test_session_process.py",
    "profiles/mhp3rd/tests/test_session_screen_tests.cpp",
)

OBSERVATION_TESTING_FILES = (
    "case_catalog.cpp", "case_catalog.hpp", "case_controller.cpp", "case_controller.hpp",
    "case_runtime.cpp", "case_runtime.hpp", "game_observers.cpp", "game_observers.hpp",
    "journal.cpp", "journal.hpp", "overlay_observation.cpp", "overlay_observation.hpp",
    "probes.cpp", "probes.hpp", "runtime_diagnostics.cpp", "runtime_diagnostics.hpp",
    "runtime_recording.cpp", "runtime_recording.hpp", "sdl_observers.cpp", "sdl_observers.hpp",
    "texture_decode_observation.hpp",
    "session_recorder.cpp", "session_recorder.hpp", "state_observation.cpp",
    "state_observation.hpp",
)
OBSERVATION_FILES += tuple(PROFILE + "host/testing/" + name for name in OBSERVATION_TESTING_FILES)

RETAIN_B0 = (
    "profiles/mhp3rd/host/kernel/iso_image.cpp",
    "profiles/mhp3rd/host/movie/psmf_demuxer.cpp",
    "profiles/mhp3rd/host/native/angle_step_bridge.cpp",
    "profiles/mhp3rd/host/native/angle_step_bridge.hpp",
    "profiles/mhp3rd/host/native/scale_matrix.hpp",
    "profiles/mhp3rd/host/native/scale_matrix_bridge.cpp",
    "profiles/mhp3rd/tests/native_angle_tests.cpp",
    "profiles/mhp3rd/tests/native_scale_tests.cpp",
)
CANDIDATE_ONLY_NATIVE = tuple(
    PROFILE + "host/native/" + stem + suffix
    for stem in ("translation_matrix", "vector_construct", "matrix_copy")
    for suffix in (".cpp", ".hpp", "_bridge.cpp")
)

B0_CONTROL_WRITE_LOOP = """        auto &memory = rt.memory();
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t entry = address + i * 16u;
            memory.store32(entry, static_cast<std::uint32_t>(kernel().now_us()));
            memory.store32(entry + 4u, buttons);
            memory.store8(entry + 8u, analog_x);
            memory.store8(entry + 9u, analog_y);
            // Bytes 10 and 11 are the HD release's second stick, not padding.
            // Leaving them zero reads as a full diagonal deflection and turns
            // the camera every frame; 0x80 is the centre the guest tests for.
            memory.store8(entry + 10u, right_x);
            memory.store8(entry + 11u, right_y);
            for (std::uint32_t j = 12u; j < 16u; ++j) memory.store8(entry + j, 0u);
        }
"""


class PreparationError(ValueError):
    """Registered archive, source overlay or destination failed a guard."""


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _git(repo: Path, *args: str) -> bytes:
    result = subprocess.run(["git", "-C", str(repo), *args], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False)
    if result.returncode:
        raise PreparationError("Git source verification failed: " +
                               result.stderr.decode("utf-8", "replace").strip())
    return result.stdout


def _safe_name(name: str) -> str:
    clean = name[:-1] if name.endswith("/") else name
    path = PurePosixPath(clean)
    if not clean or clean.startswith("/") or "\\" in clean or any(
            part in ("", ".", "..", ".git") for part in clean.split("/")):
        raise PreparationError("Unsafe archive or overlay path")
    if str(path) != clean:
        raise PreparationError("Noncanonical archive or overlay path")
    return str(path)


def _source_record(path: str, data: bytes, origin: str) -> dict:
    return {"path": path, "sha256": _sha(data), "size": len(data), "origin": origin}


def _extract_registered_tar(archive_bytes: bytes, stage: Path) -> dict[str, dict]:
    inventory: dict[str, dict] = {}
    with tarfile.open(fileobj=io.BytesIO(archive_bytes), mode="r:") as archive:
        for item in archive:
            name = _safe_name(item.name)
            target = stage / name
            if item.isdir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            if not item.isfile() or name in inventory or item.size > 128 * 1024 * 1024:
                raise PreparationError("Unsupported or duplicate registered source entry")
            if not target.parent.is_dir():
                target.parent.mkdir(parents=True, exist_ok=True)
            stream = archive.extractfile(item)
            if stream is None:
                raise PreparationError("Registered source entry could not be read")
            data = stream.read(item.size + 1)
            if len(data) != item.size:
                raise PreparationError("Registered source entry length differs")
            target.write_bytes(data)
            target.chmod(item.mode & 0o777)
            inventory[name] = _source_record(name, data, "B0")
    if not inventory:
        raise PreparationError("Registered source archive is empty")
    return inventory


def _read_overlay(repo: Path, relative: str) -> tuple[bytes, int]:
    _safe_name(relative)
    source = repo / relative
    if source.is_symlink() or not source.is_file():
        raise PreparationError("Observation source is absent or is not a regular file: " + relative)
    mode = source.stat().st_mode
    if not stat.S_ISREG(mode):
        raise PreparationError("Observation source is not a regular file: " + relative)
    return source.read_bytes(), stat.S_IMODE(mode)


def _recording_revision(stage: Path) -> str:
    testing = PROFILE + "host/testing/"
    paths = sorted(testing + name for name in OBSERVATION_TESTING_FILES)
    paths += [
        PROFILE + "host/native/mode_registry.hpp",
        PROFILE + "host/native/vector_metric_dispatch.hpp",
        PROFILE + "host/native/vector_metric_dispatch.cpp",
        PROFILE + "tools/instrument_probes.py",
        PROFILE + "host/gpu/texture_decode_policy.hpp",
        PROFILE + "host/hle/control_delivery.hpp",
        PROFILE + "host/hle/control_delivery.cpp",
        PROFILE + "host/settings/settings.cpp",
        PROFILE + "host/settings/settings.hpp",
        PROFILE + "host/ui/test_session_screen.cpp",
        PROFILE + "host/ui/test_session_screen.hpp",
        PROFILE + "host/ui/translations/zh_cn.inc",
    ]
    return _sha("".join(_sha((stage / path).read_bytes()) + ";" for path in paths).encode("ascii"))


def prepare(repo: Path, registration: Path, output: Path, *,
            expected_commit: str = PINNED_B0,
            overlay_paths: tuple[str, ...] = OBSERVATION_FILES,
            retained_paths: tuple[str, ...] = RETAIN_B0,
            require_build_seams: bool = True) -> dict:
    repo = repo.resolve(strict=True)
    registration = registration.resolve(strict=True)
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise PreparationError("Destination must be new")
    if not output.parent.is_dir() or output.parent.is_symlink():
        raise PreparationError("Destination parent must be an existing directory")
    if len(set(overlay_paths)) != len(overlay_paths):
        raise PreparationError("Duplicate observation overlay path")
    for path in overlay_paths + retained_paths:
        _safe_name(path)
    if set(overlay_paths) & set(retained_paths):
        raise PreparationError("A retained B0 file cannot be overlaid")

    manifest = json.loads((registration / "manifest.json").read_text(encoding="utf-8"))
    source = manifest["identity"]["source"]
    commit = source["commit"]
    if commit != expected_commit or source["tree"] != _git(repo, "rev-parse", commit + "^{tree}").decode().strip():
        raise PreparationError("Registered B0 commit or tree differs")
    archive_record = next((record for record in manifest["archived_files"]
                           if record["path"] == "source.tar"), None)
    archive = registration / "source.tar"
    if archive_record is None or not archive.is_file():
        raise PreparationError("Registered B0 source archive is missing")
    archive_bytes = archive.read_bytes()
    if len(archive_bytes) != archive_record["size"] or _sha(archive_bytes) != archive_record["sha256"]:
        raise PreparationError("Registered B0 source archive hash differs")
    if _sha(_git(repo, "archive", "--format=tar", commit)) != archive_record["sha256"]:
        raise PreparationError("Registered B0 archive differs from Git commit")

    if require_build_seams:
        cmake = (repo / PROFILE / "CMakeLists.txt").read_text(encoding="utf-8")
        main = (repo / PROFILE / "host/main.cpp").read_text(encoding="utf-8")
        media = (repo / PROFILE / "host/hle/hle_media.cpp").read_text(encoding="utf-8")
        if "MHP3RD_BASELINE_B0" not in cmake or "MHP3RD_BASELINE_B0" not in main:
            raise PreparationError("Baseline build/native mode seals are absent")
        if B0_CONTROL_WRITE_LOOP not in media:
            raise PreparationError("Controller delivery no longer retains the B0 write loop")
        if "deliver_control_buffer(rt.memory()" in media:
            raise PreparationError("Controller delivery still invokes the candidate refactor")

    stage = Path(tempfile.mkdtemp(prefix=".observed-b0-", dir=output.parent))
    try:
        inventory = _extract_registered_tar(archive_bytes, stage)
        for relative in retained_paths:
            if relative not in inventory:
                raise PreparationError("Required B0 game source is missing: " + relative)
        overrides = []
        for relative in overlay_paths:
            data, mode = _read_overlay(repo, relative)
            target = stage / relative
            prior = inventory.get(relative)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            target.chmod(mode)
            inventory[relative] = _source_record(relative, data, "observation_overlay")
            overrides.append({"path": relative, "b0_sha256": prior["sha256"] if prior else None,
                              "observed_sha256": _sha(data)})
        for relative in retained_paths:
            if inventory[relative]["origin"] != "B0":
                raise PreparationError("Required B0 game source was replaced: " + relative)
        if any(relative in inventory for relative in CANDIDATE_ONLY_NATIVE):
            raise PreparationError("Candidate native source entered the Baseline tree")
        for relative, record in inventory.items():
            if relative.startswith(("src/", "include/psprecomp/")) and record["origin"] != "B0":
                raise PreparationError("Reusable runtime source was replaced")

        records = sorted(inventory.values(), key=lambda item: item["path"])
        content_id = _sha((json.dumps(records, sort_keys=True, separators=(",", ":")) + "\n").encode())
        observer_commit = _git(repo, "rev-parse", "HEAD").decode().strip()
        result = {
            "schema": SCHEMA,
            "baseline_id": "B0",
            "baseline_commit": commit,
            "baseline_tree": source["tree"],
            "registered_archive_sha256": archive_record["sha256"],
            "observer_checkout_commit": observer_commit,
            "recording_revision": "source-sha256:" + _recording_revision(stage),
            "source_content_sha256": content_id,
            "retained_b0_game_files": list(retained_paths),
            "overrides": overrides,
            "files": records,
        }
        (stage / "observed_baseline_manifest.json").write_text(
            json.dumps(result, sort_keys=True, indent=2) + "\n", encoding="utf-8")
        stage.rename(output)
        return result
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path("."))
    parser.add_argument("--registration", type=Path, default=Path("out/testing/baselines/B0"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = prepare(args.repo, args.registration, args.output)
    except (OSError, ValueError, KeyError, tarfile.TarError) as error:
        parser.error(str(error))
    print(json.dumps({key: result[key] for key in (
        "baseline_commit", "recording_revision", "source_content_sha256")}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
