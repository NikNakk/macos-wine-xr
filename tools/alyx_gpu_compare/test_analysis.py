# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""python3 -m unittest discover -s tools/alyx_gpu_compare"""
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import analysis
import compare

XML = b"""<?xml version="1.0"?><trace-query-result><node><schema name="metal-gpu-intervals">
<col><mnemonic>start</mnemonic></col><col><mnemonic>duration</mnemonic></col>
<col><mnemonic>process</mnemonic></col><col><mnemonic>channel-name</mnemonic></col></schema>
<row><start-time id="1" fmt="00:00.000">0</start-time><duration id="2" fmt="4 ms">4000000</duration>
<process id="3" fmt="hlvr.exe (812)">812</process><string id="4" fmt="Fragment">Fragment</string></row>
<row><start-time fmt="x">2000000</start-time><duration fmt="x">4000000</duration><process ref="3"/><string ref="4"/></row>
<row><start-time fmt="x">10000000</start-time><duration fmt="x">1000000</duration>
<process fmt="monado-service (99)">99</process><sentinel/></row>
<row><start-time fmt="x">1000000000</start-time><duration fmt="x">0</duration><process ref="3"/><string ref="4"/></row>
</node></trace-query-result>"""

HEADER = ('event,session_id,client_frame_id,event_ns,wake_ns,predicted_display_ns,predicted_period_ns,'
          'display_time_ns,cpu_est_ns,draw_est_ns,gpu_est_ns,cpu_actual_ns,draw_actual_ns,gpu_actual_ns\n')


def pacing_csv(frames, session=1, period_ns=16_666_667, start=1_000_000_000):
    lines = [HEADER]
    for i, refreshes in enumerate(frames):
        t = start + sum(frames[:i]) * period_ns
        lines.append(f'delivered,{session},{i},{t},0,0,{period_ns},{t + 3 * period_ns},0,0,0,0,0,0\n')
        lines.append(f'gpu_done,{session},{i},{t + 1},0,0,{period_ns},0,0,0,0,0,0,{12_000_000}\n')
    return ''.join(lines)


class UnionTest(unittest.TestCase):
    def test_overlap_nesting_and_gaps(self):
        self.assertEqual(analysis.union_ns([(0, 4), (2, 6), (3, 5), (10, 11), (11, 12), (20, 20)]), 8)
        self.assertEqual(analysis.union_ns([]), 0)


class TraceTest(unittest.TestCase):
    def test_refs_and_busy_per_process(self):
        per = analysis.gpu_busy_by_process(analysis.parse_xctrace_xml(XML))
        self.assertEqual(per['hlvr.exe (812)']['busy_ns'], 6_000_000)
        self.assertEqual(per['hlvr.exe (812)']['intervals'], 3)
        self.assertEqual(per['monado-service (99)']['busy_ns'], 1_000_000)
        hits, busy = analysis.match_process(per, 'HLVR')
        self.assertEqual(list(hits), ['hlvr.exe (812)'])
        self.assertEqual(busy, 6_000_000)
        self.assertEqual(analysis.trace_span_ns(per), 1_000_000_000)


