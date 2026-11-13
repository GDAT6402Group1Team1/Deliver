"""用确定的收包时间检查缓冲计算，实际 HTTP 预检由 probe_http_speech 默认模式执行"""
import unittest
import threading
import time

from tts_evaluation.probe_http_speech import buffering_metrics, TraceSynth


def parts(*items):
    return [{'received_ms': time, 'duration_s': duration} for time, duration in items]


class HttpSpeechProbeMetricsTests(unittest.TestCase):
    def test_voice_name_is_captured_on_synthesis_thread(self):
        class ThreadVoice:
            def __init__(self): self.completed = threading.local()
            @property
            def voice_name(self): return getattr(self.completed, 'name', 'Uncle_Fu')
            def synthesize(self, *args):
                self.completed.name = 'Xiaofang_B'
                return b'\1\0', 24000
            def synthesize_stream(self, text, voice, emotion, cancel, on_chunk, chunk_frames=8):
                on_chunk(b'\1\0')
                self.completed.name = 'Xiaofang_B'
                return 2, 24000
        for mode in ('whole', '8'):
            with self.subTest(mode=mode):
                traced = TraceSynth(ThreadVoice())
                traced.row = {'voice_name': 'unknown', 'server_pcm_chunks': []}
                traced.mode, traced.origin = mode, time.perf_counter()
                def work():
                    args = ('你先说。', 'female', 'Calm', threading.Event())
                    if mode == 'whole': traced.synthesize(*args)
                    else: traced.synthesize_stream(*args, lambda _: None)
                thread = threading.Thread(target=work)
                thread.start(); thread.join(1)
                self.assertFalse(thread.is_alive())
                self.assertEqual(traced.voice_name, 'Uncle_Fu')
                self.assertEqual(traced.row['voice_name'], 'Xiaofang_B')

    def test_full_sentence_starts_when_received(self):
        result = buffering_metrics(parts((2000, 2)), 2001)
        self.assertEqual(result['retrospective_uninterrupted_start_ms'], 2000)
        self.assertEqual(result['buffer_064_start_ms'], 2000)
        self.assertEqual(result['simulated_total_stall_ms'], 0)

    def test_short_sentence_waits_for_end_notification(self):
        result = buffering_metrics(parts((400, .32)), 500)
        self.assertEqual(result['buffer_064_start_ms'], 500)
        self.assertEqual(result['retrospective_uninterrupted_start_ms'], 400)
        self.assertEqual(result['simulated_total_stall_ms'], 0)

    def test_late_next_chunk_requires_later_continuous_start(self):
        result = buffering_metrics(parts((500, .64), (1300, .64)), 1400)
        self.assertAlmostEqual(result['buffer_064_min_margin_ms'], -160)
        self.assertAlmostEqual(result['buffer_064_max_missing_ms'], 160)
        self.assertAlmostEqual(result['retrospective_uninterrupted_start_ms'], 660)
        self.assertAlmostEqual(result['simulated_total_stall_ms'], 160)

    def test_resume_waits_until_buffer_is_large_enough(self):
        result = buffering_metrics(parts((500, .64), (1300, .16), (1500, .16)), 1600)
        self.assertAlmostEqual(result['simulated_total_stall_ms'], 360)
        self.assertEqual(len(result['simulated_stalls']), 1)

    def test_complete_tail_can_resume_below_resume_buffer(self):
        result = buffering_metrics(parts((500, .64), (1300, .08)), 1350)
        self.assertAlmostEqual(result['simulated_total_stall_ms'], 210)
        self.assertTrue(result['simulation_only_not_ue_playback'])

    def test_audio_end_delay_does_not_invent_an_audible_stall(self):
        result = buffering_metrics(parts((500, .64)), 2000)
        self.assertEqual(result['simulated_total_stall_ms'], 0)
        self.assertIsNone(buffering_metrics([], 10))


if __name__ == '__main__':
    unittest.main()
