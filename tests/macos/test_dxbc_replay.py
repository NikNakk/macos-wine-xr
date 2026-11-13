import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('dxbc',Path(__file__).resolve().parents[2]/'tools/analyze_dxbc_replay.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
class Tests(unittest.TestCase):
    def test_discards_and_coverage_are_not_omitted(self):
        r=module.analyze('ps_5_0\ndcl_output oMask\ndiscard_nz r0.x\nret\n');self.assertEqual(r['discard_instructions'],1);self.assertTrue(r['coverage_output']);self.assertFalse(r['depth_output'])
    def test_conservative_depth_and_uav_flags(self):
        r=module.analyze('ps_5_0\ndcl_output_siv oDepthGE\ndcl_uav_raw u0\nstore_raw u0, r0, r1\n');self.assertTrue(r['depth_output']);self.assertTrue(r['uav_declared']);self.assertEqual(r['write_instructions'],{'store_raw':1})
    def test_numbered_disassembly_and_declarations(self):
        r=module.analyze('0: vs_5_0\n1: dcl_input v0.xy\n2: mov o0, v0\n3: ret\n');self.assertEqual(r['instruction_count'],2);self.assertEqual(r['profile'],'vs_5_0')
if __name__=='__main__':unittest.main()
