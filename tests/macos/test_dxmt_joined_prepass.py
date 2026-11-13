import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('joined',Path(__file__).resolve().parents[2]/'tools/analyze_dxmt_joined_prepass.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class Tests(unittest.TestCase):
    def text(self):
        controls=''.join(f'JOINED_CONTROL after={i} rate=0.5 baseline_bad=0 depth_bad=0 mapped_bad=0 center_bad=0 color_bad=2\n' for i in [0,1])
        samples=''.join(f'JOINED_SAMPLE mode={m} round={i} warmup={int(i<24)} gpu_ms={1.0 if m==0 else .5} cpu_record_ms=0.01\n' for m in [0,1] for i in range(65))
        return controls+samples+'JOINED_PREPASS native_reference_bad=0 baseline_gpu_ms=1.000000 foveated_gpu_ms=0.500000 size=100\n'
    def test_complete_sequence_samples_are_not_added_as_components(self):
        result=module.analyze_text(self.text());self.assertEqual(result['paired_saving_ms']['median'],.5);self.assertEqual(result['savings_percent'],50)
    def test_missing_duplicate_or_incorrect_warmup_is_rejected(self):
        text=self.text()
        for changed in [text.replace('JOINED_SAMPLE mode=0 round=0 warmup=1','JOINED_SAMPLE mode=0 round=0 warmup=0'),text+'JOINED_SAMPLE mode=0 round=0 warmup=1 gpu_ms=1 cpu_record_ms=0.01\n']:
            with self.assertRaises(ValueError):module.analyze_text(changed)
    def test_correctness_or_reported_median_failure_is_rejected(self):
        for a,b in [('native_reference_bad=0','native_reference_bad=1'),('depth_bad=0','depth_bad=1'),('baseline_gpu_ms=1.000000','baseline_gpu_ms=2.000000')]:
            with self.assertRaises(ValueError):module.analyze_text(self.text().replace(a,b))
if __name__=='__main__':unittest.main()
