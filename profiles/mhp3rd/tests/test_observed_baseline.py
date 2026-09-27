"""Synthetic provenance and safety checks for observed B0 source staging."""

from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import re
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock


TOOL = Path(__file__).resolve().parents[1] / "tools" / "prepare_observed_baseline.py"
SPEC = importlib.util.spec_from_file_location("prepare_observed_baseline", TOOL)
assert SPEC and SPEC.loader
PREP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREP)


def git(repo: Path, *args: str, input_data: bytes | None = None) -> bytes:
    return subprocess.run(["git", "-C", str(repo), *args], input=input_data,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True).stdout


class ObservedBaselineTests(unittest.TestCase):
    def test_shared_texture_boundaries_are_staged_without_candidate_bridge(self) -> None:
        for relative in (
            "cmake/TextureCommandInstrumentation.cmake", "tools/instrument_texture_commands.py",
            "host/native/texture_command_dispatch.hpp", "host/native/texture_command_dispatch.cpp",
            "tests/texture_command_dispatch_tests.cpp",
        ):
            self.assertIn(PREP.PROFILE + relative, PREP.OBSERVATION_FILES)
        self.assertNotIn(PREP.PROFILE + "host/native/texture_commands_bridge.cpp", PREP.OBSERVATION_FILES)

    def test_recording_revision_matches_cmake_input_order(self) -> None:
        repo = TOOL.parents[3]
        profile = TOOL.parents[1]
        cmake = (profile / "CMakeLists.txt").read_text(encoding="utf-8")
        start = cmake.index("list(APPEND MHP3RD_RECORDING_REVISION_INPUTS")
        end = cmake.index(")", start)
        appended = re.findall(r'"\$\{MHP3RD_PROFILE_DIR\}/([^"\n]+)"', cmake[start:end])
        self.assertEqual(appended[:3], [
            "host/native/mode_registry.hpp",
            "host/native/vector_metric_dispatch.hpp",
            "host/native/vector_metric_dispatch.cpp",
        ])
        paths = sorted([*profile.glob("host/testing/*.cpp"), *profile.glob("host/testing/*.hpp")])
        paths.extend(profile / name for name in appended)
        joined = "".join(hashlib.sha256(path.read_bytes()).hexdigest() + ";" for path in paths)
        self.assertEqual(PREP._recording_revision(repo), hashlib.sha256(joined.encode("ascii")).hexdigest())

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        git(self.repo, "init", "-q")
        git(self.repo, "config", "user.name", "Synthetic Test")
        git(self.repo, "config", "user.email", "synthetic@example.invalid")
        self.runtime = "src/runtime.cpp"
        self.bridge = "profiles/mhp3rd/host/native/angle_step_bridge.cpp"
        self.observer = "profiles/mhp3rd/host/testing/journal.cpp"
        self._put(self.runtime, b"B0 runtime\n")
        self._put(self.bridge, b"B0 original bridge\n")
        self._put(self.observer, b"B0 placeholder\n")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "synthetic B0")
        self.commit = git(self.repo, "rev-parse", "HEAD").decode().strip()
        self.tree = git(self.repo, "rev-parse", "HEAD^{tree}").decode().strip()
        self.registration = self.root / "registration"
        self.registration.mkdir()
        self.archive = git(self.repo, "archive", "--format=tar", self.commit)
        (self.registration / "source.tar").write_bytes(self.archive)
        self._write_registration(self.archive)
        self._put(self.observer, b"new shared observer\n")
        self.output = self.root / "observed-B0"

    def _put(self, relative: str, content: bytes) -> None:
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)

    def _write_registration(self, archive: bytes) -> None:
        (self.registration / "manifest.json").write_text(json.dumps({
            "identity": {"source": {"commit": self.commit, "tree": self.tree}},
            "archived_files": [{"path": "source.tar", "size": len(archive),
                                "sha256": hashlib.sha256(archive).hexdigest()}],
        }), encoding="utf-8")

    def prepare(self) -> dict:
        with mock.patch.object(PREP, "_recording_revision", return_value="synthetic-revision"):
            return PREP.prepare(self.repo, self.registration, self.output,
                                expected_commit=self.commit, overlay_paths=(self.observer,),
                                retained_paths=(self.bridge,), require_build_seams=False)

    def test_b0_files_survive_and_observer_is_hashed(self) -> None:
        result = self.prepare()
        self.assertEqual((self.output / self.runtime).read_bytes(), b"B0 runtime\n")
        self.assertEqual((self.output / self.bridge).read_bytes(), b"B0 original bridge\n")
        self.assertEqual((self.output / self.observer).read_bytes(), b"new shared observer\n")
        self.assertEqual(result["baseline_tree"], self.tree)
        files = {entry["path"]: entry for entry in result["files"]}
        self.assertEqual(files[self.observer]["origin"], "observation_overlay")
        self.assertEqual(files[self.runtime]["origin"], "B0")
        self.assertEqual(json.loads((self.output / "observed_baseline_manifest.json").read_text()), result)
        self.assertEqual(self.output.joinpath(self.runtime).read_bytes(), b"B0 runtime\n")
        with self.assertRaisesRegex(PREP.PreparationError, "Destination must be new"):
            self.prepare()

    def test_corrupt_registration_is_rejected_before_publication(self) -> None:
        (self.registration / "source.tar").write_bytes(self.archive + b"tampered")
        with self.assertRaisesRegex(PREP.PreparationError, "archive hash differs"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_manifest_cannot_replace_git_identity(self) -> None:
        forged = self.archive + b"forged"
        (self.registration / "source.tar").write_bytes(forged)
        self._write_registration(forged)
        with self.assertRaisesRegex(PREP.PreparationError, "differs from Git commit"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_b0_retained_file_cannot_be_overlaid(self) -> None:
        with self.assertRaisesRegex(PREP.PreparationError, "cannot be overlaid"):
            PREP.prepare(self.repo, self.registration, self.output,
                         expected_commit=self.commit, overlay_paths=(self.bridge,),
                         retained_paths=(self.bridge,), require_build_seams=False)

    def test_symlink_overlay_is_rejected(self) -> None:
        (self.repo / self.observer).unlink()
        (self.repo / self.observer).symlink_to(self.repo / self.runtime)
        with self.assertRaisesRegex(PREP.PreparationError, "not a regular file"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_archive_traversal_is_rejected(self) -> None:
        data = io.BytesIO()
        with tarfile.open(fileobj=data, mode="w") as archive:
            item = tarfile.TarInfo("../escape")
            item.size = 4
            archive.addfile(item, io.BytesIO(b"evil"))
        with self.assertRaisesRegex(PREP.PreparationError, "Unsafe archive"):
            PREP._extract_registered_tar(data.getvalue(), self.root / "scratch")
        self.assertFalse((self.root / "escape").exists())


if __name__ == "__main__":
    unittest.main()
