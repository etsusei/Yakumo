"""Synthetic process and filesystem checks for the OFF-006 runner."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from profiles.mhp3rd.scripts import check_offline as gate  # noqa: E402


def _digest(data):
    return hashlib.sha256(data).hexdigest()


def _git(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args], text=True).strip()


class OfflineFixture(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.repo = Path(temporary.name).resolve() / "repo"
        self.repo.mkdir()
        _git(self.repo, "init", "-q")
        _git(self.repo, "config", "user.name", "Fixture")
        _git(self.repo, "config", "user.email", "fixture@example.invalid")
        (self.repo / ".gitignore").write_text("/out/\n")
        tracked = self.repo / "tracked.txt"
        tracked.write_text("original tracked source\n")
        _git(self.repo, "add", ".gitignore", "tracked.txt")
        _git(self.repo, "commit", "-qm", "fixture")
        tracked.write_text("modified tracked source\n")
        (self.repo / "untracked.txt").write_text("new untracked source\n")
        self.build = self.repo / "out/build"
        (self.build / "bin").mkdir(parents=True)
        (self.build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.repo}\n"
            "PSPRECOMP_PROFILE:STRING=mhp3rd\n"
            "PSPRECOMP_BUILD_TESTS:BOOL=ON\n"
            "CMAKE_BUILD_TYPE:STRING=Release\n"
        )
        self.output = self.repo / "out/testing/offline"
        self.baseline = self.repo / "out/testing/baselines/B0/manifest.json"
        self.baseline.parent.mkdir(parents=True)
        sources = self.repo / "out/originals"
        sources.mkdir()
        self.iso = sources / "disc.iso"
        self.iso.write_bytes(b"synthetic image")
        self.elf = sources / "EBOOT.ELF"
        self.elf.write_bytes(b"synthetic executable")
        self.archive = sources / "source-save.zip"
        self.archive.write_bytes(b"synthetic archive")
        self.save = sources / "source-save"
        (self.save / "empty").mkdir(parents=True)
        (self.save / "nested").mkdir()
        (self.save / "nested/data.bin").write_bytes(b"synthetic save")
        self.manifest = {
            "schema": 1,
            "identity": {
                "baseline_id": "B0",
                "source": {"commit": _git(self.repo, "rev-parse", "HEAD")},
                "inputs": {
                    name: {"path": str(path), "size": path.stat().st_size,
                           "sha256": _digest(path.read_bytes())}
                    for name, path in (("iso", self.iso), ("elf", self.elf),
                                       ("save_archive", self.archive))
                },
                "starting_save": {
                    "source_path": str(self.save),
                    "tree": {"dirs": ["empty", "nested"],
                             "files": [{"path": "nested/data.bin", "size": 14,
                                        "sha256": _digest(b"synthetic save")}]},
                },
            },
        }
        self.manifest["identity"]["inputs"]["supported_elf_sha256"] = _digest(self.elf.read_bytes())
        self.baseline.write_text(json.dumps(self.manifest))
        self.tools = self.repo / "out/fake-tools"
        self.tools.mkdir()
        self._write_executable(self.tools / "cmake", """
import os
import sys
if os.environ.get('OFFLINE_FAKE_MODE') == 'build_fail':
    print('synthetic compiler failure', flush=True)
    sys.exit(3)
print('synthetic build completed')
""")
        self._write_executable(self.tools / "ctest", self._ctest_body())
        for name in gate.BUILD_TARGETS:
            path = self.build / ("psprecomp_tests" if name == "psprecomp_tests" else f"bin/{name}")
            self._write_executable(path, self._native_body())

    @staticmethod
    def _write_executable(path, body):
        path.write_text(f"#!{sys.executable}\n" + body)
        path.chmod(0o755)

    def _ctest_body(self):
        names = repr(gate.CTEST_NAMES)
        return f"""
