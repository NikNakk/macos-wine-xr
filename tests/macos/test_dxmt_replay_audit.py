import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('audit',Path(__file__).resolve().parents[2]/'tools/analyze_dxmt_replay_audit.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
class Tests(unittest.TestCase):
    def test_write_mask_with_keep_is_not_a_write(self):
        r=dict(stencil_enable='1',stencil_write='240',dsv_flags='1',front_fail='1',front_zfail='1',front_pass='1',back_fail='1',back_zfail='1',back_pass='1');self.assertFalse(module.stencil_writes(r));r['front_pass']='3';self.assertTrue(module.stencil_writes(r));r['dsv_flags']='2';self.assertFalse(module.stencil_writes(r))
    def test_actual_stencil_comparison_requires_overlapping_mask(self):
        r=dict(stencil_enable='1',stencil_read='129',front_func='3',back_func='3');self.assertTrue(module.stencil_reads(r,240));self.assertFalse(module.stencil_reads(r,16));r['front_func']=r['back_func']='8';self.assertFalse(module.stencil_reads(r,240))
if __name__=='__main__':unittest.main()
