"""The combined village case binds vector discovery and renderer observation."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
import unittest


PROFILE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROFILE / "tools"))

import native_batch  # noqa: E402
import native_modes  # noqa: E402
import run_cases  # noqa: E402
from test_initial_case_pack import _chinese_entries, _panel_mappings  # noqa: E402


CATALOG = PROFILE / "testing/cases/render_discovery.json"
VECTOR_PROFILE = PROFILE / "testing/profiles/render_discovery_vectors_v1.json"
TEXTURE_PROFILE = PROFILE / "testing/profiles/render_discovery_texture_v1.json"
OLD_CATALOG = PROFILE / "testing/cases/vector_metrics_discovery.json"


def _canonical_hash(value: object) -> str:
    encoded = json.dumps(value, sort_keys=True, ensure_ascii=False,
                         separators=(",", ":"), allow_nan=False).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


class RenderDiscoveryCaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.catalog = run_cases.load_case_catalog(CATALOG)
        cls.vector_profile = native_batch.load_profile(VECTOR_PROFILE, cls.catalog)
        cls.texture_profile = json.loads(
            TEXTURE_PROFILE.read_text(encoding="utf-8"),
            object_pairs_hook=run_cases._unique_json_object,
            parse_constant=run_cases._reject_json_constant,
        )

    def test_one_finite_case_reuses_the_checked_village_route(self) -> None:
        self.assertEqual(len(self.catalog["cases"]), 1)
        case = self.catalog["cases"][0]
        old = run_cases.load_case_catalog(OLD_CATALOG)["cases"][0]
        self.assertEqual((case["id"], case["version"]), ("RENDER-DISCOVERY-01", 1))
        self.assertEqual(case["steps"][0], old["steps"][0])
        self.assertEqual(case["steps"][4:7], old["steps"][4:7])
        self.assertTrue(8 <= len(case["steps"]) <= 10)
        self.assertIn("settings unchanged", case["steps"][1])
        self.assertIn("do not repeat", case["steps"][2])
        self.assertIn("missing or incorrect textures", case["steps"][7])
        self.assertEqual(case["checkpoints"], ["render_observation_complete"])
        self.assertEqual(case["required_probes"], [])
        self.assertEqual(case["required_state_fields"], ["character_loaded"])
        self.assertTrue(case["human_acceptance"])

    def test_vector_and_renderer_profiles_bind_the_same_catalog(self) -> None:
        digest = _canonical_hash(self.catalog)
        self.assertEqual(self.vector_profile["schema"], "yakumo-native-batch-v2")
        self.assertEqual(self.vector_profile["case_catalog_sha256"], digest)
        self.assertEqual(self.vector_profile["required_native_entries"], [])
        for name in native_modes.LEGACY_FIELDS:
            self.assertEqual(self.vector_profile["candidate_modes"][name], "off")
        for name in native_modes.VECTOR_FIELDS:
            self.assertEqual(self.vector_profile["candidate_modes"][name], "verify")

        renderer = self.texture_profile
        self.assertEqual(set(renderer), {"schema", "id", "case_catalog_sha256",
                                         "candidate_mode", "minimum_decodes", "coverage_scope"})
        self.assertEqual(renderer["schema"], "yakumo-renderer-batch-v1")
        self.assertEqual(renderer["id"], "render-discovery-texture-v1")
        self.assertEqual(renderer["case_catalog_sha256"], digest)
        self.assertEqual(renderer["candidate_mode"], "verify")
        self.assertEqual(renderer["minimum_decodes"], 1)
        self.assertEqual(renderer["coverage_scope"], "run_total")

    def test_panel_has_chinese_for_every_case_string(self) -> None:
        mappings = _panel_mappings("case_text")
        checkpoints = _panel_mappings("checkpoint_text")
        translated = _chinese_entries()
        for case in self.catalog["cases"]:
            for literal in [case["title"], *case["steps"]]:
                self.assertEqual(mappings.get(literal), literal)
                self.assertIn(literal, translated)
                self.assertNotEqual(translated[literal], literal)
            for checkpoint in case["checkpoints"]:
                label = checkpoints.get(checkpoint)
                self.assertEqual(label, "Render observation complete")
                self.assertIn(label, translated)
                self.assertNotEqual(translated[label], label)


if __name__ == "__main__":
    unittest.main()
