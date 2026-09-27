#!/usr/bin/env python3
"""Offline checks for local, signed paired-application assembly."""

from __future__ import annotations

import hashlib
import json
import plistlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
import launch_test_run  # noqa: E402
import native_batch  # noqa: E402
import package_test_pair as pair  # noqa: E402
import register_baseline  # noqa: E402


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def put(path: Path, data: bytes) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def catalog(path: Path) -> Path:
    return put(path, json.dumps({"schema": "yakumo-case-catalog-v1", "cases": [{
        "id": "REC-01", "version": 1, "title": "Start", "steps": ["Begin"],
        "checkpoints": ["ready"], "required_probes": [
            {"entry": entry, "min_calls": 1} for entry in native_batch.REQUIRED_NATIVE_ENTRIES],
        "required_state_fields": [], "human_acceptance": True,
    }]}).encode())


class FakeMachO:
    def __init__(self) -> None:
        self.deps: dict[Path, list[str]] = {}
        self.ids: dict[Path, str] = {}
        self.rpaths: dict[Path, list[str]] = {}

    def __call__(self, command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
        action, path = command[1], Path(command[2])
        if action == "-L":
            values = self.deps[path]
            output = str(path) + ":\n" + "".join("\t" + value + " (compatibility version 1.0.0)\n" for value in values)
        elif action == "-D":
            output = str(path) + ":\n" + (self.ids[path] + "\n" if path in self.ids else "")
        else:
            lines = []
            for value in self.rpaths.get(path, []):
                lines += ["cmd LC_RPATH", "cmdsize 32", "path " + value + " (offset 12)"]
            output = "\n".join(lines)
        return subprocess.CompletedProcess(command, 0, output, "")


class PairPackagingTests(unittest.TestCase):
    def test_baseline_build_binds_the_observer_checkout(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            executable = put(root / "game", b"synthetic executable")
            files = [{"path": "host/original.cpp", "origin": "B0", "sha256": "d" * 64}]
            audit = {"baseline_id": "B0", "baseline_commit": "4" * 40,
                     "baseline_tree": "5" * 40, "registered_archive_sha256": "6" * 64,
                     "observer_checkout_commit": "7" * 40,
                     "recording_revision": "source-sha256:" + "8" * 64,
                     "retained_b0_game_files": ["host/original.cpp"], "files": files,
                     "source_content_sha256": sha((json.dumps(files, sort_keys=True,
                         separators=(",", ":")) + "\n").encode())}
            audit_path = put(root / "audit.json", json.dumps(audit).encode())
            manifest = {"schema": pair.BUILD_SCHEMA, "role": "baseline",
                        "gameplay_source_commit": audit["baseline_commit"],
                        "observer_commit": audit["observer_checkout_commit"],
                        "build_config_sha256": "9" * 64,
                        "recorder_revision": audit["recording_revision"], "baseline_sealed": True,
                        "executable": str(executable), "executable_sha256": pair._hash(executable),
                        "baseline_provenance": {"path": str(audit_path), "sha256": pair._hash(audit_path)}}
            registration = {"identity": {"source": {"commit": audit["baseline_commit"],
                             "tree": audit["baseline_tree"]}},
                            "archived_files": [{"path": "source.tar", "sha256": "6" * 64}]}
            path = put(root / "build.json", json.dumps(manifest).encode())
            self.assertEqual(pair._build(path, "baseline", registration)["baseline_source_content_sha256"],
                             audit["source_content_sha256"])
            manifest["observer_commit"] = "a" * 40
            path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(pair.PairError, "does not bind"):
                pair._build(path, "baseline", registration)

    def test_registration_checks_original_iso_elf_and_save(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            iso = put(root / "disc.iso", b"synthetic disc")
            elf = put(root / "game.elf", b"synthetic elf")
            source_save = root / "source-save"
            put(source_save / "DATA.BIN", b"starting save")
            tree = register_baseline._scan_tree(source_save)
            (root / "snapshots").mkdir()
            snapshot = register_baseline._register_snapshot(root / "snapshots", source_save, tree)
            archive = put(root / "B0/source.tar", b"tracked source archive")
            identity = {
                "baseline_id": "B0",
                "source": {"commit": "4292eb6" + "a" * 33},
                "inputs": {
                    "iso": {"path": str(iso), "size": iso.stat().st_size, "sha256": pair._hash(iso)},
                    "elf": {"path": str(elf), "size": elf.stat().st_size, "sha256": pair._hash(elf)},
                },
                "starting_save": {"source_path": str(source_save), "snapshot_path": str(snapshot),
                                  "snapshot_id": snapshot.name, "tree": tree},
            }
            manifest = {"schema": 1, "identity": identity,
                        "archived_files": [{"path": "source.tar", "size": archive.stat().st_size,
                                            "sha256": pair._hash(archive)}],
                        "snapshot_manifest_sha256": pair._hash(snapshot / "manifest.json")}
            path = root / "B0/manifest.json"
            path.write_text(json.dumps(manifest))
            with mock.patch.object(register_baseline, "SUPPORTED_ELF_SHA256", pair._hash(elf)):
                loaded, sources = pair._registration(path)
                self.assertEqual(loaded["identity"]["baseline_id"], "B0")
                self.assertEqual(sources["snapshot"], str(snapshot))
                iso.write_bytes(b"changed disc")
                with self.assertRaisesRegex(pair.PairError, "registered iso differs"):
                    pair._registration(path)

    def test_preflight_requires_exact_build_and_sealed_source(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            executable = put(Path(temporary) / "game", b"fake")
            settings = {"ui.language": "zh-CN", "text.font": str(Path(temporary) / "Chinese game font.ttc")}
            build = {"role": "baseline", "executable": str(executable),
                     "recorder_revision": "source-sha256:" + "a" * 64,
                     "build_config_sha256": "b" * 64,
                     "gameplay_source_commit": "4292eb6" + "a" * 33,
                     "baseline_sealed": True, "baseline_source_content_sha256": "c" * 64}

            def run(command: list[str], **options: object) -> subprocess.CompletedProcess[str]:
                data = Path(options["env"]["MHP3RD_DATA_DIR"])  # type: ignore[index]
                for variable in ("MHP3RD_INPUT_SCRIPT", "PSPRECOMP_NO_CHAIN", "DYLD_INSERT_LIBRARIES"):
                    self.assertNotIn(variable, options["env"])
                self.assertEqual(dict(line.split("=", 1) for line in
                                      (data / "settings.ini").read_text().splitlines()), settings)
                self.assertEqual(command[1], "--test-preflight")
                value = {"schema": pair.PREFLIGHT_SCHEMA, "recorder_revision": build["recorder_revision"],
                         "configuration_sha256": "d" * 64, "build_config_sha256": build["build_config_sha256"],
                         "gameplay_source_commit": build["gameplay_source_commit"],
                         "baseline_sealed": True, "renderer_compiled": True,
                         "aot_probes_compiled": True,
                         "baseline_provenance_sha256": build["baseline_source_content_sha256"]}
                return subprocess.CompletedProcess(command, 0, json.dumps(value), "")

            with mock.patch.dict("os.environ", {"MHP3RD_INPUT_SCRIPT": "unwanted",
                                 "PSPRECOMP_NO_CHAIN": "1", "DYLD_INSERT_LIBRARIES": "unwanted"}):
                self.assertEqual(pair._preflight(build, settings=settings, runner=run)["configuration_sha256"], "d" * 64)
            reported = build["baseline_source_content_sha256"]
            build["baseline_source_content_sha256"] = "e" * 64
            self.assertNotEqual(reported, build["baseline_source_content_sha256"])
            with self.assertRaisesRegex(pair.PairError, "source provenance differs"):
                def wrong(command: list[str], **options: object) -> subprocess.CompletedProcess[str]:
                    result = run(command, **options)
                    value = json.loads(result.stdout)
                    value["baseline_provenance_sha256"] = reported
                    return subprocess.CompletedProcess(command, 0, json.dumps(value), "")
                pair._preflight(build, settings=settings, runner=wrong)

    def test_dependency_closure_distinguishes_overlay_bundle_from_dylib(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            game = put(root / "game", b"exe")
            overlays = root / "overlays"
            bundle = put(overlays / "first.dylib", b"bundle")
            moltenvk = put(root / "libMoltenVK.dylib", b"driver")
            codec = put(root / "libcodec.1.dylib", b"codec")
            util = put(root / "libutil.1.dylib", b"util")
            fake = FakeMachO()
            fake.deps = {
                game: [str(codec), "/usr/lib/libSystem.B.dylib"],
                bundle: [str(util)],  # Bundles have no LC_ID_DYLIB.
                moltenvk: [str(moltenvk)],
                codec: [str(codec), str(util)],
                util: [str(util), "/usr/lib/libSystem.B.dylib"],
            }
            fake.ids = {moltenvk: str(moltenvk), codec: str(codec), util: str(util)}
            result = pair.dependency_closure(game, overlays, moltenvk, runner=fake)
            self.assertEqual(set(result), {"libMoltenVK.dylib", "libcodec.1.dylib", "libutil.1.dylib"})

    def test_bundle_config_uses_final_signed_hashes_and_supervisor_schema(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            published = root / "dist"
            stage = root / "stage"
            stage.mkdir()
            source_overlay = put(root / "source-overlays/one.dylib", b"overlay")
            overlays = source_overlay.parent
            overlay_tree = register_baseline._scan_tree(overlays)
            snapshot_files = root / "snapshot/files"
            put(snapshot_files / "DATA.BIN", b"save")
            save_tree = register_baseline._scan_tree(snapshot_files)
            iso = put(root / "disc.iso", b"disc")
            elf = put(root / "game.elf", b"ELF")
            source_game = put(root / "game", b"game")
            launcher = put(root / "launcher", b"launcher")
            moltenvk = put(root / "Cellar/molten-vk/1/lib/libMoltenVK.dylib", b"driver")
            put(root / "Cellar/molten-vk/1/etc/vulkan/icd.d/MoltenVK_icd.json",
                b'{"file_format_version":"1.0.0","ICD":{"library_path":"../../../lib/libMoltenVK.dylib",'
                b'"api_version":"1.4.0","is_portability_driver":true}}')
            python = put(root / "python", b"python")
            font = put(root / "fonts/NotoSansCJKjp-Regular.otf", b"font")
            cases = catalog(root / "cases.json")
            registration = {"identity": {
                "source": {"commit": "4292eb6" + "a" * 33},
                "inputs": {"iso": {"sha256": sha(b"disc")}, "elf": {"sha256": sha(b"ELF")}},
                "starting_save": {"snapshot_id": register_baseline._tree_id(save_tree), "tree": save_tree},
            }}
            build = {"gameplay_source_commit": "4292eb6" + "a" * 33,
                     "executable": str(source_game), "build_config_sha256": "b" * 64,
                     "recorder_revision": "source-sha256:" + "c" * 64,
                     "baseline_source_content_sha256": "d" * 64}

            def signed(command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
                if command[0] == "codesign" and "--sign" in command and Path(command[-1]).is_file():
                    with Path(command[-1]).open("ab") as stream:
                        stream.write(b"signed")
                return subprocess.CompletedProcess(command, 0, "", "")

            with (mock.patch.object(pair, "_rewrite_links"),
                  mock.patch.object(pair, "_verify_links"),
                  mock.patch.object(pair, "_copy_licenses")):
                config = pair._bundle_app(
                    stage / "Yakumo Baseline.app", role="baseline", build=build,
                    launcher=launcher, scripts=[TOOLS / "run_package.py", TOOLS / "run_cases.py",
                                               TOOLS / "native_batch.py", TOOLS / "compare_test_runs.py",
                                               TOOLS / "launch_test_run.py"],
                    cases=cases, python=python, overlays=overlays,
                    libraries={"libMoltenVK.dylib": moltenvk}, font=font,
                    settings_hash="e" * 64,
                    settings={"ui.language": "zh-CN", "text.font": str(font)}, registration=registration,
                    sources={"iso": str(iso), "elf": str(elf), "snapshot": str(snapshot_files.parent)},
                    overlay_tree=overlay_tree, overlay_id=register_baseline._tree_id(overlay_tree),
                    work_root=root / "runs", output=published, batch_id="test-pair", runner=signed)
                catalog_hash = sha(json.dumps(pair.run_cases.load_case_catalog(cases), sort_keys=True,
                                              separators=(",", ":"), ensure_ascii=False).encode())
                profile = native_batch.validate_profile({
                    "schema": native_batch.SCHEMA, "id": "native-data-1",
                    "case_catalog_sha256": catalog_hash,
                    "candidate_modes": {
                        switch: ("native" if entry in native_batch.REQUIRED_NATIVE_ENTRIES else "off")
                        for entry, switch in native_batch.NATIVE_SWITCH_BY_ENTRY.items()},
                    "required_native_entries": list(native_batch.REQUIRED_NATIVE_ENTRIES),
                }, pair.run_cases.load_case_catalog(cases))
                candidate = pair._bundle_app(
                    stage / "Yakumo Candidate.app", role="candidate", build=build,
                    launcher=launcher, scripts=[TOOLS / "run_package.py", TOOLS / "run_cases.py",
                                               TOOLS / "native_batch.py", TOOLS / "compare_test_runs.py",
                                               TOOLS / "launch_test_run.py"],
                    cases=cases, python=python, overlays=overlays,
                    libraries={"libMoltenVK.dylib": moltenvk}, font=font,
                    settings_hash="e" * 64,
                    settings={"ui.language": "zh-CN", "text.font": str(font)}, registration=registration,
                    sources={"iso": str(iso), "elf": str(elf), "snapshot": str(snapshot_files.parent)},
                    overlay_tree=overlay_tree, overlay_id=register_baseline._tree_id(overlay_tree),
                    work_root=root / "runs", output=published, batch_id="test-pair", runner=signed,
                    execution_profile=profile)
            stage.rename(published)
            app = published / "Yakumo Baseline.app"
            self.assertEqual(config["binary"]["sha256"], pair._hash(app / "Contents/MacOS/YakumoGame"))
            info = plistlib.loads((app / "Contents/Info.plist").read_bytes())
            self.assertTrue(info["CFBundleIdentifier"].endswith(".test.baseline"))
            self.assertNotEqual(info["CFBundleIdentifier"], "io.github.teamgdb.yakumo.test.baseline")
            self.assertIn("test-pair", info["CFBundleDisplayName"])
            self.assertNotEqual(config["overlays"]["tree_id"], register_baseline._tree_id(overlay_tree))
            self.assertEqual(config["starting_save"]["path"], str(snapshot_files))
            self.assertEqual(config["baseline_provenance_sha256"], "d" * 64)
            self.assertEqual(config["settings"], {"ui.language": "zh-CN", "text.font": str(font)})
            self.assertEqual((app / "Contents/Resources/testing/python.sha256").read_text().strip(), sha(b"python"))
            with mock.patch.object(launch_test_run.run_package, "SUPPORTED_ELF_SHA256", sha(b"ELF")):
                loaded = launch_test_run._load_config(app / "Contents/Resources/testing/launch-config.json")
            self.assertEqual(loaded["role"], "baseline")
            self.assertEqual(loaded["settings"]["text.font"], str(font))
            self.assertEqual(loaded["overlays"]["tree_id"], config["overlays"]["tree_id"])
            candidate_app = published / "Yakumo Candidate.app"
            self.assertEqual(candidate["native_modes"], profile["candidate_modes"])
            self.assertEqual(candidate["batch_id"], "test-pair")
            self.assertNotEqual(candidate["batch_id"], profile["id"])
            self.assertEqual(json.loads((candidate_app / "Contents/Resources/testing/execution-profile.json").read_text()),
                             profile)
            self.assertTrue((candidate_app / "Contents/Resources/testing/native_batch.py").is_file())
            self.assertFalse((app / "Contents/Resources/testing/execution-profile.json").exists())


if __name__ == "__main__":
    unittest.main()
