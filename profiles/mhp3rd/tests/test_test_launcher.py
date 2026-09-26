"""Mac launcher checks with a synthetic supervisor; no game binary is used."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock


REPO = Path(__file__).resolve().parents[3]
SOURCE = REPO / "profiles/mhp3rd/tools/test_launcher.cpp"
SHA_SOURCE = REPO / "src/sha256.cpp"


@unittest.skipUnless(sys.platform == "darwin", "Native launcher is macOS only")
class NativeTestLauncherTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.build = tempfile.TemporaryDirectory(prefix="yakumo-launcher-build-")
        cls.launcher = Path(cls.build.name) / "YakumoTestLauncher"
        subprocess.run(
            ["clang++", "-std=c++20", "-Wall", "-Wextra", "-Wpedantic",
             "-I", str(REPO / "include"), str(SOURCE), str(SHA_SOURCE),
             "-framework", "CoreFoundation", "-framework", "CoreServices",
             "-o", str(cls.launcher)],
            check=True, capture_output=True, text=True, timeout=30,
        )

    @classmethod
    def tearDownClass(cls) -> None:
        cls.build.cleanup()

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="yakumo-launcher-test-")
        self.root = Path(self.temp.name).resolve()
        self.script = self.root / "fake_supervisor.py"
        self.script.write_text(textwrap.dedent("""\
            import json
            import os
            from pathlib import Path
            import sys

            args = sys.argv[1:]
            config = json.loads(Path(args[args.index('--config') + 1]).read_text())
            Path(config['marker']).write_text('invoked')
            Path(config['env_marker']).write_text(json.dumps({
                key: os.environ.get(key) for key in
                ('PYTHONPATH', 'PYTHONHOME', 'MHP3RD_NATIVE_ANGLE_STEP', 'TMPDIR')
            }))
            if config.get('noise'):
                print('x' * (256 * 1024), flush=True)
                print('y' * (256 * 1024), file=sys.stderr, flush=True)
            if config['mode'] == 'missing':
                sys.exit(0)
            result_file = Path(args[args.index('--result-file') + 1])
            run_directory = Path(config['run_directory'])
            run_directory.mkdir()
            (run_directory / 'evidence.txt').write_text('synthetic evidence')
            if config['mode'] == 'malformed':
                result_file.write_text('{bad json')
                sys.exit(0)
            status = config['mode']
            assert status != 'prepared' or '--prepare-only' in args
            result = {
                'schema': 'yakumo-test-launch-result-v1',
                'role': 'candidate', 'run_id': run_directory.name,
                'run_directory': str(run_directory),
                'status': status, 'recording_complete': status == 'completed',
                'evidence_complete': status == 'completed' and config.get('evidence', True),
                'package_path': str(run_directory / 'package') if status == 'completed' else None,
                'error': 'Synthetic preflight failure' if status == 'preflight_failed' else None,
            }
            result_file.write_text(json.dumps(result))
            sys.exit(config.get('exit_code', 0))
        """), encoding="utf-8")
        self.config = self.root / "config.json"
        self.python = Path(sys.executable).resolve()
        self.python_hash = hashlib.sha256(self.python.read_bytes()).hexdigest()

    def tearDown(self) -> None:
        self.temp.cleanup()

    def run_launcher(self, mode: str, *, prepare_only: bool = False,
                     noise: bool = False, exit_code: int = 0,
                     digest: str | None = None, evidence: bool = True) -> subprocess.CompletedProcess[str]:
        self.marker = self.root / "invoked"
        self.env_marker = self.root / "supervisor-env.json"
        self.run_directory = self.root / "runs" / "run-synthetic"
        self.run_directory.parent.mkdir(exist_ok=True)
        self.config.write_text(json.dumps({
            "marker": str(self.marker), "run_directory": str(self.run_directory),
            "env_marker": str(self.env_marker),
            "mode": mode, "noise": noise, "exit_code": exit_code,
            "evidence": evidence,
        }), encoding="utf-8")
        command = [str(self.launcher), "--headless", "--python", str(self.python),
                   "--python-sha256", digest or self.python_hash,
                   "--script", str(self.script), "--config", str(self.config)]
        if prepare_only:
            command.append("--prepare-only")
        return subprocess.run(command, capture_output=True, text=True, timeout=15,
                              env={**os.environ, "TMPDIR": str(self.root)})

    def test_prepare_only_preserves_run_evidence_and_cleans_private_result(self) -> None:
        result = self.run_launcher("prepared", prepare_only=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("status prepared", result.stdout)
        self.assertTrue(self.marker.exists())
        self.assertEqual((self.run_directory / "evidence.txt").read_text(), "synthetic evidence")
        self.assertFalse(list(self.root.glob("yakumo-test-launch-*")))

    def test_completed_means_collected_without_claiming_gameplay_success(self) -> None:
        result = self.run_launcher("completed")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("collected", result.stdout)
        self.assertNotIn("passed", result.stdout.lower())

    def test_incomplete_and_preflight_failure_are_explicit(self) -> None:
        for status in ("incomplete", "preflight_failed"):
            with self.subTest(status=status):
                result = self.run_launcher(status)
                self.assertEqual(result.returncode, 1)
                self.assertIn("incomplete", result.stdout)
                self.assertIn(f"status {status}", result.stdout)
                self.run_directory.joinpath("evidence.txt").unlink()
                self.run_directory.rmdir()
                self.marker.unlink()

    def test_missing_or_malformed_result_fails_closed(self) -> None:
        for mode in ("missing", "malformed"):
            with self.subTest(mode=mode):
                result = self.run_launcher(mode)
                self.assertEqual(result.returncode, 1)
                self.assertIn("incomplete", result.stdout)
                if self.run_directory.exists():
                    self.run_directory.joinpath("evidence.txt").unlink()
                    self.run_directory.rmdir()
                self.marker.unlink()

    def test_success_json_cannot_override_child_exit_failure(self) -> None:
        result = self.run_launcher("completed", exit_code=7)
        self.assertEqual(result.returncode, 1)
        self.assertIn("supervisor exit 7", result.stdout)

    def test_completed_label_requires_complete_evidence(self) -> None:
        result = self.run_launcher("completed", evidence=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn("incomplete", result.stdout)

    def test_pinned_python_hash_is_checked_before_spawn(self) -> None:
        result = self.run_launcher("prepared", prepare_only=True, digest="0" * 64)
        self.assertEqual(result.returncode, 2)
        self.assertIn("SHA-256 differs", result.stderr)
        self.assertFalse(self.marker.exists())

    def test_overrides_require_headless(self) -> None:
        result = subprocess.run([str(self.launcher), "--script", str(self.script)],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertIn("require --headless", result.stderr)

    def test_supervisor_output_is_bounded(self) -> None:
        result = self.run_launcher("prepared", prepare_only=True, noise=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("truncated", result.stderr)
        self.assertLess(len(result.stdout) + len(result.stderr), 2048)

    def test_python_environment_excludes_injection_and_game_switches(self) -> None:
        with mock.patch.dict(os.environ, {
            "PYTHONPATH": "/unexpected/modules", "PYTHONHOME": "/unexpected/home",
            "MHP3RD_NATIVE_ANGLE_STEP": "native",
        }):
            result = self.run_launcher("prepared", prepare_only=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        observed = json.loads(self.env_marker.read_text())
        self.assertEqual(observed["TMPDIR"], str(self.root))
        for key in ("PYTHONPATH", "PYTHONHOME", "MHP3RD_NATIVE_ANGLE_STEP"):
            self.assertIsNone(observed[key])


if __name__ == "__main__":
    unittest.main()
