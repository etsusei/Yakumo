"""Reject weak or broadened original-allocation evidence without private inputs."""
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("allocation_runner",
    Path(__file__).resolve().parents[1] / "tools/check_texture_allocation.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


class ReportTests(unittest.TestCase):
    def setUp(self):
        self.report = dict(tool.EXPECTED, schema_version=1,
            scope="original_heap_lifecycle_and_caller_tail", success=True,
            elf_sha256=tool.ELF_HASH, max_interpreter_slices=20000,
            full_ram_vram_cpu_compared=True, virtual_source_provider_executed=True,
            object_constructor_and_resource_loader_executed=False,
            live_allocation_authority_installed=False)

    def test_complete_scope(self):
        tool.validate(self.report)

    def test_missing_or_wrong_coverage(self):
        for field in tool.EXPECTED:
            for value in (None, 0, True, tool.EXPECTED[field]+1):
                with self.subTest(field=field, value=value):
                    with self.assertRaises(ValueError): tool.validate(dict(self.report, **{field:value}))

    def test_uncertified_or_broadened_scope(self):
        for field, value in (("elf_sha256", "0"*64), ("success", False),
                ("full_ram_vram_cpu_compared", False), ("virtual_source_provider_executed", False),
                ("object_constructor_and_resource_loader_executed", True),
                ("live_allocation_authority_installed", True), ("scope", "whole_game")):
            with self.subTest(field=field):
                with self.assertRaises(ValueError): tool.validate(dict(self.report, **{field:value}))

    def test_bounds_and_duplicate_fields(self):
        for value in (None, 0, True, 100001):
            with self.assertRaises(ValueError): tool.validate(dict(self.report,max_interpreter_slices=value))
        with self.assertRaises(ValueError): tool.unique_object([("success",True),("success",False)])


if __name__ == "__main__":
    unittest.main()
