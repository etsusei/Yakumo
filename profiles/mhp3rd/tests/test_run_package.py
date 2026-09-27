"""Independent synthetic framing and local package safety checks."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zlib


sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from profiles.mhp3rd.tools import run_package as package  # noqa: E402
from profiles.mhp3rd.tools import native_modes  # noqa: E402
from profiles.mhp3rd.tools import texture_decode_policy  # noqa: E402


FILE_HEADER = struct.pack("<8sHHI", b"YKMJNL1\0", 1, 16, 0)
ELF_SHA256 = "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c"


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False, allow_nan=False).encode("utf-8")


def frame(kind, sequence, fields, *, ns=100, raw=None):
    payload = canonical(fields) if raw is None else raw
    prefix = struct.pack("<4sIQQHH", b"YKE1", len(payload), sequence, ns, kind, 0)
    return prefix + struct.pack("<I", zlib.crc32(prefix + payload) & 0xFFFFFFFF) + payload


def digest(data):
    return hashlib.sha256(data).hexdigest()


def context(run_id="run-1", *, complete=True):
    result = {"schema": package.CONTEXT_SCHEMA, "run_id": run_id}
    if complete:
        result.update({
            "game_sha256": digest(b"game"), "elf_sha256": ELF_SHA256,
            "overlay_sha256": digest(b"overlay"),
            "starting_save_sha256": digest(b"starting-save"),
            "config_sha256": digest(b"configuration"),
            "build_config_sha256": digest(b"build"),
            "case_catalog_sha256": digest(b"cases"),
            "source_commit": "4292eb6", "platform_os": "macos",
            "platform_arch": "arm64",
        })
    return result


def begin_fields(ctx, *, bound=True):
    fields = {
        "schema": "journal-v1", "role": "baseline", "run_id": "run-1",
        "batch_id": "batch-1", "baseline_id": "B0",
        "baseline_commit": "4292eb6", "recorder_revision": "fixture-revision",
        "observer_schema": "observers-v1", "recording_mode": "observational-summary",
        "binary_sha256": digest(b"binary"),
    }
    fields.update({name: "off" for name in package.NATIVE_MODE_FIELDS})
    if bound:
        normalized = {name: ctx.get(name) for name in ("schema",) + package.CONTEXT_FIELDS}
        fields["context_sha256"] = digest(canonical(normalized))
    return fields


def health_fields(**changes):
    value = {
        "event": "observer.health", "domain": "game", "emission_errors": 0,
        "dropped_events": 0, "invalid_events": 0, "io_failed": False,
    }
    value.update(changes)
    return value


def runtime_inputs_fields(*, elf_sha=ELF_SHA256, supported=True):
    return {"event": "runtime.inputs", "elf_sha256": elf_sha, "supported_elf": supported}


def run_end(*, completed=True, reason="window closed", caller_count=1, **changes):
    value = {
        "stop_reason": reason, "completed": completed,
        "accepted_events": caller_count, "written_events": caller_count,
        "dropped_events": 0, "invalid_events": 0,
    }
    value.update(changes)
    return value


def supervisor(*, status="exited", code=4, reason="window closed"):
    return {
        "schema": package.SUPERVISOR_SCHEMA, "run_id": "run-1",
        "status": status, "exit_code": code, "stop_reason": reason,
    }


def journal(ctx=None, *, events=None, end=None, bound=True, include_runtime_inputs=True):
    ctx = context() if ctx is None else ctx
    events = [(8, health_fields())] if events is None else events
    if include_runtime_inputs:
        events = [(8, runtime_inputs_fields())] + events
    rows = [(1, begin_fields(ctx, bound=bound))] + events
    rows.append((2, run_end(caller_count=sum(kind not in (1, 2, 12) for kind, _ in events))
                 if end is None else end))
    return FILE_HEADER + b"".join(frame(kind, index, fields, ns=900 - index)
                                  for index, (kind, fields) in enumerate(rows, 1))


class JournalTests(unittest.TestCase):
    @unittest.skipUnless(hasattr(os, "mkfifo"), "FIFO fixtures require POSIX")
    def test_fifo_is_rejected_without_waiting_for_a_writer(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory).resolve() / "events.journal"
            os.mkfifo(path)
            script = ("import sys,json;sys.path.insert(0,sys.argv[1]);import run_package as p;"
                      "print(json.dumps(p.read_journal(sys.argv[2])))")
            result = subprocess.run([sys.executable, "-c", script, str(Path(package.__file__).parent), str(path)],
                                    capture_output=True, timeout=3)
            self.assertEqual(result.returncode, 0, result.stderr)
            recovery = json.loads(result.stdout)
            self.assertEqual(recovery["issue"], "corrupt")
            self.assertIn("regular file", recovery["detail"])

    def test_valid_framing_flat_scalars_and_concurrent_timestamps(self):
        raw = FILE_HEADER + frame(1, 1, {"role": "fixture"}, ns=300) + frame(
            8, 2, {"zero": False, "signed": -(1 << 63), "unsigned": (1 << 64) - 1,
                   "ratio": -0.0, "none": None, "text": "\u65e5\u672c\u8a9e"}, ns=100) + frame(
            2, 3, run_end(caller_count=1), ns=200)
        result = package._read_journal_bytes(raw)
        self.assertEqual(result["issue"], "none")
        self.assertEqual(result["valid_bytes"], len(raw))
        self.assertTrue(result["complete_framing"])
        self.assertIs(result["records"][1]["fields"]["zero"], False)
        self.assertEqual(result["records"][1]["fields"]["unsigned"], (1 << 64) - 1)
        self.assertEqual([r["monotonic_ns"] for r in result["records"]], [300, 100, 200])

    def test_every_truncation_boundary_preserves_prefix(self):
        raw = FILE_HEADER + frame(1, 1, {"role": "fixture"}) + frame(
            8, 2, {"event": "state"}) + frame(2, 3, run_end(caller_count=1))
        boundaries = [0, 16]
        offset = 16
        for item in (frame(1, 1, {"role": "fixture"}),
                     frame(8, 2, {"event": "state"}),
                     frame(2, 3, run_end(caller_count=1))):
            offset += len(item)
            boundaries.append(offset)
        for cut in range(len(raw)):
            result = package._read_journal_bytes(raw[:cut])
            expected = "open" if cut in boundaries[1:] else "truncated"
            self.assertEqual(result["issue"], expected, cut)
            self.assertEqual(result["valid_bytes"], max((n for n in boundaries if n <= cut), default=0), cut)
            self.assertFalse(result["complete_framing"])

    def test_corrupt_tail_crc_and_json_stop_without_resync(self):
        prefix = FILE_HEADER + frame(1, 1, {"role": "fixture"})
        bad = bytearray(frame(8, 2, {"event": "state"}))
        bad[-1] ^= 1
        result = package._read_journal_bytes(prefix + bad + frame(2, 3, run_end()))
        self.assertEqual((result["issue"], result["valid_bytes"]), ("corrupt", len(prefix)))
        self.assertEqual(len(result["records"]), 1)
        for payload in (b'{"x":1,"x":2}', b'{"":1}', b'{"x":{"nested":1}}', b'{"x":[1]}',
                        b'{"x":18446744073709551616}', b'{"x":1e10000}', b'{"x":"\xed\xa0\x80"}'):
            result = package._read_journal_bytes(prefix + frame(8, 2, {}, raw=payload))
            self.assertEqual(result["issue"], "corrupt", payload)
            self.assertEqual(result["valid_bytes"], len(prefix), payload)
        self.assertEqual(package._read_journal_bytes(journal() + b"tail")["issue"], "corrupt")

    def test_unsupported_and_limits(self):
        prefix = FILE_HEADER + frame(1, 1, {"role": "fixture"})
        self.assertEqual(package._read_journal_bytes(prefix + frame(13, 2, {}))["issue"], "unsupported")
        with mock.patch.object(package, "MAX_PAYLOAD_BYTES", 3):
            self.assertEqual(package._read_journal_bytes(prefix)["issue"], "limit_exceeded")
        with mock.patch.object(package, "MAX_RECORDS", 1):
            self.assertEqual(package._read_journal_bytes(prefix + frame(2, 2, {}))["issue"], "limit_exceeded")
        with mock.patch.object(package, "MAX_JOURNAL_BYTES", len(prefix) - 1):
            self.assertEqual(package._read_journal_bytes(prefix)["issue"], "limit_exceeded")
        modified = bytearray(prefix)
        modified[12] = 1
        self.assertEqual(package._read_journal_bytes(modified)["issue"], "corrupt")


class PackageTests(unittest.TestCase):
    def test_texture_policy_keeps_historical_identity_and_validates_new_records(self):
        raw = package._read_journal_bytes(journal(self.ctx))
        begin = raw["records"][0]["fields"]
        self.assertNotIn(texture_decode_policy.SCHEMA_FIELD, package._identity(begin))
        self.assertNotIn(texture_decode_policy.MODE_FIELD, package._identity(begin))
        self.assertTrue(package._validate(raw, self.ctx, supervisor())["metadata_complete"])
        begin.update({texture_decode_policy.SCHEMA_FIELD: texture_decode_policy.SCHEMA,
                      texture_decode_policy.MODE_FIELD: "off"})
        self.assertTrue(package._validate(raw, self.ctx, supervisor())["metadata_complete"])
        self.assertEqual(package._identity(begin)[texture_decode_policy.MODE_FIELD], "off")
        begin[texture_decode_policy.MODE_FIELD] = "native"
        self.assertIn("baseline_texture_decode_not_off", package._validate(raw, self.ctx, supervisor())["issues"])
        begin["role"] = "candidate"
        self.assertTrue(package._validate(raw, self.ctx, supervisor())["metadata_complete"])
        begin[texture_decode_policy.MODE_FIELD] = "invalid"
        self.assertIn("invalid_texture_decode_mode", package._validate(raw, self.ctx, supervisor())["issues"])
        begin[texture_decode_policy.MODE_FIELD] = "off"
        begin[texture_decode_policy.SCHEMA_FIELD] = "future"
        self.assertIn("unknown_texture_decode_schema", package._validate(raw, self.ctx, supervisor())["issues"])
        del begin[texture_decode_policy.MODE_FIELD]
        self.assertIn("incomplete_texture_decode_policy", package._validate(raw, self.ctx, supervisor())["issues"])

    def test_native_mode_schema_requires_exact_declared_switches(self):
        raw = package._read_journal_bytes(journal(self.ctx))
        begin = raw["records"][0]["fields"]
        self.assertNotIn("native_mode_schema", package._identity(begin))
        self.assertEqual(set(package._native_modes(begin)), set(native_modes.LEGACY_FIELDS))
        begin["MHP3RD_NATIVE_VECTOR_NORM"] = "off"
        validation = package._validate(raw, self.ctx, supervisor())
        self.assertIn("undeclared_native_mode:MHP3RD_NATIVE_VECTOR_NORM", validation["issues"])
        self.assertFalse(validation["metadata_complete"])
        begin["native_mode_schema"] = native_modes.V2_SCHEMA
        validation = package._validate(raw, self.ctx, supervisor())
        self.assertIn("missing_native_mode:MHP3RD_NATIVE_VECTOR_DISTANCE", validation["issues"])
        for name in native_modes.VECTOR_FIELDS:
            begin[name] = "off"
        self.assertTrue(package._validate(raw, self.ctx, supervisor())["metadata_complete"])
        self.assertEqual(package._identity(begin)["native_mode_schema"], native_modes.V2_SCHEMA)
        begin["native_mode_schema"] = "unknown-v3"
        validation = package._validate(raw, self.ctx, supervisor())
        self.assertIn("unknown_native_mode_schema", validation["issues"])
        self.assertFalse(validation["metadata_complete"])

    def test_v2_package_persists_schema_and_all_nine_modes(self):
        records = package._read_journal_bytes(journal(self.ctx))["records"]
        begin = records[0]["fields"]
        begin["native_mode_schema"] = native_modes.V2_SCHEMA
        begin.update({name: "off" for name in native_modes.VECTOR_FIELDS})
        raw = FILE_HEADER + b"".join(frame(row["kind"], index, row["fields"])
                                       for index, row in enumerate(records, 1))
        self.write_inputs(raw=raw)
        manifest = self.build()
        self.assertTrue(manifest["validation"]["metadata_complete"])
        self.assertEqual(manifest["identity"]["native_mode_schema"], native_modes.V2_SCHEMA)
        self.assertEqual(set(manifest["native_modes"]), set(native_modes.V2_FIELDS))
        self.assertEqual(package.load_package(self.output)["validation"]["issues"], [])

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="yakumo-package-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name).resolve(strict=True)
        self.run = self.base / "run"
        self.run.mkdir()
        self.ctx = context()
        self.ctx_path = self.base / "context.json"
        self.supervisor_path = self.base / "supervisor.json"
        self.output = self.base / "package"
        self.write_inputs()

    def write_inputs(self, *, raw=None, ctx=None, sup=None):
        self.ctx = self.ctx if ctx is None else ctx
        self.ctx_path.write_bytes(canonical(self.ctx))
        self.supervisor_path.write_bytes(canonical(supervisor() if sup is None else sup))
        (self.run / "events.journal").write_bytes(journal(self.ctx) if raw is None else raw)

    def build(self, *, with_supervisor=True):
        return package.package_run(self.run, self.ctx_path, self.output,
                                   self.supervisor_path if with_supervisor else None)

    def test_case_basis_uses_shared_inputs_and_validates_panel_bindings(self):
        basis = package.prerequisite_basis_sha256(self.ctx, "B0", "4292eb6")
        peer = dict(self.ctx, run_id="other-run", source_commit="b" * 40)
        self.assertEqual(basis, package.prerequisite_basis_sha256(peer, "B0", "4292eb6"))
        self.assertNotEqual(basis, package.prerequisite_basis_sha256(dict(peer, starting_save_sha256="b" * 64), "B0", "4292eb6"))
        data = package._read_journal_bytes(journal(self.ctx))
        begin = data["records"][0]["fields"]
        begin.update(case_catalog_sha256=self.ctx["case_catalog_sha256"], prerequisite_basis_sha256=basis)
        self.assertTrue(package._validate(data, self.ctx, supervisor())["metadata_complete"])
        begin["prerequisite_basis_sha256"] = "0" * 64
        self.assertFalse(package._validate(data, self.ctx, supervisor())["metadata_complete"])
        begin["prerequisite_basis_sha256"] = basis
        begin["case_catalog_sha256"] = "0" * 64
        self.assertFalse(package._validate(data, self.ctx, supervisor())["metadata_complete"])

    def test_legacy_identity_does_not_require_panel_fields(self):
        manifest = self.build()
        self.assertNotIn("case_catalog_sha256", manifest["identity"])
        self.assertNotIn("prerequisite_basis_sha256", manifest["identity"])
        self.assertTrue(package.load_package(self.output)["validation"]["metadata_complete"])

    def test_package_fixed_artifacts_identity_and_source_unchanged(self):
        events = [
            (3, {"event": "case.begin", "case_id": "REC-03"}),
            (5, {"event": "case.checkpoint", "checkpoint_id": "entered"}),
            (9, {"event": "probe.summary", "leaf": "angle", "entry": 1, "completed": 2}),
            (9, {"event": "probe.summary", "leaf": "angle", "entry": 1, "completed": 3}),
            (11, {"event": "runtime.warning", "message": "synthetic"}),
            (8, health_fields()),
        ]
        raw = journal(self.ctx, events=events)
        self.write_inputs(raw=raw)
        for name in ("game.iso", "save.bin", "ram.dump", "stdout.log", "extra.txt"):
            (self.run / name).write_bytes(b"secret synthetic asset")
        source_before = {p.name: p.read_bytes() for p in self.run.iterdir()}
        manifest = self.build()
        self.assertEqual(manifest["schema"], package.PACKAGE_SCHEMA)
        self.assertEqual(manifest["packager_revision"],
                         "source-sha256:" + digest(Path(package.__file__).read_bytes()))
        self.assertEqual(set(p.name for p in self.output.iterdir()),
                         set(package.ARTIFACT_NAMES) | {"manifest.json"})
        self.assertEqual({p.name: p.read_bytes() for p in self.run.iterdir()}, source_before)
        self.assertEqual((self.output / "events.journal").read_bytes(), raw)
        self.assertEqual(stat.S_IMODE((self.output / "events.journal").stat().st_mode) & 0o222, 0)
        self.assertEqual(manifest["identity"]["native_modes"],
                         {name: "off" for name in package.NATIVE_MODE_FIELDS})
        self.assertEqual(manifest["validation"]["identity_binding"], "bound")
        self.assertTrue(manifest["validation"]["recording_complete"])
        self.assertTrue(manifest["validation"]["metadata_complete"])
        loaded = package.load_package(self.output)
        self.assertTrue(loaded["validation"]["recording_complete"])
        self.assertEqual(len(loaded["records"]), len(events) + 3)
        stats = json.loads((self.output / "statistics.json").read_text())
        self.assertEqual(stats["probe_latest"][0]["fields"]["completed"], 3)
        markers = json.loads((self.output / "markers.json").read_text())
        self.assertEqual(len(markers["markers"]), 2)
        diagnostic_lines = (self.output / "diagnostics.jsonl").read_text().splitlines()
        self.assertEqual(len(diagnostic_lines), 1)
        self.assertEqual(json.loads(diagnostic_lines[0])["kind"], 11)
        for name, entry in manifest["artifacts"].items():
            payload = (self.output / name).read_bytes()
            self.assertEqual(entry, {"size": len(payload), "sha256": digest(payload)})

    def test_missing_metadata_unbound_and_supervisor_unknown(self):
        partial = context(complete=False)
        self.write_inputs(ctx=partial, raw=journal(partial, bound=False))
        manifest = self.build(with_supervisor=False)
        validation = manifest["validation"]
        self.assertFalse(validation["recording_complete"])
        self.assertFalse(validation["metadata_complete"])
        self.assertEqual(validation["identity_binding"], "unbound")
        self.assertIn("missing_context:game_sha256", validation["issues"])
        self.assertIn("completion_unknown", validation["issues"])
        self.assertIsNone(manifest["context"]["game_sha256"])

    def test_loss_bad_counters_health_and_corrupt_prefix_are_incomplete(self):
        cases = [
            ([(8, health_fields()), (12, {"dropped_total": 1})], "recording_loss"),
            ([(8, health_fields(emission_errors=1))], "nonzero_observer_emission_errors"),
            ([(8, health_fields(io_failed=True))], "observer_io_failed"),
            ([(8, health_fields())], "nonzero_dropped_events"),
        ]
        for index, (events, expected) in enumerate(cases):
            with self.subTest(index=index):
                end = run_end(caller_count=1 + sum(k not in (1, 2, 12) for k, _ in events))
                if index == 3:
                    end["dropped_events"] = 1
                self.write_inputs(raw=journal(self.ctx, events=events, end=end))
                output = self.base / ("package-" + str(index))
                manifest = package.package_run(self.run, self.ctx_path, output, self.supervisor_path)
                self.assertFalse(manifest["validation"]["recording_complete"])
                self.assertIn(expected, manifest["validation"]["issues"])
        self.write_inputs(raw=journal(self.ctx)[:-3])
        manifest = package.package_run(self.run, self.ctx_path, self.base / "partial", self.supervisor_path)
        self.assertEqual(manifest["journal"]["issue"], "truncated")
        self.assertFalse(manifest["validation"]["recording_complete"])

    def test_window_close_code_four_and_crash(self):
        healthy = self.build()
        self.assertTrue(healthy["validation"]["recording_complete"])
        variants = [
            (supervisor(status="signaled", code=None, reason="SIGKILL"), None, "supervisor_signaled"),
            (supervisor(code=23, reason="process error"), None, "abnormal_process_exit"),
            (supervisor(), run_end(reason="host_exception", caller_count=2), "abnormal_process_exit"),
            (supervisor(code=0, reason="host_exception"),
             run_end(reason="host_exception", caller_count=2), "abnormal_run_stop_reason"),
            (supervisor(code=0, reason="other"),
             run_end(reason="guest_finished", caller_count=2), "supervisor_stop_reason_mismatch"),
        ]
        for index, (sup, end, expected) in enumerate(variants):
            with self.subTest(index=index):
                self.write_inputs(raw=journal(self.ctx, end=end), sup=sup)
                manifest = package.package_run(self.run, self.ctx_path,
                                               self.base / ("exit-" + str(index)), self.supervisor_path)
                self.assertFalse(manifest["validation"]["recording_complete"])
                self.assertIn(expected, manifest["validation"]["issues"])

    def test_reject_symlinks_traversal_existing_and_nested_output(self):
        target = self.run / "events.journal"
        payload = target.read_bytes()
        target.unlink()
        target.symlink_to(self.base / "other.journal")
        (self.base / "other.journal").write_bytes(payload)
        with self.assertRaises(package.PackageError):
            self.build()
        target.unlink()
        target.write_bytes(payload)
        with self.assertRaises(package.PackageError):
            package.package_run(self.run, self.ctx_path, self.run / "inside", self.supervisor_path)
        with self.assertRaises(package.PackageError):
            package.package_run(self.run / ".." / "run", self.ctx_path, self.output,
                                self.supervisor_path)
        self.output.mkdir()
        with self.assertRaises(package.PackageError):
            self.build()
        self.output.rmdir()
        ctx_link = self.base / "context-link.json"
        ctx_link.symlink_to(self.ctx_path)
        with self.assertRaises(package.PackageError):
            package.package_run(self.run, ctx_link, self.output, self.supervisor_path)
        parent_link = self.base / "parent-link"
        parent_link.symlink_to(self.base, target_is_directory=True)
        with self.assertRaises(package.PackageError):
            package.package_run(parent_link / "run", self.ctx_path, self.output,
                                self.supervisor_path)

    def test_unsupported_metadata_and_health_order(self):
        fields = begin_fields(self.ctx)
        fields["observer_schema"] = "observers-v2"
        fields["recording_mode"] = "unknown-mode"
        raw = FILE_HEADER + frame(1, 1, fields) + frame(8, 2, runtime_inputs_fields()) + \
            frame(8, 3, health_fields()) + frame(2, 4, run_end(caller_count=2))
        self.write_inputs(raw=raw)
        validation = package.package_run(self.run, self.ctx_path, self.base / "bad-mode",
                                         self.supervisor_path)["validation"]
        self.assertFalse(validation["metadata_complete"])
        self.assertIn("unsupported_observer_schema", validation["issues"])
        self.assertIn("unsupported_recording_mode", validation["issues"])
        events = [(8, health_fields(emission_errors=1)), (8, health_fields()),
                  (7, {"event": "input.pad"})]
        self.write_inputs(raw=journal(self.ctx, events=events))
        validation = package.package_run(self.run, self.ctx_path, self.base / "bad-health",
                                         self.supervisor_path)["validation"]
        self.assertFalse(validation["recording_complete"])
        self.assertIn("multiple_observer_health", validation["issues"])
        self.assertIn("nonzero_observer_emission_errors", validation["issues"])
        self.assertIn("observer_health_not_final", validation["issues"])

    def test_runtime_executable_evidence_gates_identity(self):
        bad_elf = digest(b"different executable")
        cases = [
            (journal(self.ctx, include_runtime_inputs=False), self.ctx,
             ("missing_runtime_inputs",)),
            (journal(self.ctx, events=[(8, runtime_inputs_fields()),
                                       (8, health_fields())]), self.ctx,
             ("multiple_runtime_inputs",)),
            (journal(self.ctx, events=[(8, runtime_inputs_fields(elf_sha=bad_elf,
                                                                 supported=False)),
                                       (8, health_fields())], include_runtime_inputs=False), self.ctx,
             ("unsupported_runtime_elf", "runtime_elf_profile_mismatch")),
            (journal(self.ctx, events=[(8, runtime_inputs_fields(supported=False)),
                                       (8, health_fields())], include_runtime_inputs=False), self.ctx,
             ("unsupported_runtime_elf", "runtime_inputs_contradiction")),
            (journal(self.ctx, events=[(3, {"event": "case.begin", "case_id": "REC-03"}),
                                       (8, runtime_inputs_fields()), (8, health_fields())],
                     include_runtime_inputs=False), self.ctx,
             ("runtime_inputs_after_case_begin",)),
        ]
        for index, (raw, ctx, expected) in enumerate(cases):
            with self.subTest(index=index):
                self.write_inputs(raw=raw, ctx=ctx)
                output = self.base / ("elf-case-" + str(index))
                validation = package.package_run(self.run, self.ctx_path, output,
                                                  self.supervisor_path)["validation"]
                self.assertTrue(validation["recording_complete"])
                self.assertFalse(validation["metadata_complete"])
                self.assertEqual(validation["identity_binding"], "bound")
                for issue in expected:
                    self.assertIn(issue, validation["issues"])
                self.assertEqual(package.load_package(output)["validation"], validation)

        other_context = dict(self.ctx, elf_sha256=bad_elf)
        self.write_inputs(ctx=other_context, raw=journal(other_context))
        validation = package.package_run(self.run, self.ctx_path,
                                          self.base / "elf-context-mismatch",
                                          self.supervisor_path)["validation"]
        self.assertTrue(validation["recording_complete"])
        self.assertFalse(validation["metadata_complete"])
        self.assertEqual(validation["identity_binding"], "bound")
        self.assertIn("runtime_elf_context_mismatch", validation["issues"])

        missing_elf = dict(self.ctx)
        del missing_elf["elf_sha256"]
        self.write_inputs(ctx=missing_elf, raw=journal(missing_elf))
        validation = package.package_run(self.run, self.ctx_path,
                                          self.base / "elf-context-missing",
                                          self.supervisor_path)["validation"]
        self.assertFalse(validation["metadata_complete"])
        self.assertIn("missing_context:elf_sha256", validation["issues"])

    def test_strict_context_and_supervisor_shapes(self):
        wrong = dict(self.ctx)
        wrong["game_sha256"] = "not a hash"
        self.ctx_path.write_bytes(canonical(wrong))
        with self.assertRaises(package.PackageError):
            self.build()
        self.ctx_path.write_bytes(canonical(self.ctx))
        wrong_supervisor = supervisor(status="signaled", code=4)
        self.supervisor_path.write_bytes(canonical(wrong_supervisor))
        with self.assertRaises(package.PackageError):
            self.build()

    def test_artifact_tamper_and_editable_summary_rejected(self):
        self.build()
        (self.output / "statistics.json").write_text("{}\n")
        loaded = package.load_package(self.output)
        self.assertFalse(loaded["validation"]["recording_complete"])
        self.assertIn("artifact_mismatch:statistics.json", loaded["validation"]["issues"])
        self.assertIn("derived_content_mismatch:statistics.json", loaded["validation"]["issues"])
        manifest_path = self.output / "manifest.json"
        value = json.loads(manifest_path.read_text())
        changed = (self.output / "statistics.json").read_bytes()
        value["artifacts"]["statistics.json"] = {"size": len(changed), "sha256": digest(changed)}
        value["validation"]["recording_complete"] = False
        manifest_path.write_bytes(canonical(value))
        loaded = package.load_package(self.output)
        self.assertIn("derived_content_mismatch:statistics.json", loaded["validation"]["issues"])
        self.assertIn("manifest_validation_mismatch", loaded["validation"]["issues"])

    def test_raw_journal_tamper_reparsed_from_prefix(self):
        self.build()
        raw_file = self.output / "events.journal"
        raw_file.chmod(0o644)
        raw = bytearray(raw_file.read_bytes())
        raw[-1] ^= 1
        raw_file.write_bytes(raw)
        loaded = package.load_package(self.output)
        self.assertFalse(loaded["validation"]["recording_complete"])
        self.assertIn("artifact_mismatch:events.journal", loaded["validation"]["issues"])
        self.assertIn("journal_corrupt", loaded["validation"]["issues"])
        self.assertEqual(loaded["records"][0]["kind"], 1)

    def test_read_detects_source_mutation(self):
        source = self.run / "events.journal"
        original_read = os.read
        changed = False

        def changing_read(fd, amount):
            nonlocal changed
            data = original_read(fd, amount)
            if not changed and data:
                changed = True
                with source.open("ab") as stream:
                    stream.write(b"changed")
            return data

        with mock.patch.object(package.os, "read", side_effect=changing_read):
            with self.assertRaises(package.PackageError):
                self.build()
        self.assertFalse(self.output.exists())

    def test_atomic_publish_does_not_replace_late_output(self):
        stage = self.base / "stage"
        stage.mkdir()
        destination = self.base / "late-output"
        destination.mkdir()
        (destination / "marker").write_text("untouched")
        with self.assertRaises(OSError):
            package._publish_stage(stage, destination)
        self.assertTrue(stage.is_dir())
        self.assertEqual((destination / "marker").read_text(), "untouched")

    def test_cli_preserves_incomplete_package_with_zero_exit(self):
        self.write_inputs(raw=journal(self.ctx)[:-1])
        result = subprocess.run([
            sys.executable, str(Path(package.__file__)), "--run-dir", str(self.run),
            "--context", str(self.ctx_path), "--output", str(self.output),
            "--supervisor", str(self.supervisor_path),
        ], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(json.loads(result.stdout)["validation"]["recording_complete"])
        self.assertTrue(self.output.is_dir())


if __name__ == "__main__":
    unittest.main()
