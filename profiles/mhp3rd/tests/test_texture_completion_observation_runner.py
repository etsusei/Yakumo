import importlib.util
import re
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
SPEC = importlib.util.spec_from_file_location(
    "check_texture_completion_observation", TOOLS / "check_texture_completion_observation.py")
assert SPEC and SPEC.loader
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


class CompletionRunnerTests(unittest.TestCase):
    def test_tracker_smoke_requires_real_writer_release(self):
        tool.validate_tracker_smoke({
            "schema_version": 1,
            "scope": "synthetic-g1c-completion-tracker-smoke",
            "success": True,
            "completion_started": 1,
            "completion_completed": 1,
            "live_writers": 0,
            "authority_failure": "None",
            "transfer_readiness": False,
            "source_completion_receipts": 0,
            "native_invocations": 0,
            "game_executed": False,
        })

    def test_completion_scope_rejects_missing_events(self):
        report = {
            "schema_version": 1,
            "scope": "original_g1c_inline_completion_observation",
            "success": True,
            "full_ram_vram_cpu_compared": True,
            "imports_modeled": True,
            "event_scheduling_modeled": True,
            "transform_worker_executed": True,
            "game_executed": False,
            "cases": 64,
            "completion_event_cases": 0,
            "completion_failed_cases": 0,
            "copy_worker_cases": 14,
            "transform_worker_cases": 56,
            "rounded_write_cases": 4,
        }
        with self.assertRaises(ValueError):
            tool.validate(report)

    def test_completion_scope_accepts_bounded_shape(self):
        tool.validate({
            "schema_version": 1,
            "scope": "original_g1c_inline_completion_observation",
            "success": True,
            "full_ram_vram_cpu_compared": True,
            "imports_modeled": True,
            "event_scheduling_modeled": True,
            "transform_worker_executed": True,
            "game_executed": False,
            "cases": 64,
            "completion_event_cases": 56,
            "completion_failed_cases": 0,
            "copy_worker_cases": 14,
            "transform_worker_cases": 56,
            "rounded_write_cases": 4,
        })

    def test_integration_scope_accepts_executable_owned_values(self):
        tool.validate_integration({
            "schema_version": 1,
            "scope": "compiled-g1c-texture-completion-integration",
            "success": True,
            "compiled_integration": True,
            "synthetic_completion_sequence_used": False,
            "fixture_supplied_successful_retirement": False,
            "transfer_readiness": False,
            "full_ram_vram_cpu_compared": True,
            "game_executed": False,
            "source_completion_receipts": 0,
            "named_cases": ["inline-verbatim", "inline-transform"],
            "writer_released": 1,
            "writer_retained": 1,
            "cpu_comparisons": 2,
            "ram_comparisons": 2,
            "vram_comparisons": 2,
        })

    def test_integration_scope_rejects_synthetic_or_missing_comparisons(self):
        report = {
            "schema_version": 1,
            "scope": "compiled-g1c-texture-completion-integration",
            "success": True,
            "compiled_integration": True,
            "synthetic_completion_sequence_used": False,
            "fixture_supplied_successful_retirement": False,
            "transfer_readiness": False,
            "full_ram_vram_cpu_compared": True,
            "game_executed": False,
            "source_completion_receipts": 0,
            "named_cases": ["inline-verbatim"],
            "writer_released": 1,
            "writer_retained": 0,
            "cpu_comparisons": 1,
            "ram_comparisons": 1,
            "vram_comparisons": 1,
        }
        with self.assertRaises(ValueError):
            tool.validate_integration(dict(report, synthetic_completion_sequence_used=True))
        with self.assertRaises(ValueError):
            tool.validate_integration(dict(report, writer_released=0))
        with self.assertRaises(ValueError):
            tool.validate_integration(dict(report, named_cases=[]))

    def test_integration_publication_copies_executable_fields(self):
        source = (TOOLS / "check_texture_completion_observation.py").read_text()
        for field in (
                "compiled_integration", "synthetic_completion_sequence_used",
                "fixture_supplied_successful_retirement", "transfer_readiness",
                "source_completion_receipts", "named_cases", "writer_released",
                "writer_retained", "cpu_comparisons", "ram_comparisons",
                "vram_comparisons"):
            self.assertRegex(source, rf'"{field}"\s*:\s*report\s*\[\s*"{field}"\s*\]')

    def test_integration_fixture_is_identity_only_and_runtime_arity_is_six(self):
        source = (TOOLS / "check_texture_completion_observation.py").read_text()
        self.assertIn('paths["integration_fixture"] = integration_fixture.resolve()', source)
        self.assertIn('str(raw), str(encoded), str(decoded)]', source)
        self.assertNotIn('str(decoded), *integration_args', source)
        integration = (Path(__file__).resolve().parents[1] / "tests" /
                       "texture_completion_integration_oracle.cpp").read_text()
        self.assertIn('require(argc == 7', integration)
        self.assertIn('report.json encoded decoded', integration)
        self.assertNotIn('argv[7]', integration)

    def test_cmake_keeps_integration_and_synthetic_smoke_separate(self):
        cmake = (Path(__file__).resolve().parents[1] / "CMakeLists.txt").read_text()
        integration_start = cmake.index(
            "add_executable(mhp3rd_texture_completion_integration_oracle")
        integration_end = cmake.index(
            "if(APPLE AND TARGET mhp3rd_read_prefix_generated", integration_start)
        integration = cmake[integration_start:integration_end]
        integration_guard = cmake[cmake.rfind("if(APPLE", 0, integration_start):integration_start]
        self.assertIn("EXCLUDE_FROM_ALL", integration)
        self.assertIn("mhp3rd_texture_completion_integration_sanitized", integration)
        self.assertEqual(integration.count("EXCLUDE_FROM_ALL"), 2)
        self.assertIn("TARGET mhp3rd_completion_generated", integration_guard)
        self.assertIn("texture_completion_decode.cpp", cmake)
        tracker_start = cmake.index(
            "add_library(mhp3rd_texture_transfer_tracker STATIC")
        tracker_end = cmake.index(")", tracker_start)
        tracker = cmake[tracker_start:tracker_end]
        self.assertEqual(tracker.count("texture_completion_decode.cpp"), 1)
        for target in (
                "mhp3rd_texture_transfer_tracker", "mhp3rd_texture_lifetime_tracker",
                "mhp3rd_texture_commands", "mhp3rd_texture_commands_bridge",
                "mhp3rd_source_authority"):
            self.assertIn(target, integration)
        self.assertNotIn("MHP3RD_TEXTURE_COMPLETION_TRACKER_SMOKE", integration)
        self.assertNotIn("target_link_libraries(Yakumo", integration)
        self.assertNotIn(
            "${MHP3RD_TEXTURE_COMPLETION_DECODER_SOURCE}", integration)
        smoke_start = cmake.index(
            "add_executable(mhp3rd_texture_completion_tracker_smoke")
        smoke_guard = cmake[cmake.rfind("if(", 0, smoke_start):smoke_start]
        self.assertIn("TARGET mhp3rd_read_prefix_generated", smoke_guard)
        self.assertIn("TARGET mhp3rd_texture_transfer_tracker", smoke_guard)


if __name__ == "__main__":
    unittest.main()
