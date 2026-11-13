import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('hdr',Path(__file__).resolve().parents[2]/'tools/analyze_dxmt_hdr_joined.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
class Tests(unittest.TestCase):
    def text(self):
        rows=''.join(f'HDR_JOINED_CONTROL mode={m} after={a} rate=0.5 depth_bad=0 stencil_bad=0 mapped_bad=0 timed_consumer_bad=0 final_bad={int(m!=0)} center_bad=0 consumer_samples=10 expected_consumer_samples=10 peak_hdr=3 mae={0 if m==0 else .01}\n' for m in range(3) for a in range(2))
        rows+=''.join(f'HDR_JOINED_SAMPLE mode={m} round={i} warmup={int(i<24)} gpu_ms={1 if m==0 else .7 if m==1 else .69} cpu_record_ms=.01\n' for m in range(3) for i in range(65))
        return rows+'HDR_JOINED replay_output_bad=0 baseline_gpu_ms=1 full_replay_gpu_ms=.7 coverage_replay_gpu_ms=.69\n'
    def test_full_sequence_savings_and_replay_contrast(self):
        r=module.analyze(self.text());self.assertAlmostEqual(r['paired_saving_ms']['1']['median'],.3);self.assertAlmostEqual(r['full_minus_coverage_ms']['median'],.01);self.assertAlmostEqual(r['paired_saving_percent']['1']['median'],30)
    def test_stencil_and_consumer_failures_are_rejected(self):
        for a,b in [('stencil_bad=0','stencil_bad=1'),('timed_consumer_bad=0','timed_consumer_bad=1'),('consumer_samples=10','consumer_samples=0'),('replay_output_bad=0','replay_output_bad=1')]:
            with self.assertRaises(ValueError):module.analyze(self.text().replace(a,b))
    def test_incomplete_or_wrong_warmup_intervals_are_rejected(self):
        for text in [self.text()+'HDR_JOINED_SAMPLE mode=0 round=0 warmup=1 gpu_ms=1 cpu_record_ms=.01\n',self.text().replace('round=0 warmup=1','round=0 warmup=0')]:
            with self.assertRaises(ValueError):module.analyze(text)
    def source_text(self):
        text='\n'.join(line for line in self.text().splitlines() if 'mode=2' not in line)
        text=text.replace('HDR_JOINED replay_output_bad=0','HDR_JOINED source_materialized=1 replay_output_bad=0')
        text+='\n'+''.join(f'MSAA_SOURCE_CONTROL mode={m} after={a} source_bad=0 alias_bad=0 snapshot_bad=0 load_bad=0 modified_samples=10 preserved_samples=20\n' for m in range(2) for a in range(2))
        return text+'MSAA_SOURCE_NEGATIVE broadcast_bad=10 plane_witness=5\n'
    def test_two_mode_source_materialisation_requires_all_reuse_checks(self):
        r=module.analyze(self.source_text());self.assertEqual(len(r['source_controls']),4);self.assertIsNone(r['full_minus_coverage_ms'])
        for a,b in [('alias_bad=0','alias_bad=1'),('load_bad=0','load_bad=1'),('plane_witness=5','plane_witness=0')]:
            with self.assertRaises(ValueError):module.analyze(self.source_text().replace(a,b))
if __name__=='__main__':unittest.main()
