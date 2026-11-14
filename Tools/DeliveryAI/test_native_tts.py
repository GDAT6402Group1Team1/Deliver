"""检查原生回调的音频顺序、取消和采样设置，不加载真实模型"""
import base64
import array
import ctypes as C
import json
from pathlib import Path
import threading
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from native_tts_abi import TtsParams, Audio, VoiceReference
from native_tts_engine import NativeVoiceEngines
from neural_tts import NeuralTtsError
from native_tts import NativeQwenSpeechSynthesizer


class FakeApi:
    def __init__(self, frames, after_first=None):
        self.frames, self.after_first = frames, after_first
        self.freed = 0
        self.settings = None
        self.freed_contexts = []

    def qt_voice_ref_free(self, _):
        pass

    def qt_free(self, context):
        self.freed_contexts.append(context)

    def qt_tts_default_params(self, pointer):
        C.cast(pointer, C.POINTER(TtsParams)).contents.abi_version = 6

    def qt_audio_free(self, _):
        self.freed += 1

    def qt_last_error(self):
        return b'fake failure'

    def qt_synthesize(self, _, pointer, output):
        params = C.cast(pointer, C.POINTER(TtsParams)).contents
        self.settings = dict(temperature=params.temperature, subtalker_temperature=params.subtalker_temperature,
            top_k=params.top_k, subtalker_top_k=params.subtalker_top_k, seed=params.seed,
            speaker=params.speaker, ref_text=params.ref_text)
        for i, frames in enumerate(self.frames):
            if params.cancel(None):
                return -5
            samples = (C.c_float * (frames * 1920))(*([.2] * (frames * 1920)))
            if not params.on_chunk(samples, len(samples), None):
                return -5
            if i == 0 and self.after_first:
                self.after_first()
        return 0


