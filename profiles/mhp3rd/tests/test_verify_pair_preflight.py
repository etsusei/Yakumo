"""Synthetic process checks for the paired early-preflight validator."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
import native_modes  # noqa: E402
import texture_decode_policy  # noqa: E402
import verify_pair_preflight  # noqa: E402


class PairedPreflightTests(unittest.TestCase):
    def check_synthetic_pair(self, *, texture_schema: bool) -> dict:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            baseline = root / "baseline"
            candidate = root / "candidate"
            baseline.write_bytes(b"baseline")
            candidate.write_bytes(b"candidate")
            audit = {"baseline_commit": "4292eb66ee66eab37c327575382d071addcf6249",
                     "source_content_sha256": "a" * 64,
                     "recording_revision": "source-sha256:" + "b" * 64}
            audit_path = root / "audit.json"
            audit_path.write_text(json.dumps(audit))

            def run(command: list[str], **options: object) -> subprocess.CompletedProcess[str]:
                role = "baseline" if Path(command[0]).name == "baseline" else "candidate"
                env = options["env"]
                assert isinstance(env, dict)
                mode = env.get(texture_decode_policy.ENVIRONMENT, "off")
                rejected = "MHP3RD_DATA_DIR" not in env or any(
                    env.get(name, "off") != "off" for name in native_modes.V2_FIELDS
                ) and role == "baseline"
                if texture_schema:
                    rejected = rejected or mode not in texture_decode_policy.MODES or (
                        role == "baseline" and mode != "off")
                if rejected:
                    return subprocess.CompletedProcess(command, 1, "", "rejected")
                value = {
                    "schema": "yakumo-test-preflight-v1",
                    "recorder_revision": audit["recording_revision"],
                    "configuration_sha256": "c" * 64 if "MHP3RD_UI_LANGUAGE" not in env else "d" * 64,
                    "build_config_sha256": "e" * 64,
                    "gameplay_source_commit": audit["baseline_commit"] if role == "baseline" else "f" * 40,
                    "baseline_provenance_sha256": audit["source_content_sha256"] if role == "baseline" else "",
                    "baseline_sealed": role == "baseline",
                    "renderer_compiled": True,
                    "aot_probes_compiled": True,
                    "native_mode_schema": native_modes.V2_SCHEMA,
                }
                if texture_schema:
                    value[texture_decode_policy.SCHEMA_FIELD] = texture_decode_policy.SCHEMA
                    value[texture_decode_policy.MODE_FIELD] = mode
                return subprocess.CompletedProcess(command, 0, json.dumps(value), "")

            with mock.patch.object(verify_pair_preflight.subprocess, "run", side_effect=run):
                return verify_pair_preflight.check_pair(baseline, candidate, audit_path)

    def test_new_policy_rejects_baseline_and_invalid_candidate_modes(self):
        report = self.check_synthetic_pair(texture_schema=True)
        self.assertEqual(report["status"], "passed")
        self.assertEqual(report["binaries"]["baseline"]["preflight"][texture_decode_policy.MODE_FIELD], "off")
        checks = {(record["role"], record["check"]) for record in report["checks"]}
        self.assertIn(("baseline", "reject_texture_native"), checks)
        self.assertIn(("baseline", "reject_texture_invalid"), checks)
        self.assertIn(("candidate", "reject_texture_invalid"), checks)
        self.assertIn(("candidate", "accept_texture_native"), checks)

    def test_historical_preflight_without_policy_stays_valid(self):
        report = self.check_synthetic_pair(texture_schema=False)
        self.assertEqual(report["status"], "passed")
        self.assertNotIn(texture_decode_policy.SCHEMA_FIELD,
                         report["binaries"]["baseline"]["preflight"])


if __name__ == "__main__":
    unittest.main()
