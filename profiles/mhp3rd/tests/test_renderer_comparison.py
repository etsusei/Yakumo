"""Renderer policy through real framed-package and comparison tooling."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_run_comparison as support
import compare_test_runs as compare
import native_modes
import renderer_batch
import run_package


class RendererComparisonTests(unittest.TestCase):
    def setUp(self):
        self.fixture = support.ComparisonTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.catalog = support.catalog()
        self.catalog["cases"][0]["required_probes"] = []
        self.profile = {"schema": renderer_batch.SCHEMA, "id": "renderer-integration",
                        "case_catalog_sha256": compare.canonical_hash(self.catalog),
                        "candidate_mode": "verify", "minimum_decodes": 1,
                        "coverage_scope": "run_total"}
        modes = {name: "off" for name in native_modes.V2_FIELDS}
        modes.update({name: "verify" for name in native_modes.VECTOR_FIELDS})
        self.vector_profile = {"schema": "yakumo-native-batch-v2", "id": "vector-integration",
                               "case_catalog_sha256": self.profile["case_catalog_sha256"],
                               "candidate_modes": modes, "required_native_entries": []}

    def pair(self, *, edit_candidate=None):
        paths = {}
        for role in ("baseline", "candidate"):
            def edit(rows, role=role):
                rows[:] = [(kind, fields) for kind, fields in rows
                           if fields.get("event") != "probe.summary"]
                begin = rows[0][1]
                context, _ = support.make_records(role, self.catalog)
                begin.update(native_mode_schema=native_modes.V2_SCHEMA,
                             texture_decode_schema="yakumo-texture-decode-v1",
                             texture_decode_mode="off" if role == "baseline" else self.profile["candidate_mode"],
                             renderer_profile_sha256=renderer_batch.profile_sha256(self.profile),
                             case_catalog_sha256=self.profile["case_catalog_sha256"],
                             prerequisite_basis_sha256=run_package.prerequisite_basis_sha256(context, "B0", "4292eb6"))
                begin.update({name: "off" for name in native_modes.V2_FIELDS} if role == "baseline"
                             else self.vector_profile["candidate_modes"])
                if role == "candidate":
                    def counter(total, final):
                        values = {name: 0 for name in renderer_batch.COUNTERS}
                        values.update(requests=total, immediate_requests=total,
                                      portable_success=total, portable_elapsed_ns=total * 20)
                        values["verified" if self.profile["candidate_mode"] == "verify" else "native"] = total
                        return (8, dict(values, event="texture_decode.counters",
                                        schema="yakumo-texture-decode-v1", mode=self.profile["candidate_mode"],
                                        scope="renderer_cache_miss_decodes_not_all_draws",
                                        final=final, workers_drained=final))
                    rows.insert(1, counter(7, False))  # Before CaseBegin: deliberately run-total coverage.
                    rows.insert(-1, counter(8, True))  # Before final observer health.
                    if edit_candidate:
                        edit_candidate(rows)
            paths[role] = self.fixture.packaged(role, cases=self.catalog, edit=edit,
                                               input_button=1 if role == "baseline" else 2)
            validation = run_package.load_package(paths[role])["validation"]
            self.assertTrue(validation["recording_complete"], validation)
        return paths

    def analyze(self, paths, *, renderer=True):
        return compare.compare_runs(paths["baseline"], paths["candidate"], self.catalog,
                                    self.vector_profile, self.profile if renderer else None)

    def test_optional_vector_zero_calls_do_not_hide_verified_renderer(self):
        report = self.analyze(self.pair())
        self.assertEqual(report["renderer_observation"]["outcome"], "verified", report["renderer_observation"])
        self.assertEqual(report["renderer_observation"]["candidate_counters"]["verified"], 8)
        self.assertEqual(report["renderer_observation"]["coverage_scope"], "run_total")
        self.assertEqual(report["profile_observations"]["cases"][0]["entries"][0]["coverage"], "not_covered")
        self.assertEqual(report["cases"][0]["input_alignment"], "different_observed_stream")
        self.assertIn("Texture decoding observations", compare.render_html(report))

    def test_renderer_mode_requires_explicit_profile_argument(self):
        report = self.analyze(self.pair(), renderer=False)
        self.assertEqual(report["outcome"], "incomparable")

    def test_profile_hash_mismatch_is_not_accepted(self):
        report = self.analyze(self.pair(edit_candidate=lambda rows: rows[0][1].update(renderer_profile_sha256="b" * 64)))
        self.assertEqual(report["renderer_observation"]["outcome"], "incomparable")

    def test_missing_final_record_prevents_overall_success(self):
        def remove_final(rows):
            rows[:] = [(kind, fields) for kind, fields in rows
                       if not (fields.get("event") == "texture_decode.counters" and fields["final"])]
        report = self.analyze(self.pair(edit_candidate=remove_final))
        self.assertEqual(report["renderer_observation"]["outcome"], "incomplete")
        self.assertEqual(report["outcome"], "incomplete")

    def test_reference_pixel_mismatch_reaches_top_level_report(self):
        def mismatch(rows):
            next(fields for _, fields in rows if fields.get("event") == "texture_decode.counters"
                 and fields["final"])["mismatches"] = 1
        report = self.analyze(self.pair(edit_candidate=mismatch))
        self.assertEqual(report["renderer_observation"]["outcome"], "confirmed_mismatch")
        self.assertEqual(report["outcome"], "confirmed_mismatch")

    def test_unexpected_helper_mode_invalidates_combined_policy(self):
        edit = lambda rows: rows[0][1].update(MHP3RD_NATIVE_VECTOR_NORM="native")
        report = self.analyze(self.pair(edit_candidate=edit))
        self.assertEqual(report["renderer_observation"]["outcome"], "incomparable")

    def test_native_texture_observation_never_claims_reference_match(self):
        self.profile["candidate_mode"] = "native"
        report = self.analyze(self.pair())
        result = report["renderer_observation"]
        self.assertEqual(result["outcome"], "native_observed", result)
        self.assertEqual(result["candidate_counters"]["verified"], 0)


if __name__ == "__main__":
    unittest.main()
