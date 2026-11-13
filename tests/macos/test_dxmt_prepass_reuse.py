import importlib.util
from pathlib import Path
import tempfile
import unittest
spec=importlib.util.spec_from_file_location('prepass',Path(__file__).resolve().parents[2]/'tools/analyze_dxmt_prepass_reuse.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class Tests(unittest.TestCase):
    def text(self,rate=.5,bad=2):
        return 'PREPASS_CONTROL full_color_bad=0 mapped_color_bad=0 readonly_depth_bad=0\n'+''.join(f'PREPASS_REUSE filter={f} rate={rate} checked_samples=100 color_bad={bad} holes={bad} readonly_depth_bad=0 source_depth_bad=0\n' for f in range(3))+'TBDR cb mapped_render=144\n'
    def run_text(self,text):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'probe.log';p.write_text(text);return module.analyze(p)
    def test_reduced_rate_counterexample_is_reported_as_error_count(self):
        self.assertEqual(self.run_text(self.text())['rows'][0]['color_bad_percent'],2)
    def test_identity_and_reference_failure_cannot_be_accepted(self):
        for text in [self.text(1),self.text().replace('mapped_color_bad=0','mapped_color_bad=1')]:
            with self.assertRaises(ValueError):self.run_text(text)
    def test_missing_actual_map_or_lost_source_is_rejected(self):
        for text in [self.text().replace('mapped_render=144','mapped_render=120'),self.text().replace('source_depth_bad=0','source_depth_bad=1')]:
            with self.assertRaises(ValueError):self.run_text(text)
if __name__=='__main__':unittest.main()
