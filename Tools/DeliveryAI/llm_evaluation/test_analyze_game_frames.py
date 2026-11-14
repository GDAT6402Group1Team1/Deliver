"""验证 CSV 尾部表头、分位数和时间范围，不启动 UE 或模型"""
import gzip
from pathlib import Path
import tempfile
import unittest

from analyze_game_frames import analyze


class GameFrameAnalysisTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name)/'frames.csv'

    def capture(self, body):
        self.path.write_text(body, encoding='utf-8')
        return self.path

    def test_percentiles_use_each_frame_and_keep_recording_identity(self):
        path = self.capture('EVENTS,FrameTime\n,10\n,10\n,10\n,100\n'
                            'EVENTS,FrameTime\n[HasHeaderRowAtEnd],1,[Platform],Windows\n')
        result = analyze(path, 'gpu21', {'gpu_layers': '21'}, capture_start='2026-11-14T18:30:00+08:00')
        self.assertEqual(result['sample_count'], 4)
        self.assertEqual(result['frame_time_ms']['median'], 10)
        self.assertAlmostEqual(result['frame_time_ms']['p95'], 86.5)
        self.assertEqual(result['configuration']['gpu_layers'], '21')
        self.assertEqual(result['interval']['capture_start_utc'], '2026-11-14T10:30:00+00:00')
        self.assertEqual(result['source']['metadata']['platform'], 'Windows')
        self.assertTrue(result['source']['ue_completion_footer_present'])
        self.assertEqual(len(result['source_sha256']), 64)

    def test_final_header_can_introduce_column_and_missing_samples_are_reported(self):
        path = self.capture('EVENTS,GameThread\n,3\n,4,20\n,5,30\n,6,0\n'
                            'EVENTS,GameThread,FrameTime\n[HasHeaderRowAtEnd],1\n')
        result = analyze(path, 'baseline')
        self.assertEqual(result['sample_count'], 2)
        self.assertEqual(result['source']['data_rows'], 4)
        self.assertEqual(result['source']['missing_frame_indices'], [0])
        self.assertEqual(result['source']['zero_frame_indices'], [3])
        self.assertAlmostEqual(result['interval']['measured_duration_seconds'], .05)

    def test_time_window_uses_frame_start_and_excludes_end(self):
        path = self.capture('FrameTime\n10\n20\n30\n40\n')
        result = analyze(path, 'gpu14', start_seconds=.01, end_seconds=.06)
        self.assertEqual(result['sample_count'], 2)
        self.assertEqual(result['interval']['first_frame_index'], 1)
        self.assertEqual(result['interval']['last_frame_index'], 2)
        self.assertEqual(result['frame_time_ms']['median'], 25)

    def test_bracketed_event_is_a_frame_not_metadata(self):
        path = self.capture('EVENTS,FrameTime\n[model_started]##0.020,20\n,30\n'
                            'EVENTS,FrameTime\n[HasHeaderRowAtEnd],1\n')
        self.assertEqual(analyze(path, 'gpu21')['sample_count'], 2)

    def test_gzip_capture_is_supported(self):
        path = self.path.with_suffix('.csv.gz')
        with gzip.open(path, 'wt', encoding='utf-8') as stream:
            stream.write('FrameTime\n16.6\n17.0\n')
        self.assertEqual(analyze(path, 'gpu99')['sample_count'], 2)

    def test_invalid_or_absent_frame_data_is_rejected(self):
        for body in ('', 'FPS\n60\n', 'FrameTime\n', 'FrameTime\n0\n',
                     'FrameTime\nNaN\n', 'FrameTime\n-1\n', 'FrameTime\nmissing\n'):
            with self.subTest(body=body), self.assertRaises(ValueError):
                analyze(self.capture(body), 'baseline')
        with self.assertRaises(ValueError):
            analyze(self.capture('FrameTime\n10\n'), 'baseline', start_seconds=2)


if __name__ == '__main__':
    unittest.main()