class PacingTest(unittest.TestCase):
    def test_window_session_and_refreshes(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'p.csv'
            # Session 1: alternate 2 and 3 refreshes at 60 Hz; session 2 has fewer frames.
            path.write_text(pacing_csv([2, 3] * 30) + pacing_csv([1] * 5, session=2)[len(HEADER):] +
                            'delivered,1,9\n')  # truncated row at service exit
            s = analysis.pacing_summary(analysis.read_app_pacing(path), 0, 10**12, 16_666_667)
        self.assertEqual(s['session_id'], 1)
        self.assertEqual(s['frames'], 60)
        self.assertEqual(s['refreshes_per_frame'], {2: 30, 3: 29})
        self.assertEqual(s['monado_gpu_actual_ms_p50'], 12.0)
        self.assertAlmostEqual(s['interval_ms_p50'], 33.33, places=1)  # 30 of 59 intervals are two refreshes
        self.assertAlmostEqual(s['interval_ms_p90'], 50.0, places=1)

    def test_window_excludes_outside_frames(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'p.csv'
            path.write_text(pacing_csv([1] * 120))
            start = 1_000_000_000 + 60 * 16_666_667
            s = analysis.pacing_summary(analysis.read_app_pacing(path), start, start + 10**9, 16_666_667)
        self.assertEqual(s['frames'], 60)
        self.assertAlmostEqual(s['fps'], 60, places=0)

    def test_percentile(self):
        self.assertEqual(analysis.percentile([3, 1, 2], .5), 2)
        self.assertIsNone(analysis.percentile([], .5))


TOC = b"""<?xml version="1.0"?><trace-toc><run number="1"><info><summary>
<start-date>2026-10-26T12:33:55.372Z</start-date><end-date>2026-10-26T12:34:06.314Z</end-date>
<duration>10.942044</duration></summary></info></run></trace-toc>"""


class WindowTest(unittest.TestCase):
    def test_trace_start_after_slow_xctrace_startup(self):
        import datetime
        wall = datetime.datetime(2026, 10, 26, 12, 33, 0, tzinfo=datetime.timezone.utc).timestamp()
        begin, end = analysis.window_from_toc(TOC, [wall, 10**12])
        self.assertAlmostEqual(begin, 10**12 + 55.872e9, delta=1e3)  # trace start + 0.5 s trim
        self.assertAlmostEqual((end - begin) / 1e9, 9.942044, places=6)

    def test_rejects_implausible_start(self):
        import datetime
        wall = datetime.datetime(2026, 10, 27, 12, 0, tzinfo=datetime.timezone.utc).timestamp()
        self.assertIsNone(analysis.window_from_toc(TOC, [wall, 0]))
        self.assertIsNone(analysis.window_from_toc(b'<trace-toc/>', [wall, 0]))


class RunTest(unittest.TestCase):
    def test_analyze_run_gpu_per_frame(self):
        with tempfile.TemporaryDirectory() as d:
            run = Path(d)
            (run / 'timing').mkdir()
            (run / 'metal.trace').mkdir()
            (run / 'timing/monado_psvr2_5_app_pacing.csv').write_text(pacing_csv([2] * 31))
            (run / 'timing/monado_psvr2_6_app_pacing.csv').write_text(HEADER)
            (run / 'run.json').write_text(json.dumps(dict(
                name='01-wine-dxmt', variant='wine-dxmt', order=1, problems=[],
                capture_uptime_ns=[1_000_000_000, 1_000_000_000 + 10**9])))
            with mock.patch.object(analysis, 'xctrace_rows', lambda trace, schema: analysis.parse_xctrace_xml(XML)):
                r = analysis.analyze_run(run, 'hlvr', 60)
        self.assertEqual(r['pacing']['frames'], 30)
        self.assertAlmostEqual(r['game_gpu_busy_ms_per_s'], 6.0)
        self.assertAlmostEqual(r['game_gpu_ms_per_frame'], 0.2)
        # No clock pair in this record, so the runner's wider window is used and noted.
        self.assertEqual(r['problems'], ['pacing window from the runner, not the trace (wider than the trace)'])
        summary = analysis.summarize([r])
        self.assertIn('| wine-dxmt | 1 |', analysis.markdown(summary, [r]))

    def test_missing_trace_is_reported(self):
        with tempfile.TemporaryDirectory() as d:
            run = Path(d)
            (run / 'run.json').write_text(json.dumps(dict(name='x', variant='native-mvk', order=2)))
            r = analysis.analyze_run(run, 'alyx_host', 120)
        self.assertIn('no metal.trace', r['problems'])
        self.assertIsNone(r['game_gpu_ms_per_frame'])


class CondenseTest(unittest.TestCase):
    def test_condensed_intervals_give_the_same_busy_time_and_trace_is_removed(self):
        import datetime
        with tempfile.TemporaryDirectory() as d:
            run = Path(d)
            (run / 'metal.trace').mkdir()
            (run / 'metal.trace/data').write_bytes(b'x' * 10)
            wall = datetime.datetime(2026, 10, 26, 12, 33, 0, tzinfo=datetime.timezone.utc).timestamp()
            toc = mock.Mock(stdout=TOC)
            with mock.patch.object(analysis, 'xctrace_rows', lambda t, s: analysis.parse_xctrace_xml(XML)), \
                    mock.patch.object(analysis.subprocess, 'run', return_value=toc):
                self.assertEqual(analysis.condense_trace(run, [wall, 10**12]), [])
            self.assertFalse((run / 'metal.trace').exists())
            info = json.loads((run / analysis.TRACE_INFO_FILE).read_text())
            self.assertEqual(info['intervals'], 4)
            self.assertEqual(info['trace_bytes'], 10)
            per = analysis.gpu_busy_by_process(analysis.read_intervals_file(run / analysis.INTERVALS_FILE))
        self.assertEqual(per, analysis.gpu_busy_by_process(analysis.parse_xctrace_xml(XML)))

    def test_keep_trace(self):
        with tempfile.TemporaryDirectory() as d:
            run = Path(d)
            (run / 'metal.trace').mkdir()
            with mock.patch.object(analysis, 'xctrace_rows', lambda t, s: analysis.parse_xctrace_xml(XML)), \
                    mock.patch.object(analysis.subprocess, 'run', return_value=mock.Mock(stdout=TOC)):
                analysis.condense_trace(run, None, keep_trace=True)
            self.assertTrue((run / 'metal.trace').exists())


class GameStateTest(unittest.TestCase):
    def test_prepare_and_restore_survive_a_new_runner(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            hlvr, backup, keep = d / 'hlvr', d / 'backup', d / 'keep'
            (hlvr / 'save').mkdir(parents=True)
            (hlvr / 'cfg').mkdir()
            backup.mkdir()
            keep.mkdir()
            (hlvr / 'save/own.sav').write_text('mine')
            (hlvr / 'cfg/machine_convars_vr.vcfg').write_text('volume 1')
            (backup / 'autosave.sav').write_text('test')
            override = hlvr / compare.OVERRIDE_CSV
            state = compare.GameState(hlvr, backup, keep, [override])
            state.prepare()
            override.write_text('scale 0.5')
            (hlvr / 'cfg/machine_convars_vr.vcfg').write_text('volume 0')
            self.assertEqual(sorted(p.name for p in (hlvr / 'save').iterdir()), ['autosave.sav'])
            # A second run in the same comparison must not overwrite the kept originals.
            compare.GameState(hlvr, backup, keep, [override]).prepare()
            # Restore from a fresh object, as after the runner was killed.
            self.assertTrue(compare.GameState(hlvr, backup, keep, [override]).restore())
            self.assertEqual(sorted(p.name for p in (hlvr / 'save').iterdir()), ['own.sav'])
            self.assertEqual((hlvr / 'cfg/machine_convars_vr.vcfg').read_text(), 'volume 1')
            self.assertFalse(override.exists())


class OrderTest(unittest.TestCase):
    def test_alternating(self):
        self.assertEqual(compare.run_order(['a', 'b'], 2), ['a', 'b', 'b', 'a'])
        self.assertEqual(compare.run_order(['a', 'b', 'c'], 3), ['a', 'b', 'c', 'c', 'b', 'a', 'a', 'b', 'c'])

    def test_native_disables_fdm_and_wine_adds_vulkan(self):
        args = mock.Mock(fidelity=3, save_name='autosave', game_arg=[], render_scale=0.5,
                         render_scale_native=None, render_scale_wine=None)
        native = compare.game_args(args, 'native-mvk')
        vulkan = compare.game_args(args, 'wine-vulkan')
        self.assertIn('+vr_fdm', native)
        self.assertNotIn('+vr_fdm', vulkan)
        self.assertEqual(vulkan[-1], '-vulkan')
        # With a render scale, level 0 of the one-level override table is used.
        self.assertEqual(native[:5], ['-novid', '+vr_fidelity_level_auto', '0', '+vr_fidelity_level', '0'])
        self.assertIn('+vr_symmetric_fov', vulkan)
        # Windows needs the start-up map queued before the save, or it discards the save.
        self.assertLess(vulkan.index('startup'), vulkan.index('+load'))
        self.assertNotIn('startup', native)

    def test_render_scale_choice_and_override_table(self):
        args = mock.Mock(fidelity=3, render_scale=0.5, render_scale_native=0.4, render_scale_wine=None)
        self.assertEqual(compare.render_scale(args, 'native-mvk'), 0.4)
        self.assertEqual(compare.render_scale(args, 'wine-dxmt'), 0.5)
        args.render_scale = -1
        self.assertIsNone(compare.render_scale(args, 'wine-dxmt'))
        wine = compare.game_args(mock.Mock(fidelity=3, save_name='a', game_arg=[], render_scale=-1,
                                           render_scale_native=None, render_scale_wine=None), 'wine-dxmt')
        self.assertEqual(wine[wine.index('+vr_fidelity_level') + 1], '3')
        table = compare.override_csv(0.5, 2).splitlines()
        self.assertEqual(table[1], '0,1,2,0.5,,,,')
        self.assertEqual(table[-1], 'end,,,,,,,')

    def test_submitted_sizes_in_window(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'c.csv'
            path.write_text('event_ns,source_width,source_height\n5,0,0\n10,1344,1372\n20,1344,1372\n'
                            '30,2800,2856\n99,1,1\n')
            self.assertEqual(analysis.submitted_sizes([path], 0, 50), {'1344x1372': 2, '2800x2856': 1})


if __name__ == '__main__':
    unittest.main()
