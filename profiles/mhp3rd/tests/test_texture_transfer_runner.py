"""Retain model boundaries and exact coverage in transfer evidence."""
import importlib.util
from pathlib import Path
import sys
import unittest
TOOLS=Path(__file__).resolve().parents[1]/"tools";sys.path.insert(0,str(TOOLS))
SPEC=importlib.util.spec_from_file_location("transfer_runner",TOOLS/"check_texture_transfer.py")
tool=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(tool)


class TransferReportTests(unittest.TestCase):
    def setUp(self):
        self.report=dict(tool.EXPECTED,schema_version=1,scope="original_read_copy_transform_with_modeled_imports",
            success=True,full_ram_vram_cpu_compared=True,imports_modeled=True,event_scheduling_modeled=True,
            transform_worker_executed=True,game_executed=False,max_interpreter_slices=1485992)

    def test_complete_scope(self):tool.validate(self.report)

    def test_missing_coverage_or_fake_comparison(self):
        for field,value in tool.EXPECTED.items():
            for bad in (None,True,value+1):
                with self.subTest(field=field,bad=bad):
                    with self.assertRaises(ValueError):tool.validate(dict(self.report,**{field:bad}))

    def test_model_boundary_is_not_hardware_or_game_proof(self):
        for field,value in (("imports_modeled",False),("event_scheduling_modeled",False),("game_executed",True),
                ("full_ram_vram_cpu_compared",False),("transform_worker_executed",False),("scope","whole_game")):
            with self.assertRaises(ValueError):tool.validate(dict(self.report,**{field:value}))

    def test_unknown_claim_is_rejected(self):
        with self.assertRaises(ValueError):tool.validate(dict(self.report,actual_concurrent_scheduler=True))

    def test_budget_and_schema(self):
        for value in (None,0,True,2000001):
            with self.assertRaises(ValueError):tool.validate(dict(self.report,max_interpreter_slices=value))
        with self.assertRaises(ValueError):tool.validate(dict(self.report,schema_version=True))


if __name__=="__main__":unittest.main()