import json
import os
from pathlib import Path
import sys
names = {names}
repo = Path({str(self.repo)!r})
build = Path({str(self.build)!r})
mode = os.environ.get('OFFLINE_FAKE_MODE', '')
if '--show-only=json-v1' in sys.argv:
    if mode == 'missing_test':
        names = names[:-1]
    tests = []
    for name in names:
        if name in {repr(gate.BUILD_TARGETS)}:
            command = [str(build / ('psprecomp_tests' if name == 'psprecomp_tests' else 'bin/' + name))]
        else:
            pattern = 'test_baseline_registration.py' if name == 'mhp3rd_baseline_registration_tests' else 'test_resource_preparation.py'
            command = [sys.executable, '-m', 'unittest', 'discover', '-s', str(repo / 'profiles/mhp3rd/tests'), '-p', pattern, '-v']
        tests.append({{'name': name, 'command': command}})
    print(json.dumps({{'tests': tests}}))
else:
    if mode == 'mutate_input':
        (repo / 'out/originals/disc.iso').write_bytes(b'changed by fake ctest')
    print('100% tests passed, 0 tests failed out of 11')
"""

    @staticmethod
    def _native_body():
        return """
import os
from pathlib import Path
import sys
import time
name = Path(sys.argv[0]).name
if len(sys.argv) != 2:
    sys.exit(2)
mode = os.environ.get('OFFLINE_FAKE_MODE', '')
if mode == 'timeout' and name == 'mhp3rd_native_angle_tests':
    print('started synthetic original ELF work', flush=True)
    time.sleep(5)
labels = {
    'mhp3rd_native_angle_tests': ('Differential cases', 'Native angle failures'),
    'mhp3rd_native_scale_tests': ('Scale differential cases', 'Native scale failures'),
    'mhp3rd_native_translation_matrix_tests': ('Translation differential cases', 'Native translation failures'),
    'mhp3rd_native_vector_construct_tests': ('Vector constructor differential cases', 'Native vector constructor failures'),
    'mhp3rd_native_matrix_copy_tests': ('Matrix-copy local ELF differential cases', 'Native matrix-copy failures'),
}
if name in labels:
    cases, failures = labels[name]
    counts = {
        'mhp3rd_native_angle_tests': 806432,
        'mhp3rd_native_scale_tests': 100512,
        'mhp3rd_native_translation_matrix_tests': 100512,
        'mhp3rd_native_vector_construct_tests': 100512,
        'mhp3rd_native_matrix_copy_tests': 100348,
    }
    count = 0 if mode == 'zero_cases' and name == 'mhp3rd_native_angle_tests' else counts[name]
    suffix = ', prefix fallbacks: 10000' if name in ('mhp3rd_native_scale_tests', 'mhp3rd_native_translation_matrix_tests') else ''
    print('FAIL expected negative-path diagnostic, handled by suite')
    print(f'{cases}: {count}{suffix}')
    print(f'{failures}: 0')
