"""Synthetic, offline checks for the local B0 registration command."""

import hashlib
import subprocess
import sys
import tarfile
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from profiles.mhp3rd.tools import register_baseline as baseline  # noqa: E402


def git(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args], text=True).strip()


class Fixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.repo = Path(self.tmp.name) / "repo"
        self.repo.mkdir()
        git(self.repo, "init", "-q")
        git(self.repo, "config", "user.name", "Fixture")
        git(self.repo, "config", "user.email", "fixture@example.invalid")
        (self.repo / ".gitignore").write_text("/out/\n/profiles/mhp3rd/analysis/\n")
        (self.repo / "tracked.txt").write_text("committed source\n")
        git(self.repo, "add", ".gitignore", "tracked.txt")
        git(self.repo, "commit", "-qm", "synthetic source")
        self.commit = git(self.repo, "rev-parse", "HEAD")
        inputs = self.repo / "out/inputs"
        inputs.mkdir(parents=True)
        self.iso = inputs / "disc.iso"
        self.iso.write_bytes(b"synthetic image bytes only")
        self.elf = inputs / "EBOOT.ELF"
        self.elf.write_bytes(b"synthetic executable bytes only")
        self.save_archive = inputs / "source-save.zip"
        self.save_archive.write_bytes(b"synthetic archive bytes only")
        self.save = inputs / "source-save"
        (self.save / "nested").mkdir(parents=True)
        (self.save / "empty").mkdir()
        (self.save / "PARAM.SFO").write_bytes(b"synthetic save metadata")
        (self.save / "nested/data.bin").write_bytes(b"synthetic save data")
        self.build = self.repo / "out/build"
        (self.build / "bin").mkdir(parents=True)
        (self.build / "profiles/mhp3rd/generated_version").mkdir(parents=True)
        (self.build / "CMakeCache.txt").write_text(
            "CMAKE_BUILD_TYPE:STRING=Release\n"
            "CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/c++\n"
            "SDL3_DIR:PATH=/synthetic/dependency\n"
            "MHP3RD_FFMPEG:BOOL=ON\n"
            "MHP3RD_RENDERER:STRING=Vulkan\n"
            "MHP3RD_RELEASE:BOOL=OFF\n"
            "MHP3RD_LIBAV_libavcodec_VERSION:INTERNAL=1.2.3\n"
        )
        (self.build / "bin/Yakumo").write_bytes(b"historical binary fixture; never execute")
        (self.build / "profiles/mhp3rd/generated_version/yakumo_version.hpp").write_text(
            'inline constexpr const char *kYakumoVersion = "26398fe-dirty";\n'
        )
        self.overlays = self.repo / "out/overlays"
        self.overlays.mkdir()
        (self.overlays / "overlay_0001.bin").write_bytes(b"synthetic overlay")
        self.output = self.repo / "out/testing/baselines/B0"
        self.snapshots = self.repo / "profiles/mhp3rd/analysis/testing/start-saves"
        self.inputs = baseline.RegistrationInputs(
            repo=self.repo,
            commit=self.commit,
            iso=self.iso,
            elf=self.elf,
            save_dir=self.save,
            output=self.output,
            snapshots_root=self.snapshots,
            build_dir=self.build,
            overlays=self.overlays,
            save_archive=self.save_archive,
        )
        self.expected_elf = hashlib.sha256(self.elf.read_bytes()).hexdigest()

    def register(self, inputs=None):
        return baseline.register_baseline(
            inputs or self.inputs,
            expected_elf_sha256=self.expected_elf,
            required_commit_prefix=self.commit[:12],
        )


