"""Check that owner evidence cannot silently omit execution or claim loading."""
import importlib.util
from pathlib import Path
import sys
import unittest
TOOLS=Path(__file__).resolve().parents[1]/"tools"
sys.path.insert(0,str(TOOLS))
SPEC=importlib.util.spec_from_file_location("owner_runner",TOOLS/"check_texture_owner.py")
tool=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(tool)


class OwnerReportTests(unittest.TestCase):
    def setUp(self):
        self.report=dict(tool.EXPECTED,schema_version=1,scope="original_lobby_owner_construction",success=True,
            full_ram_vram_cpu_compared=True,resource_loader_executed=False,max_interpreter_slices=96567,
            max_aot_dispatches=1,executed_pages=[0x08804000,0x0A0E7000])

    def test_exact_scope(self): tool.validate(self.report)

    def test_omitted_or_invented_coverage(self):
        for field,value in tool.EXPECTED.items():
            for wrong in (None,True,0,value+1):
                with self.subTest(field=field,wrong=wrong):
                    with self.assertRaises(ValueError): tool.validate(dict(self.report,**{field:wrong}))

    def test_claims_and_identity(self):
        for field,value in (("schema_version",True),("success",False),("scope","whole_game"),
                ("full_ram_vram_cpu_compared",False),("resource_loader_executed",True)):
            with self.assertRaises(ValueError): tool.validate(dict(self.report,**{field:value}))

    def test_execution_budget_and_actual_overlay(self):
        for field,value in (("max_interpreter_slices",2000001),("max_aot_dispatches",0),
                ("executed_pages",[]),("executed_pages",[0x08804000]),
                ("executed_pages",[0x0A0E7001]),("executed_pages",[0x0A0E7000,0x0A0E7000]),
                ("executed_pages",[0x0A0E7000,0x0A0E8000])):
            with self.assertRaises(ValueError): tool.validate(dict(self.report,**{field:value}))


if __name__=="__main__": unittest.main()
