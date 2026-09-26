"""Synthetic child-process checks for the local paired-test supervisor."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "profiles" / "mhp3rd" / "tools"))
import launch_test_run as launch
import run_package as package


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()


def tree(directory: Path) -> dict:
    dirs = []
    files = []
    for item in sorted(directory.rglob("*")):
        relative = item.relative_to(directory).as_posix()
        if item.is_dir():
            dirs.append(relative)
        else:
            raw = item.read_bytes()
            files.append({"path": relative, "size": len(raw), "sha256": digest(raw)})
    shape = {"dirs": sorted(dirs), "files": sorted(files, key=lambda item: item["path"])}
    return {"path": str(directory), "tree_id": digest(canonical(shape)), **shape}


def fake_binary(mode: str, *, role: str, preflight_config: str, source_commit: str) -> str:
    return textwrap.dedent('''\
        #!/usr/bin/env python3
        import hashlib, json, os, pathlib, signal, struct, sys, time, zlib
        MODE = {mode!r}
        ROLE = {role!r}
        PREFLIGHT_CONFIG = {preflight_config!r}
        SOURCE_COMMIT = {source_commit!r}
        data = pathlib.Path(os.environ['MHP3RD_DATA_DIR'])
        root = data.parent
        assert (data / 'EBOOT.ELF').is_file()
        assert (data / 'ms0/PSP/SAVEDATA/ULJM05800/MHP3RD.BIN').is_file()
        assert 'disc_image=' in (data / 'settings.ini').read_text()
        if '--test-preflight' in sys.argv:
            print(json.dumps({{
                'schema': 'yakumo-test-preflight-v1',
                'recorder_revision': 'source-sha256:' + 'f' * 64,
                'configuration_sha256': PREFLIGHT_CONFIG,
                'build_config_sha256': 'b' * 64,
                'gameplay_source_commit': SOURCE_COMMIT,
                'baseline_sealed': ROLE == 'baseline',
                'renderer_compiled': True,
                'aot_probes_compiled': True,
                'baseline_provenance_sha256': 'e' * 64 if ROLE == 'baseline' else '',
            }}), flush=True)
            sys.exit(0)
        keys = ('MHP3RD_INPUT_SCRIPT', 'MHP3RD_GAME_DIR', 'PSPRECOMP_NO_CHAIN',
                'DYLD_INSERT_LIBRARIES', 'MHP3RD_RECORD_DIR', 'MHP3RD_DATA_DIR',
                'MHP3RD_NATIVE_VECTOR_CONSTRUCT', 'MHP3RD_RECORD_PROBES')
        (root / 'child-env.json').write_text(json.dumps({{key: os.environ.get(key) for key in keys}}))
        (data / 'ms0/PSP/SAVEDATA/ULJM05800/MHP3RD.BIN').write_bytes(b'run changed this save')
        record = pathlib.Path(os.environ['MHP3RD_RECORD_DIR'])
        if MODE == 'missing':
            sys.exit(0)
        record.mkdir()
        context = json.loads((root / 'context.json').read_text())
        binary_hash = hashlib.sha256(pathlib.Path(sys.argv[0]).read_bytes()).hexdigest()
        begin = {{
            'schema': 'journal-v1', 'role': os.environ['MHP3RD_RECORD_ROLE'],
            'run_id': os.environ['MHP3RD_RECORD_RUN_ID'],
            'batch_id': os.environ['MHP3RD_RECORD_BATCH_ID'],
            'baseline_id': os.environ['MHP3RD_RECORD_BASELINE_ID'],
            'baseline_commit': os.environ['MHP3RD_RECORD_BASELINE_COMMIT'],
            'recorder_revision': 'source-sha256:' + 'f' * 64,
            'observer_schema': 'observers-v1',
            'recording_mode': 'observational-summary',
            'binary_sha256': binary_hash,
            'context_sha256': os.environ['MHP3RD_RECORD_CONTEXT_SHA256'],
            'case_catalog_sha256': os.environ['MHP3RD_RECORD_CASE_CATALOG_SHA256'],
            'prerequisite_basis_sha256': os.environ['MHP3RD_RECORD_PREREQUISITES_SHA256'],
        }}
        for name in ('MHP3RD_NATIVE_ANGLE_STEP', 'MHP3RD_NATIVE_SCALE_MATRIX',
                     'MHP3RD_NATIVE_TRANSLATION_MATRIX', 'MHP3RD_NATIVE_VECTOR_CONSTRUCT',
                     'MHP3RD_NATIVE_MATRIX_COPY'):
            begin[name] = os.environ[name]
        rows = [(1, begin), (8, {{'event': 'runtime.inputs', 'supported_elf': True,
                                    'elf_sha256': context['elf_sha256']}}),
                (8, {{'event': 'observer.health', 'emission_errors': 0,
                     'dropped_events': 0, 'invalid_events': 0, 'io_failed': False}})]
        if MODE not in ('prefix', 'signal', 'timeout'):
            reason = 'window closed' if MODE in ('close4', 'close5') else 'guest_finished'
            rows.append((2, {{'completed': True, 'stop_reason': reason,
                             'accepted_events': 2, 'written_events': 2,
                             'dropped_events': 0, 'invalid_events': 0}}))
        payload = bytearray(struct.pack('<8sHHI', b'YKMJNL1\\0', 1, 16, 0))
        for sequence, (kind, fields) in enumerate(rows, 1):
            content = json.dumps(fields, separators=(',', ':')).encode()
            header = struct.pack('<4sIQQHH', b'YKE1', len(content), sequence,
                                 sequence * 1000, kind, 0)
            payload.extend(header + struct.pack('<I', zlib.crc32(header + content) & 0xffffffff) + content)
        (record / 'events.journal').write_bytes(payload)
        if MODE == 'signal':
            os.kill(os.getpid(), signal.SIGTERM)
        if MODE == 'timeout':
            time.sleep(5)
        if MODE == 'log':
            print('x' * (2 * 1024 * 1024), flush=True)
        sys.exit({{'close4': 4, 'abnormal4': 4, 'close5': 5}}.get(MODE, 0))
    ''').format(mode=mode, role=role, preflight_config=preflight_config,
                source_commit=source_commit)


class SupervisorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name).resolve()
        self.assets = self.root / "assets"
        self.assets.mkdir()
        self.work = self.root / "work"
        self.work.mkdir()
        self.iso = self.assets / "disc.iso"
        self.iso.write_bytes(b"synthetic ISO bytes")
        self.elf = self.assets / "EBOOT.ELF"
        self.elf.write_bytes(b"synthetic ELF bytes")
        self.overlays = self.assets / "overlays"
        self.overlays.mkdir()
        (self.overlays / "one.bin").write_bytes(b"synthetic overlay")
        self.save = self.assets / "snapshot-files"
        self.save.mkdir()
        for name in ("ICON0.PNG", "MHP3RD.BIN", "PARAM.SFO", "PIC1.PNG"):
            (self.save / name).write_bytes(("starting " + name).encode())
        self.catalog = {"schema": "yakumo-case-catalog-v1", "cases": []}
        self.cases = self.assets / "cases.json"
        self.cases.write_bytes(canonical(self.catalog))
        self.config_hash = "c" * 64
        self.baseline_commit = "4292eb6"
        self.source_commit = self.baseline_commit + "0" * 33
        self.binary = self.assets / "Yakumo"
        self.config_path = self.root / "launch.json"

    def tearDown(self) -> None:
        self.temp.cleanup()

    def make_config(self, mode: str = "close4", *, role: str = "baseline",
                    preflight_config: str | None = None) -> dict:
        source = self.source_commit if role == "baseline" else "a" * 40
        self.binary.write_text(fake_binary(mode, role=role,
                                           preflight_config=preflight_config or self.config_hash,
                                           source_commit=source))
        self.binary.chmod(self.binary.stat().st_mode | stat.S_IXUSR)
        binary_hash = digest(self.binary.read_bytes())
        modes = {name: "off" for name in package.NATIVE_MODE_FIELDS}
        if role == "candidate":
            modes["MHP3RD_NATIVE_VECTOR_CONSTRUCT"] = "verify"
        config = {
            "schema": launch.LAUNCH_SCHEMA, "role": role, "batch_id": "batch-1",
            "baseline_id": "B0", "baseline_commit": self.baseline_commit,
            "source_commit": source,
            "binary": {"path": str(self.binary), "sha256": binary_hash},
            "inputs": {
                "iso": {"path": str(self.iso), "sha256": digest(self.iso.read_bytes())},
                "elf": {"path": str(self.elf), "sha256": digest(self.elf.read_bytes())},
            },
            "overlays": tree(self.overlays), "starting_save": tree(self.save),
            "cases": {"path": str(self.cases), "sha256": digest(canonical(self.catalog))},
            "work_root": str(self.work), "settings": {"ui.language": "zh-CN"},
            "native_modes": modes, "probe_selection": "all",
            "configuration_sha256": self.config_hash,
            "build_config_sha256": "b" * 64,
            "recorder_revision": "source-sha256:" + "f" * 64,
            "baseline_provenance_sha256": "e" * 64 if role == "baseline" else "",
        }
        self.config_path.write_text(json.dumps(config))
        return config

    def run_config(self, *, timeout: float | None = None, prepare_only: bool = False) -> dict:
        with mock.patch.object(package, "SUPPORTED_ELF_SHA256", digest(self.elf.read_bytes())):
            return launch.run_launch(self.config_path, timeout=timeout, prepare_only=prepare_only)

    def test_normal_close_and_candidate_have_bound_independent_runs(self) -> None:
        self.make_config()
        baseline = self.run_config()
        self.assertEqual(baseline["status"], "completed")
        self.assertEqual((baseline["process_status"], baseline["exit_code"], baseline["stop_reason"]),
                         ("exited", 4, "window closed"))
        self.assertTrue(baseline["recording_complete"])
        self.assertTrue(baseline["evidence_complete"])
        self.assertEqual(json.loads((Path(baseline["run_directory"]) / "result.json").read_text()), baseline)
        packaged = package.load_package(Path(baseline["package_path"]))
        self.assertEqual(packaged["validation"]["identity_binding"], "bound")
        self.assertEqual(packaged["manifest"]["context"]["config_sha256"], self.config_hash)
        self.assertEqual(set(Path(baseline["package_path"]).iterdir()),
                         {Path(baseline["package_path"]) / name for name in (*package.ARTIFACT_NAMES, "manifest.json")})
        base_data = Path(baseline["run_directory"]) / "data"
        self.assertEqual((base_data / "EBOOT.ELF").read_bytes(), self.elf.read_bytes())
        self.assertIn(str(self.iso), (base_data / "settings.ini").read_text())
        self.assertEqual((self.save / "MHP3RD.BIN").read_bytes(), b"starting MHP3RD.BIN")
        self.assertEqual(self.iso.read_bytes(), b"synthetic ISO bytes")
        self.assertEqual((self.overlays / "one.bin").read_bytes(), b"synthetic overlay")
        self.make_config("normal", role="candidate")
        candidate = self.run_config()
        self.assertEqual(candidate["status"], "completed")
        self.assertNotEqual(candidate["run_id"], baseline["run_id"])
        self.assertNotEqual(candidate["run_directory"], baseline["run_directory"])
        self.assertEqual((base_data / "ms0/PSP/SAVEDATA/ULJM05800/MHP3RD.BIN").read_bytes(),
                         b"run changed this save")
        self.assertTrue(candidate["recording_complete"])
        self.assertTrue(candidate["evidence_complete"])

    def test_recorder_failure_code_five_overrides_valid_run_end(self) -> None:
        self.make_config("close5")
        result = self.run_config()
        self.assertEqual(result["status"], "incomplete")
        self.assertEqual(result["exit_code"], 5)
        self.assertEqual(result["stop_reason"], "window closed")
        self.assertIn("abnormal_process_exit", result["validation_issues"])
        self.assertIsNotNone(result["package_path"])

    def test_exit_four_requires_a_normal_close_reason(self) -> None:
        self.make_config("abnormal4")
        result = self.run_config()
        self.assertEqual((result["exit_code"], result["stop_reason"]), (4, "guest_finished"))
        self.assertEqual(result["status"], "incomplete")
        self.assertIn("abnormal_process_exit", result["validation_issues"])

    def test_signal_and_zero_exit_prefixes_are_packaged_but_incomplete(self) -> None:
        for mode, process, reason in (("signal", "signaled", "signal:SIGTERM"),
                                      ("prefix", "exited", "journal_open")):
            with self.subTest(mode=mode):
                self.make_config(mode)
                result = self.run_config()
                self.assertEqual(result["status"], "incomplete")
                self.assertEqual(result["process_status"], process)
                self.assertEqual(result["stop_reason"], reason)
                self.assertIsNotNone(result["package_path"])
                self.assertIn("missing_run_end", result["validation_issues"])

    def test_missing_journal_is_explicit_and_never_faked(self) -> None:
        self.make_config("missing")
        result = self.run_config()
        self.assertEqual(result["status"], "incomplete")
        self.assertEqual(result["stop_reason"], "missing_journal")
        self.assertEqual(result["validation_issues"], ["missing_journal"])
        self.assertIsNone(result["package_path"])
        self.assertFalse(Path(result["recording_directory"]).joinpath("events.journal").exists())

    def test_preflight_mismatch_prevents_game_start(self) -> None:
        self.make_config(preflight_config="d" * 64)
        result = self.run_config()
        self.assertEqual(result["status"], "preflight_failed")
        self.assertIn("configuration_sha256", result["error"])
        self.assertFalse((Path(result["run_directory"]) / "child-env.json").exists())
        self.assertIsNone(result["supervisor_path"])

    def test_prepare_only_and_sanitized_environment(self) -> None:
        self.make_config("normal")
        prepared = self.run_config(prepare_only=True)
        self.assertEqual(prepared["status"], "prepared")
        self.assertFalse(Path(prepared["recording_directory"]).exists())
        self.assertEqual(json.loads((Path(prepared["run_directory"]) / "result.json").read_text()), prepared)
        with mock.patch.dict(os.environ, {
            "MHP3RD_INPUT_SCRIPT": "bad-script", "MHP3RD_GAME_DIR": "/bad/game",
            "PSPRECOMP_NO_CHAIN": "1", "DYLD_INSERT_LIBRARIES": "/bad/library",
        }):
            result = self.run_config()
        child = json.loads((Path(result["run_directory"]) / "child-env.json").read_text())
        self.assertEqual(result["status"], "completed")
        for key in ("MHP3RD_INPUT_SCRIPT", "MHP3RD_GAME_DIR", "PSPRECOMP_NO_CHAIN", "DYLD_INSERT_LIBRARIES"):
            self.assertIsNone(child[key])
        self.assertEqual(child["MHP3RD_RECORD_PROBES"], "all")
        self.assertEqual(child["MHP3RD_NATIVE_VECTOR_CONSTRUCT"], "off")

    def test_input_integrity_and_new_output_constraints(self) -> None:
        config = self.make_config()
        config["inputs"]["iso"]["sha256"] = "0" * 64
        self.config_path.write_text(json.dumps(config))
        with self.assertRaisesRegex(launch.LaunchError, "hash differs"):
            self.run_config()
        self.assertFalse((self.work / "runs").exists())
        config = self.make_config()
        config["starting_save"]["files"][0]["path"] = "../escape"
        self.config_path.write_text(json.dumps(config))
        with self.assertRaises(launch.LaunchError):
            self.run_config()
        self.make_config()
        result_file = self.root / "result.json"
        with mock.patch.object(package, "SUPPORTED_ELF_SHA256", digest(self.elf.read_bytes())):
            result = launch.run_launch(self.config_path, result_path=result_file)
            self.assertEqual(json.loads(result_file.read_text())["run_id"], result["run_id"])
            self.assertEqual(launch.result_directory(result_file), Path(result["run_directory"]))
            query = subprocess.run(
                [sys.executable, str(Path(launch.__file__).resolve()),
                 "--result-directory", str(result_file)],
                check=True, capture_output=True, text=True,
            )
            self.assertEqual(query.stdout, result["run_directory"] + "\n")
            with self.assertRaisesRegex(launch.LaunchError, "already exists"):
                launch.run_launch(self.config_path, result_path=result_file)

    def test_symlink_tree_and_preflight_identity_fail_closed(self) -> None:
        config = self.make_config()
        (self.overlays / "link.bin").symlink_to(self.overlays / "one.bin")
        with self.assertRaisesRegex(launch.LaunchError, "tree differs|symlink"):
            self.run_config()
        (self.overlays / "link.bin").unlink()
        config = self.make_config()
        config["overlays"]["tree_id"] = "0" * 64
        self.config_path.write_text(json.dumps(config))
        with self.assertRaisesRegex(launch.LaunchError, "tree identity differs"):
            self.run_config()
        self.make_config()
        linked = self.root / "binary-link"
        linked.symlink_to(self.binary)
        config = json.loads(self.config_path.read_text())
        config["binary"]["path"] = str(linked)
        self.config_path.write_text(json.dumps(config))
        with self.assertRaisesRegex(launch.LaunchError, "symlink|alias"):
            self.run_config()
        self.make_config()
        config = json.loads(self.config_path.read_text())
        config["recorder_revision"] = "source-sha256:" + "e" * 64
        self.config_path.write_text(json.dumps(config))
        result = self.run_config()
        self.assertEqual(result["status"], "preflight_failed")
        self.assertIn("recorder_revision", result["error"])

    def test_catalog_hash_is_normalized_and_no_run_id_is_reused(self) -> None:
        self.cases.write_text(json.dumps(self.catalog, indent=2))
        self.make_config()
        first = self.run_config(prepare_only=True)
        second = self.run_config(prepare_only=True)
        self.assertNotEqual(first["run_id"], second["run_id"])
        self.assertEqual(first["status"], "prepared")
        self.assertEqual(second["status"], "prepared")
        self.assertEqual((self.save / "MHP3RD.BIN").read_bytes(), b"starting MHP3RD.BIN")

    def test_test_only_timeout_and_bounded_logs(self) -> None:
        self.make_config("timeout")
        result = self.run_config(timeout=0.2)
        self.assertEqual(result["status"], "incomplete")
        self.assertEqual(result["process_status"], "unknown")
        self.assertEqual(result["stop_reason"], "timeout")
        self.assertIsNotNone(result["package_path"])
        self.make_config("log")
        result = self.run_config()
        self.assertEqual(result["status"], "completed")
        log = Path(result["run_directory"]) / "game.stdout.log"
        self.assertLessEqual(log.stat().st_size, launch.MAX_LOG_BYTES + 100)
        self.assertIn(b"truncated", log.read_bytes()[-100:])


if __name__ == "__main__":
    unittest.main()
