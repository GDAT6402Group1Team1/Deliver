"""检查原生回调的音频顺序、取消和采样设置，不加载真实模型"""
import base64
import array
import ctypes as C
import hashlib
from io import StringIO
import json
from pathlib import Path
import threading
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import wave

from native_tts_abi import COMMIT, InitParams, NativeLibrary, TtsParams, Audio, VoiceReference
from native_tts_engine import NativeVoiceEngines
from neural_tts import NeuralTtsError
from native_tts import NativeQwenSpeechSynthesizer, load_runtime
from native_tts_worker import warm_up_voices


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

    def test_shared_codec_requires_explicit_flag_and_both_voices(self):
        runtime = {'schema_version': 1, 'source_commit': COMMIT, 'device': 'CUDA0', 'quantization': 'Q8_0',
                   'library': {'path': 'fake.dll'}, 'dependencies': [],
                   'models': {name: {'path': name + '.gguf'} for name in ('male', 'female', 'codec')}}
        with tempfile.TemporaryDirectory() as directory, mock.patch('native_tts.project_path', side_effect=Path):
            path = Path(directory) / 'runtime.json'
            for shared, residency, valid in ((True, 'both', True), (False, 'active', True),
                                             (True, 'active', False), ('true', 'both', False), (1, 'both', False)):
                with self.subTest(shared=shared, residency=residency):
                    settings = dict(runtime, shared_codec=shared, voice_residency=residency)
                    path.write_text(json.dumps(settings), encoding='utf-8')
                    if valid:
                        self.assertEqual(load_runtime(path), settings)
                    else:
                        with self.assertRaisesRegex(NeuralTtsError, 'native_tts_invalid_runtime'):
                            load_runtime(path)

    def test_old_library_works_until_shared_codec_is_enabled(self):
        api = mock.Mock()
        api.qt_version.return_value = COMMIT[:7].encode()
        api.qt_init_default_params.side_effect = lambda p: setattr(C.cast(p, C.POINTER(InitParams)).contents,
                                                                     'abi_version', 6)
        api.qt_tts_default_params.side_effect = lambda p: setattr(C.cast(p, C.POINTER(TtsParams)).contents,
                                                                    'abi_version', 6)
        del api.qt_init_shared_codec
        path = Path('old.dll').resolve()
        with mock.patch('native_tts_abi.C.CDLL', return_value=api):
            self.assertIs(NativeLibrary(path).lib, api)
            with self.assertRaisesRegex(ValueError, 'native_tts_shared_codec_unavailable'):
                NativeLibrary(path, shared_codec=True)
        api.qt_init.assert_not_called()

    def test_shared_codec_reuses_first_handle_and_releases_it_on_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            reference = path / 'reference.wav'
            with wave.open(str(reference), 'wb') as audio:
                audio.setparams((1, 2, 24000, 0, 'NONE', 'not compressed'))
                audio.writeframes(b'\0\0' * 1920)
            config = {'model_revision': 'test', 'voices': {'male': {'speaker': 'Uncle_Fu'},
                      'female': {'reference_path': str(reference),
                                 'reference_sha256': hashlib.sha256(reference.read_bytes()).hexdigest()}}}
            runtime = {'shared_codec': True, 'library': {'path': str(path / 'qwen.dll')},
                       'models': {name: {'path': str(path / f'{name}.gguf'), 'source_revision': 'test'}
                                  for name in ('male', 'female', 'codec')}}
            for female_handle in (20, None):
                with self.subTest(female_handle=female_handle):
                    api = mock.Mock()
                    api.qt_init.return_value = 10
                    api.qt_init_shared_codec.return_value = female_handle
                    api.qt_model_type.side_effect = [b'custom_voice', b'base']
                    api.qt_n_speakers.return_value = 1
                    api.qt_speaker_name.return_value = b'Uncle_Fu'
                    api.qt_extract_voice_ref.return_value = 0
                    with (mock.patch('native_tts_engine.NativeLibrary', return_value=SimpleNamespace(lib=api)),
                          mock.patch('native_tts_engine.native_identities', return_value={}),
                          mock.patch('native_tts_engine.project_path', side_effect=Path)):
                        if female_handle:
                            engine = NativeVoiceEngines(config, runtime)
                            engine.close()
                            engine.close()
                            self.assertEqual(api.qt_free.call_args_list, [mock.call(10), mock.call(20)])
                            api.qt_voice_ref_free.assert_called_once()
                        else:
                            with self.assertRaisesRegex(NeuralTtsError, 'native_tts_model_load_failed'):
                                NativeVoiceEngines(config, runtime)
                            api.qt_free.assert_called_once_with(10)
                            api.qt_voice_ref_free.assert_not_called()
                    api.qt_init.assert_called_once()
                    api.qt_init_shared_codec.assert_called_once()
                    self.assertEqual(api.qt_init_shared_codec.call_args.args[1], 10)

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

    def test_warmup_prepares_configured_voices_without_sending_audio(self):
        speakers = {'male': 'Uncle_Fu', 'female': 'Xiaofang_B'}
        for residency, expected in (('both', ['male', 'female']), ('active', ['male'])):
            with self.subTest(voice_residency=residency):
                closed = threading.Event()
                tasks = []

                def synthesize(task, stop, send):
                    tasks.append(task)
                    self.assertIs(stop, closed)
                    self.assertEqual(task.speaker, speakers[task.voice])
                    self.assertTrue(task.text.strip())
                    self.assertEqual(task.chunk_frames, 8)
                    self.assertFalse(task.cancel.is_set())
                    self.assertIsNot(task.cancel, closed)
                    self.assertIsNone(send({'type': 'audio_chunk', 'task_id': task.task_id,
                                            'pcm': base64.b64encode(b'\1\0' * 1920).decode()}))
                    return {'stream_metrics': {'first_audio_ms': 12, 'synthesis_ms': 34}}

                engine = SimpleNamespace(voice_residency=residency, synthesize=synthesize)
                with mock.patch('sys.stdout', new_callable=StringIO) as output:
                    with mock.patch('sys.stderr', new_callable=StringIO) as diagnostics:
                        self.assertIsNone(warm_up_voices(engine, speakers, closed))

                self.assertEqual([task.voice for task in tasks], expected)
                self.assertEqual(len({task.task_id for task in tasks}), len(tasks))
                self.assertEqual(len({id(task.cancel) for task in tasks}), len(tasks))
                self.assertEqual(output.getvalue(), '')
                self.assertEqual(diagnostics.getvalue().count('native tts voice_ready'), len(expected))
                self.assertFalse(closed.is_set())


if __name__ == '__main__':
    unittest.main()
