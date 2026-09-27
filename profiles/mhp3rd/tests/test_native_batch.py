"""Offline validation of the bounded native execution profile."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import native_batch  # noqa: E402


def catalog() -> dict:
    def case(case_id: str, entries: list[int]) -> dict:
        return {"id": case_id, "version": 1, "title": case_id,
                "steps": ["Observe the village"], "checkpoints": ["done"],
                "required_probes": [{"entry": entry, "min_calls": 1} for entry in entries],
                "required_state_fields": [], "human_acceptance": True}
    return {"schema": "yakumo-case-catalog-v1", "cases": [
        case("SCALE", [0x08878B28]), case("COPY", [0x08879D08])]}


def profile(cases: dict) -> dict:
    return {"schema": native_batch.SCHEMA, "id": "native-data-1",
            "case_catalog_sha256": hashlib.sha256(json.dumps(
                cases, sort_keys=True, separators=(",", ":"), ensure_ascii=False
            ).encode()).hexdigest(),
            "candidate_modes": {
                switch: ("native" if entry in native_batch.REQUIRED_NATIVE_ENTRIES else "off")
                for entry, switch in native_batch.NATIVE_SWITCH_BY_ENTRY.items()},
            "required_native_entries": list(native_batch.REQUIRED_NATIVE_ENTRIES)}


class NativeBatchTests(unittest.TestCase):
    def test_profile_binds_catalog_and_native_probe_union(self) -> None:
        cases = catalog()
        value = profile(cases)
        normalized = native_batch.validate_profile(value, cases)
        self.assertEqual(normalized, value)
        self.assertIsNot(normalized, value)
        self.assertIsNot(normalized["candidate_modes"], value["candidate_modes"])
        self.assertEqual(native_batch.profile_sha256(normalized), hashlib.sha256(json.dumps(
            value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
        ).encode()).hexdigest())

    def test_schema_and_catalog_mismatch_fail(self) -> None:
        cases = catalog()
        for key, wrong in (("schema", "bad"), ("id", "bad/name"),
                           ("case_catalog_sha256", "A" * 64),
                           ("case_catalog_sha256", "0" * 64)):
            with self.subTest(key=key, wrong=wrong):
                value = profile(cases)
                value[key] = wrong
                with self.assertRaises(native_batch.NativeBatchError):
                    native_batch.validate_profile(value, cases)
        for key in ("schema", "id", "candidate_modes", "required_native_entries"):
            with self.subTest(missing=key):
                value = profile(cases)
                del value[key]
                with self.assertRaises(native_batch.NativeBatchError):
                    native_batch.validate_profile(value, cases)
        value = profile(cases)
        value["environment"] = {"MHP3RD_NATIVE_SCALE_MATRIX": "native"}
        with self.assertRaises(native_batch.NativeBatchError):
            native_batch.validate_profile(value, cases)

    def test_only_exact_pilot_modes_and_entries_allowed(self) -> None:
        cases = catalog()
        scale = native_batch.NATIVE_SWITCH_BY_ENTRY[0x08878B28]
        angle = native_batch.NATIVE_SWITCH_BY_ENTRY[0x088775AC]
        for wrong in ("verify", "off", "bogus", True):
            with self.subTest(scale=wrong):
                value = profile(cases)
                value["candidate_modes"][scale] = wrong
                with self.assertRaises(native_batch.NativeBatchError):
                    native_batch.validate_profile(value, cases)
        for change in (lambda m: m.__setitem__(angle, "native"),
                       lambda m: m.pop(angle),
                       lambda m: m.__setitem__("MHP3RD_NATIVE_WRONG", "off")):
            value = profile(cases)
            change(value["candidate_modes"])
            with self.assertRaises(native_batch.NativeBatchError):
                native_batch.validate_profile(value, cases)
        for entries in ([0x08878B28], [0x08878B28, 0x08878B28],
                        [0x08879D08, 0x08878B28], [True, 0x08879D08],
                        [0x08878B28, 0x088775AC]):
            with self.subTest(entries=entries):
                value = profile(cases)
                value["required_native_entries"] = entries
                with self.assertRaises(native_batch.NativeBatchError):
                    native_batch.validate_profile(value, cases)

    def test_declared_native_entries_need_catalog_probe_requirements(self) -> None:
        cases = catalog()
        value = profile(cases)
        cases["cases"][1]["required_probes"] = []
        value["case_catalog_sha256"] = hashlib.sha256(json.dumps(
            cases, sort_keys=True, separators=(",", ":")
        ).encode()).hexdigest()
        with self.assertRaisesRegex(native_batch.NativeBatchError, "lacks required native probe"):
            native_batch.validate_profile(value, cases)

    def test_file_reader_rejects_duplicate_oversize_symlink_and_fifo(self) -> None:
        cases = catalog()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "profile.json"
            path.write_text(json.dumps(profile(cases)), encoding="utf-8")
            self.assertEqual(native_batch.load_profile(path, cases), profile(cases))
            path.write_text(path.read_text().replace('"schema":', '"id":"duplicate", "schema":'))
            with self.assertRaises(native_batch.NativeBatchError):
                native_batch.load_profile(path, cases)
            path.write_bytes(b" " * (native_batch.MAX_PROFILE_BYTES + 1))
            with self.assertRaises(native_batch.NativeBatchError):
                native_batch.load_profile(path, cases)
            link = root / "linked.json"
            link.symlink_to(path)
            with self.assertRaises((native_batch.NativeBatchError, OSError)):
                native_batch.load_profile(link, cases)
            pipe = root / "profile.fifo"
            os.mkfifo(pipe)
            with self.assertRaises(native_batch.NativeBatchError):
                native_batch.load_profile(pipe, cases)


if __name__ == "__main__":
    unittest.main()
