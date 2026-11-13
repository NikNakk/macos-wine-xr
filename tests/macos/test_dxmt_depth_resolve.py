import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('resolve',Path(__file__).resolve().parents[2]/'tools/analyze_dxmt_depth_resolve.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class Tests(unittest.TestCase):
    def rows(self):
        return ['TBDR pipeline encoder=20 frame=1 vs=abc ps=def colors=0 samples=1 depth_enable=1 depth_write=1 depth_func=8 stencil_enable=0',
                'TBDR srv encoder=20 frame=1 stage=1 slot=0 resource=source allocation=a native=separate_view',
                'TBDR attachment encoder=10 frame=1 role=depth_stencil samples=2 resource=source allocation=a native=base width=100 height=50 layers=1',
                'TBDR attachment encoder=20 frame=1 role=depth_stencil samples=1 resource=dest allocation=b native=destination width=100 height=50 layers=1',
                'TBDR render encoder=20 frame=1 label=R draws=1 d1 i0',
                'TBDR attachment encoder=30 frame=1 role=depth_stencil samples=2 resource=source allocation=a native=base width=100 height=50 layers=1']
    def test_joins_depth_view_by_allocation_and_preserves_later_use(self):
        c=module.analyze(self.rows())['candidates'];self.assertEqual(len(c),1)
        self.assertEqual(c[0]['later_source_attachment_encoders'],[30]);self.assertTrue(c[0]['same_dimensions'])
    def test_reused_resource_with_different_allocation_does_not_match(self):
        rows=self.rows();rows[1]=rows[1].replace('allocation=a','allocation=other')
        self.assertFalse(module.analyze(rows)['candidates'])
    def test_stencil_or_multiple_draws_are_rejected(self):
        for old,new in [('stencil_enable=0','stencil_enable=1'),('draws=1 d1','draws=2 d2')]:
            self.assertFalse(module.analyze([s.replace(old,new) for s in self.rows()])['candidates'])
if __name__=='__main__':unittest.main()