class RegistrationTests(Fixture):
    def test_source_and_inputs_are_frozen_without_changing_originals(self):
        original = {
            path: path.read_bytes()
            for path in (self.iso, self.elf, self.save_archive, self.save / "PARAM.SFO", self.save / "nested/data.bin")
        }
        untracked = self.repo / "out/inputs/untracked.iso"
        untracked.write_bytes(b"never in source archive")
        manifest = self.register()
        self.assertEqual(manifest["identity"]["source"]["commit"], self.commit)
        self.assertEqual(manifest["identity"]["inputs"]["iso"]["sha256"], hashlib.sha256(original[self.iso]).hexdigest())
        self.assertEqual(manifest["identity"]["inputs"]["supported_elf_sha256"], self.expected_elf)
        self.assertEqual(manifest["identity"]["intended_settings"]["ui.language"], "zh-CN")
        self.assertTrue(all(value == "off" for value in manifest["identity"]["intended_settings"]["native_switches"].values()))
        self.assertRegex(manifest["registered_at_utc"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$")
        self.assertEqual(manifest["identity"]["historical_build"]["reported_version_label"], "26398fe-dirty")
        self.assertIn("unverified", manifest["identity"]["historical_build"]["provenance"])
        self.assertIn("SDL3_DIR", manifest["identity"]["historical_build"]["cmake_cache_entries"])
        self.assertEqual(manifest["identity"]["historical_build"]["cmake_cache_entries"]["MHP3RD_FFMPEG"], "ON")
        self.assertEqual(manifest["identity"]["historical_build"]["cmake_cache_entries"]["MHP3RD_RENDERER"], "Vulkan")
        self.assertEqual(manifest["identity"]["historical_build"]["cmake_cache_entries"]["MHP3RD_RELEASE"], "OFF")
        self.assertEqual(
            manifest["identity"]["historical_build"]["dependencies"]["cmake_dependency_paths_and_versions"]["MHP3RD_LIBAV_libavcodec_VERSION"],
            "1.2.3",
        )
        with tarfile.open(self.output / "source.tar") as archive:
            names = set(archive.getnames())
            self.assertIn("tracked.txt", names)
            self.assertNotIn("out/inputs/untracked.iso", names)
            self.assertEqual(archive.extractfile("tracked.txt").read(), b"committed source\n")
        snapshot = Path(manifest["identity"]["starting_save"]["snapshot_path"])
        self.assertEqual((snapshot / "files/nested/data.bin").read_bytes(), original[self.save / "nested/data.bin"])
        self.assertTrue((snapshot / "files/empty").is_dir())
        self.assertEqual((snapshot / "files/PARAM.SFO").stat().st_mode & 0o222, 0)
        for path, contents in original.items():
            self.assertEqual(path.read_bytes(), contents)

    def test_reuse_is_idempotent_and_verifies_archived_outputs(self):
        first = self.register()
        first_bytes = (self.output / "manifest.json").read_bytes()
        self.assertEqual(self.register(), first)
        self.assertEqual((self.output / "manifest.json").read_bytes(), first_bytes)
        (self.output / "source.tar").write_bytes(b"corrupt")
        with self.assertRaisesRegex(baseline.RegistrationError, "Archived B0"):
            self.register()

    def test_reuse_rejects_archived_binary_and_save_corruption(self):
        self.register()
        binary = self.output / "historical-build/bin/Yakumo"
        binary.write_bytes(b"changed historical binary")
        with self.assertRaisesRegex(baseline.RegistrationError, "Archived B0"):
            self.register()
        binary.write_bytes((self.build / "bin/Yakumo").read_bytes())
        snapshot = next(path for path in self.snapshots.iterdir() if path.is_dir())
        save_file = snapshot / "files/PARAM.SFO"
        save_file.chmod(0o644)
        save_file.write_bytes(b"corrupt")
        with self.assertRaisesRegex(baseline.RegistrationError, "Snapshot files"):
            self.register()

    def test_reuse_rejects_changed_input_identity(self):
        self.register()
        self.iso.write_bytes(b"changed image")
        with self.assertRaisesRegex(baseline.RegistrationError, "different identities"):
            self.register()

    def test_supported_elf_and_pinned_commit_are_required_by_default(self):
        with self.assertRaisesRegex(baseline.RegistrationError, "supported executable"):
            baseline.register_baseline(self.inputs, required_commit_prefix=self.commit[:12])
        with self.assertRaisesRegex(baseline.RegistrationError, "pinned"):
            baseline.register_baseline(self.inputs, expected_elf_sha256=self.expected_elf)
        self.assertFalse(self.output.exists())

    def test_rejects_save_symlinks_and_nested_output(self):
        (self.save / "link").symlink_to(self.iso)
        with self.assertRaisesRegex(baseline.RegistrationError, "symlink"):
            self.register()
        (self.save / "link").unlink()
        (self.save / "linked-dir").symlink_to(self.save / "nested", target_is_directory=True)
        with self.assertRaisesRegex(baseline.RegistrationError, "symlink"):
            self.register()
        (self.save / "linked-dir").unlink()
        nested = replace(self.inputs, output=self.save / "B0")
        with self.assertRaisesRegex(baseline.RegistrationError, "Input and output paths overlap"):
            self.register(nested)
        self.assertFalse(self.output.exists())

    def test_failed_snapshot_stage_is_cleaned(self):
        with mock.patch.object(baseline, "_copy_tree", side_effect=OSError("synthetic copy failure")):
            with self.assertRaisesRegex(OSError, "synthetic copy failure"):
                self.register()
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.snapshots.glob(".save-stage-*")), [])

    def test_failed_artifact_stage_is_cleaned(self):
        original_copyfile = baseline.shutil.copyfile

        def fail_binary(source, target):
            if str(source).endswith("bin/Yakumo"):
                raise OSError("synthetic artifact failure")
            return original_copyfile(source, target)

        with mock.patch.object(baseline.shutil, "copyfile", side_effect=fail_binary):
            with self.assertRaisesRegex(OSError, "synthetic artifact failure"):
                self.register()
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.output.parent.glob(".B0-stage-*")), [])

    def test_source_save_mutation_during_copy_is_detected(self):
        original_copy_tree = baseline._copy_tree

        def mutate_after_copy(source, destination, tree, *, readonly):
            original_copy_tree(source, destination, tree, readonly=readonly)
            (self.save / "PARAM.SFO").write_bytes(b"changed by synthetic concurrent writer")

        with mock.patch.object(baseline, "_copy_tree", side_effect=mutate_after_copy):
            with self.assertRaisesRegex(baseline.RegistrationError, "Save changed"):
                self.register()
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.snapshots.glob(".save-stage-*")), [])

    def test_snapshot_copy_requires_fresh_run_target(self):
        manifest = self.register()
        snapshot = Path(manifest["identity"]["starting_save"]["snapshot_path"])
        fresh = self.repo / "out/runs/one/save"
        baseline.copy_snapshot_to_new_run(snapshot, fresh)
        self.assertEqual((fresh / "nested/data.bin").read_bytes(), b"synthetic save data")
        active = self.repo / "out/runs/active/save"
        active.mkdir(parents=True)
        marker = active / "active-process-marker"
        marker.write_bytes(b"leave this running process alone")
        with self.assertRaisesRegex(baseline.RegistrationError, "already exists"):
            baseline.copy_snapshot_to_new_run(snapshot, active)
        self.assertEqual(marker.read_bytes(), b"leave this running process alone")
        self.assertEqual(list(active.iterdir()), [marker])
        with self.assertRaisesRegex(baseline.RegistrationError, "already exists"):
            baseline.copy_snapshot_to_new_run(snapshot, fresh)
        self.assertEqual(list(active.parent.glob(".run-stage-*")), [])
        with self.assertRaisesRegex(baseline.RegistrationError, "overlaps"):
            baseline.copy_snapshot_to_new_run(snapshot, self.snapshots / "accidental-run")

    def test_unignored_output_is_refused(self):
        unignored = replace(self.inputs, output=self.repo / "visible/B0")
        with self.assertRaisesRegex(baseline.RegistrationError, "not Git-ignored"):
            self.register(unignored)
        self.assertFalse(unignored.output.exists())


if __name__ == "__main__":
    unittest.main()