"""

    def run_gate(self, *, mode="", elf=None, build=None, native_timeout=2):
        with mock.patch.dict(os.environ, {
            "PATH": str(self.tools) + os.pathsep + os.environ.get("PATH", ""),
            "OFFLINE_FAKE_MODE": mode,
        }):
            return gate.run_gate(repo=self.repo, build_dir=build or self.build, elf=elf or self.elf,
                                 baseline=self.baseline, output_dir=self.output,
                                 build_timeout=2, ctest_timeout=2, native_timeout=native_timeout)


class OfflineGateTests(OfflineFixture):
    def test_pass_records_all_commands_counts_hashes_and_relocated_elf(self):
        relocated = self.repo / "out/second-elf/EBOOT.ELF"
        relocated.parent.mkdir()
        relocated.write_bytes(self.elf.read_bytes())
        report = self.run_gate(elf=relocated)
        self.assertEqual(report["status"], "pass", report["errors"])
        self.assertEqual(len(report["commands"]), 12)
        self.assertEqual(report["coverage_tiers"]["synthetic_and_unit"]["executed"], 11)
        original = report["coverage_tiers"]["original_elf_differential"]
        self.assertEqual(len(original["results"]), 5)
        self.assertEqual(original["certified_minimums"], gate.CERTIFIED_MINIMUMS)
        self.assertTrue(all(item["cases"] >= 100348 for item in original["results"].values()))
        self.assertEqual(len(report["binary_hashes"]), 9)
        self.assertEqual(report["inputs"]["before"], report["inputs"]["after"])
        self.assertTrue(report["source"]["dirty"])
        self.assertEqual({item["path"] for item in report["source"]["files"]},
                         {".gitignore", "tracked.txt", "untracked.txt"})
        self.assertEqual(report["build_configuration"]["before"], report["build_configuration"]["after"])
        for command in report["commands"]:
            self.assertEqual(command["exit_status"], 0)
            self.assertTrue(Path(command["log"]).is_file())
        self.assertEqual(json.loads((self.output / "report.json").read_text()), report)

    def test_missing_ctest_is_incomplete_and_replaces_latest_pass(self):
        first = self.run_gate()
        self.assertEqual(first["status"], "pass")
        second = self.run_gate(mode="missing_test")
        self.assertEqual(second["status"], "incomplete")
        self.assertNotEqual(first["run_dir"], second["run_dir"])
        self.assertEqual(json.loads((self.output / "report.json").read_text())["status"], "incomplete")
        self.assertEqual(json.loads((Path(first["run_dir"]) / "report.json").read_text())["status"], "pass")
        self.assertEqual(len(second["commands"]), 6)

    def test_timeout_keeps_partial_log_and_fails(self):
        report = self.run_gate(mode="timeout", native_timeout=0.5)
        self.assertEqual(report["status"], "failed")
        last = report["commands"][-1]
        self.assertTrue(last["timed_out"])
        self.assertIsNone(last["exit_status"])
        self.assertIn("started synthetic original ELF work", Path(last["log"]).read_text())
        self.assertEqual(report["inputs"]["before"], report["inputs"]["after"])

    def test_input_mutation_downgrades_a_pass(self):
        report = self.run_gate(mode="mutate_input")
        self.assertEqual(report["status"], "failed")
        self.assertTrue(any("Original input identity changed" in error for error in report["errors"]))
        self.assertEqual(report["inputs"]["after"]["mismatches"], ["iso"])

    def test_zero_case_summary_fails(self):
        report = self.run_gate(mode="zero_cases")
        self.assertEqual(report["status"], "failed")
        self.assertTrue(any("below certified minimum" in error for error in report["errors"]))

    def test_failed_build_keeps_exit_status_and_log(self):
        report = self.run_gate(mode="build_fail")
        self.assertEqual(report["status"], "failed")
        build = report["commands"][-1]
        self.assertEqual(build["name"], "build-offline-tests")
        self.assertEqual(build["exit_status"], 3)
        self.assertIn("synthetic compiler failure", Path(build["log"]).read_text())
        self.assertEqual(report["inputs"]["before"], report["inputs"]["after"])

    def test_optional_save_archive_is_explicitly_absent(self):
        self.manifest["identity"]["inputs"]["save_archive"] = None
        self.baseline.write_text(json.dumps(self.manifest))
        report = self.run_gate()
        self.assertEqual(report["status"], "pass", report["errors"])
        self.assertFalse(report["inputs"]["before"]["save_archive"]["registered"])

    def test_build_directory_cannot_overlap_original_save(self):
        with self.assertRaisesRegex(gate.GateError, "Build directory overlaps an original input"):
            self.run_gate(build=self.save)
        self.assertFalse(self.output.exists())

    def test_symlink_output_is_rejected_without_touching_target(self):
        target = self.repo / "out/target"
        target.mkdir()
        marker = target / "marker"
        marker.write_bytes(b"leave alone")
        self.output.symlink_to(target, target_is_directory=True)
        with self.assertRaisesRegex(gate.GateError, "Symlink"):
            self.run_gate()
        self.assertEqual(marker.read_bytes(), b"leave alone")


if __name__ == "__main__":
    unittest.main()
