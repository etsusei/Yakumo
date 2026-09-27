"""Synthetic preflight policy checks; no game process or C++ build is used."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import native_modes  # noqa: E402
import verify_pair_preflight  # noqa: E402


class PairPreflightModeTests(unittest.TestCase):
    def test_v2_baseline_seal_checks_all_nine_and_legacy_stays_five(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            baseline = root / "baseline"
            candidate = root / "candidate"
            baseline.write_bytes(b"synthetic-baseline")
            candidate.write_bytes(b"synthetic-candidate")
            audit = root / "audit.json"
            audit.write_text(json.dumps({
                "baseline_commit": "4292eb66ee66eab37c327575382d071addcf6249",
                "source_content_sha256": "a" * 64,
                "recording_revision": "source-sha256:" + "b" * 64,
            }))

            for schema, count in ((native_modes.V2_SCHEMA, 33), (None, 21)):
                with self.subTest(schema=schema):
                    switches = native_modes.fields(schema)

                    def fake_run(command, *, env, **_options):
                        role = "baseline" if Path(command[0]).name == "baseline" else "candidate"
                        if "MHP3RD_DATA_DIR" not in env:
                            return subprocess.CompletedProcess(command, 2, "", "isolated directory required")
                        if role == "baseline" and any(env.get(name, "off") != "off" for name in switches):
                            return subprocess.CompletedProcess(command, 2, "", "baseline sealed")
                        identity = {
                            "schema": "yakumo-test-preflight-v1",
                            "recorder_revision": "source-sha256:" + "b" * 64,
                            "configuration_sha256": ("c" if env.get("MHP3RD_UI_LANGUAGE") != "en" else "d") * 64,
                            "build_config_sha256": "e" * 64,
                            "gameplay_source_commit": (
                                "4292eb66ee66eab37c327575382d071addcf6249" if role == "baseline" else "f" * 40),
                            "baseline_provenance_sha256": "a" * 64 if role == "baseline" else "",
                            "baseline_sealed": role == "baseline",
                            "renderer_compiled": True,
                            "aot_probes_compiled": True,
                        }
                        if schema is not None:
                            identity["native_mode_schema"] = schema
                        return subprocess.CompletedProcess(command, 0, json.dumps(identity), "")

                    with mock.patch.object(verify_pair_preflight.subprocess, "run", side_effect=fake_run):
                        report = verify_pair_preflight.check_pair(baseline, candidate, audit)
                    self.assertEqual(len(report["checks"]), count)
                    self.assertEqual(report["binaries"]["baseline"]["preflight"].get("native_mode_schema"), schema)


if __name__ == "__main__":
    unittest.main()
