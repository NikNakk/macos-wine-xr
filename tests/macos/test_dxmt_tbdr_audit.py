import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[2]/"tools/analyze_dxmt_tbdr_audit.py"
spec = importlib.util.spec_from_file_location("tbdr", path)
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def cb(seq, start, end, frame=10, present=0, queue="main", render=1, status=4):
    return (f"TBDR cb queue={queue} seq={seq} frame={frame} gpu_start_ns={start} gpu_end_ns={end} "
            f"status={status} present={present} render_after={render} load_proxy=100 store_proxy=200\n")


class Tests(unittest.TestCase):
    def test_interval_union_does_not_double_count_overlap(self):
        self.assertEqual(audit.union_ns([(100, 110), (108, 120), (130, 140)]), 30)
        result = audit.analyze([cb(1, 100, 110), cb(2, 108, 120, present=1)], 0, 100)
        frame = result["frames"][0]
        self.assertAlmostEqual(frame["cb_interval_union_ms"], 20/1e6)
        self.assertAlmostEqual(frame["cb_interval_sum_ms"], 22/1e6)
        self.assertEqual(frame["inter_cb_gap_ms"], 0)

    def test_gap_and_incomplete_frame_are_distinct(self):
        result = audit.analyze([cb(1, 100, 110), cb(2, 130, 140, present=1), cb(3, 150, 160, frame=11)], 0, 100)
        self.assertEqual(result["selected_frames"], 1)
        self.assertEqual(result["omitted_no_present"], 1)
        self.assertAlmostEqual(result["frames"][0]["inter_cb_gap_ms"], 20/1e6)

    def test_duplicate_and_other_queue_are_not_added_twice(self):
        row = cb(1, 100, 120, present=1, render=10)
        result = audit.analyze([row, row, cb(1, 100, 130, present=1, queue="aux", render=1)], 0, 100)
        self.assertEqual(result["duplicate_rows"], 1)
        self.assertEqual(result["primary_queue"], "main")
        self.assertEqual(result["frames"][0]["store_proxy"], 200)

    def test_failed_gpu_interval_is_excluded(self):
        result = audit.analyze([cb(1, 100, 120, present=1), cb(2, 120, 900, status=5)], 0, 100)
        self.assertEqual(result["invalid_gpu_intervals"], 1)
        self.assertAlmostEqual(result["frames"][0]["cb_interval_union_ms"], 20/1e6)


if __name__ == "__main__":
    unittest.main()
