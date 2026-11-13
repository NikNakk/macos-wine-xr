#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest
spec = importlib.util.spec_from_file_location("scene_trace", Path(__file__).resolve().parents[2] / "tools/analyze_dxmt_scene_trace.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class SceneTraceTests(unittest.TestCase):
    def test_alias_provenance_and_order(self):
        rows = [
            'SCENE access encoder=1 stage=1 flags=3 resource=a allocation=b native=aa width=512 height=512 format=70 samples=1 layers=1',
            'SCENE render encoder=1 width=512 height=512 colors=1 samples=1 depth=0',
            'SCENE access encoder=2 stage=5 flags=1 resource=a allocation=b native=bb width=512 height=512 format=70 samples=1 layers=1',
            'SCENE upscale encoder=2 input=bb depth=0 motion=0 output=0',
            'SCENE access encoder=3 stage=1 flags=2 resource=a allocation=b native=bb width=512 height=512 format=70 samples=1 layers=1',
        ]
        result = module.summarize(rows)
        self.assertEqual(result['upscale_resources'][0]['render_producers'], [1])
        self.assertEqual(result['upscale_resources'][0]['prior_writer_found'], 1)

    def test_missing_and_truncated_capture(self):
        result = module.summarize(['unrelated', 'SCENE upscale encoder=1 input=aa depth=0 motion=0 output=bb', 'SCENE truncated limit=5'])
        self.assertTrue(result['truncated'])
        self.assertEqual(result['upscale_resources'], [])

    def test_msaa_and_mrt_are_observations(self):
        result = module.summarize(['SCENE render encoder=2 width=900 height=800 colors=4 samples=2 depth=aa'])
        self.assertEqual(result['render_profiles'][0]['samples'], 2)
        self.assertEqual(result['render_profiles'][0]['colors'], 4)
        self.assertNotIn('safe', result['render_profiles'][0])

if __name__ == '__main__':
    unittest.main()