class NativeTtsTests(unittest.TestCase):
    def engine(self, frames, after_first=None):
        try:
            import numpy as np
        except ImportError:
            self.skipTest('原生音频测试需要 NumPy；语音试验环境已安装')
        engine = NativeVoiceEngines.__new__(NativeVoiceEngines)
        engine.np = np
        engine.config = {'seed': 20261113, 'max_new_tokens': 256, 'synthesis_timeout_seconds': 20,
                         'max_pcm_bytes': 384000, 'voices': {'female': {'reference_text': '参考原话'}}}
        engine.api = FakeApi(frames, after_first)
        engine.contexts = {'male': 1, 'female': 2}
        engine.reference = VoiceReference()
        engine.voice_residency = 'both'
        engine._closed = False
        engine._reference_owned = True
        engine.identities = {'male': {'speaker': 'Uncle_Fu'}, 'female': {'speaker': 'Xiaofang_B'}}
        return engine

    def task(self, voice='male', chunk_frames=8):
        return SimpleNamespace(task_id='a' * 32, voice=voice,
            speaker='Uncle_Fu' if voice == 'male' else 'Xiaofang_B', text='你先听我说完。',
            chunk_frames=chunk_frames, cancel=threading.Event())

    def test_stream_is_sent_during_generation_and_preserves_order(self):
        messages, received = [], threading.Event()
        def receive(message):
            messages.append(message)
            received.set()
        engine = self.engine([1, 2, 4, 8], lambda: self.assertTrue(received.wait(1)))
        result = engine.synthesize(self.task(chunk_frames=4), threading.Event(), receive)
        self.assertEqual(result['type'], 'audio_end')
        self.assertEqual(result['total_pcm_bytes'], 15 * 3840)
        self.assertEqual(result['chunk_count'], 5)
        offset = 0
        for i, message in enumerate(messages):
            self.assertEqual((message['chunk_index'], message['offset_bytes']), (i, offset))
            pcm = base64.b64decode(message['pcm'])
            self.assertLessEqual(len(pcm), 4 * 3840)
            offset += len(pcm)
        self.assertAlmostEqual(engine.api.settings['temperature'], .9)
        self.assertAlmostEqual(engine.api.settings['subtalker_temperature'], .9)
        self.assertEqual(engine.api.settings['seed'], 20261113)
        self.assertEqual(engine.api.freed, 1)

    def test_cancel_discards_late_chunks_and_next_task_can_run(self):
        task = self.task()
        messages = []
        engine = self.engine([1, 8, 8], task.cancel.set)
        with self.assertRaisesRegex(NeuralTtsError, '^cancelled$'):
            engine.synthesize(task, threading.Event(), messages.append)
        self.assertLessEqual(len(messages), 1)
        engine.api.frames, engine.api.after_first = [1, 2], None
        result = engine.synthesize(self.task(), threading.Event(), messages.append)
        self.assertEqual(result['total_pcm_bytes'], 3 * 3840)
        self.assertEqual(messages[-1]['offset_bytes'], 3840)
        self.assertEqual(engine.api.freed, 2)

    def test_sender_failure_stops_native_generation(self):
        engine = self.engine([8] * 10)
        def fail(_):
            raise OSError('pipe closed')
        with self.assertRaisesRegex(NeuralTtsError, 'callback_failed'):
            engine.synthesize(self.task(), threading.Event(), fail)
        self.assertEqual(engine.api.freed, 1)

    def test_full_audio_and_female_reference(self):
        engine = self.engine([1, 2])
        result = engine.synthesize(self.task('female', 0), threading.Event(), None)
        self.assertEqual(result['type'], 'audio')
        self.assertEqual(len(base64.b64decode(result['pcm'])), 3 * 3840)
        self.assertIsNone(engine.api.settings['speaker'])
        self.assertEqual(engine.api.settings['ref_text'].decode(), '参考原话')

    def test_limit_never_reports_partial_sentence_as_complete(self):
        engine = self.engine([8] * 20)
        with self.assertRaisesRegex(NeuralTtsError, 'audio_too_long'):
            engine.synthesize(self.task(), threading.Event(), lambda _: None)

    def test_callback_size_is_checked_before_pointer_read(self):
        engine = self.engine([9])
        with self.assertRaisesRegex(NeuralTtsError, 'invalid_stream_size'):
            engine.synthesize(self.task(), threading.Event(), lambda _: None)

    def test_native_worker_has_distinct_identity_and_keeps_original_config(self):
        runtime = {'quantization': 'BF16', 'models': {
            name: {'sha256': name + '_hash'} for name in ('male', 'female', 'codec')}}
        with mock.patch('native_tts.load_runtime', return_value=runtime):
            engine = NativeQwenSpeechSynthesizer()
        command = engine._worker_command(Path('python.exe'))
        self.assertTrue(command[5].endswith('native_tts_worker.py'))
        self.assertIn('--runtime', command)
        for voice in ('male', 'female'):
            identity = engine.identities[voice]
            self.assertEqual(identity['runtime'], 'qwentts_cpp')
            self.assertNotIn('talker_mode', identity)
            self.assertNotIn('predictor_mode', identity)
            self.assertEqual(identity['gguf_sha256'], voice + '_hash')
        self.assertEqual(engine.config['voices']['male']['provider'], 'qwen')
        self.assertEqual(engine.config['voices']['female']['provider'], 'qwen_base')

    def test_converter_rejects_windows_mojibake_in_vocabulary(self):
        from tts_evaluation.prepare_native_qwen import verify_vocabulary
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'vocab.json').write_text(json.dumps({'Ċ': 0, '你好': 1}, ensure_ascii=False), encoding='utf-8')
            field = SimpleNamespace(parts=['Ċ'.encode(), '你好'.encode()], data=[0, 1])
            reader = SimpleNamespace(fields={'tokenizer.ggml.tokens': field})
            with mock.patch.dict('sys.modules', {'gguf': SimpleNamespace(GGUFReader=lambda _: reader)}):
                verify_vocabulary(path, path / 'test.gguf')
                field.parts[0] = '膴'.encode()
                with self.assertRaisesRegex(ValueError, 'vocabulary_changed'):
                    verify_vocabulary(path, path / 'test.gguf')

    def test_first_silent_callback_is_not_reported_as_audible(self):
        from tts_evaluation.compare_native_qwen import audio_onset
        chunks = [array.array('h', [0] * 1920).tobytes(), array.array('h', [1000] * 3840).tobytes()]
        result = audio_onset(chunks, [16, 47], 24000)
        self.assertEqual(result['first_chunk_rms'], 0)
        self.assertEqual(result['first_audible_audio_offset_ms'], 80)
        self.assertEqual(result['first_audible_delivered_ms'], 47)

    def test_repeated_close_releases_each_model_once(self):
        engine = self.engine([1])
        engine.close()
        engine.close()
        self.assertEqual(engine.api.freed_contexts, [1, 2])
        self.assertFalse(engine.contexts)


if __name__ == '__main__':
    unittest.main()
