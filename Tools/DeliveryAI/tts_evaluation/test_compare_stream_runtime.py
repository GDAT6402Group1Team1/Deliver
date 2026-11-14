"""检查语音探针的缺块识别、缓冲模拟和源码隔离，不加载 GPU"""
import base64
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tts_evaluation.compare_stream_runtime import AudioTrace, compare_rows, load_variant, source_folder


def chunk(index, seconds=.64):
    size = round(seconds * 48000)
    return {'type': 'audio_chunk', 'chunk_index': index, 'offset_bytes': index * size,
            'sample_rate': 24000, 'pcm': base64.b64encode(b'\1\0' * (size // 2)).decode('ascii')}


class StreamRuntimeTests(unittest.TestCase):
    def test_slow_chunks_report_missing_audio_with_actual_buffer_size(self):
        trace = AudioTrace(0)
        with patch('tts_evaluation.compare_stream_runtime.time.perf_counter', side_effect=[1., 2., 4.]):
            for index in range(3):
                trace.receive(chunk(index))
        pcm, row = trace.finish({'type': 'audio_end', 'total_pcm_bytes': 92160, 'chunk_count': 3}, 4100)
        self.assertEqual(len(pcm), 92160)
        self.assertAlmostEqual(row['generation_rtf'], 4.1 / 1.92)
        simulation = row['buffer_simulation']
        self.assertEqual(simulation['start_buffer_s'], 1.28)
        self.assertAlmostEqual(simulation['buffer_start_ms'], 2000)
        self.assertAlmostEqual(simulation['simulated_total_stall_ms'], 720)
        self.assertTrue(simulation['simulation_only_not_ue_playback'])

    def test_duplicate_or_missing_chunk_is_not_measured_as_success(self):
        trace = AudioTrace(0)
        trace.receive(chunk(0))
        with self.assertRaises(ValueError):
            trace.receive(chunk(0))
        with self.assertRaises(ValueError):
            trace.receive(chunk(2))
        with self.assertRaises(ValueError):
            trace.finish({'type': 'audio_end', 'total_pcm_bytes': 61440, 'chunk_count': 2}, 100)

    def test_whole_audio_has_no_simulated_gap(self):
        trace = AudioTrace(0)
        result = {'type': 'audio', 'sample_rate': 24000, 'pcm': chunk(0)['pcm']}
        pcm, row = trace.finish(result, 2300)
        self.assertEqual(len(pcm), 30720)
        self.assertEqual(row['first_chunk_ms'], 2300)
        self.assertEqual(row['buffer_simulation']['simulated_total_stall_ms'], 0)

    def test_codec_mismatch_is_reported_even_if_text_and_seed_match(self):
        row = {'case': 'short', 'mode': '8', 'codec_sha256': 'old', 'pcm_sha256': 'old',
               'first_chunk_ms': 1000, 'complete_ms': 2000, 'variant': 'baseline'}
        current = dict(row, variant='current', codec_sha256='new', complete_ms=1000)
        comparison = compare_rows([row, current])[0]
        self.assertFalse(comparison['same_codec'])
        self.assertTrue(comparison['same_pcm'])
        self.assertEqual(comparison['complete_change_ms'], -1000)

    def test_loading_old_worker_preserves_live_stream_module(self):
        live = SimpleNamespace(marker='live')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'neural_audio_stream.py').write_text('marker = "baseline"\n', encoding='utf-8')
            (path / 'neural_tts_worker.py').write_text('import neural_audio_stream\n', encoding='utf-8')
            with patch.dict(sys.modules, {'neural_audio_stream': live}):
                worker = load_variant(source_folder(path), 'unit_fixture')
                self.assertEqual(worker.neural_audio_stream.marker, 'baseline')
                self.assertIs(sys.modules['neural_audio_stream'], live)


if __name__ == '__main__':
    unittest.main()
