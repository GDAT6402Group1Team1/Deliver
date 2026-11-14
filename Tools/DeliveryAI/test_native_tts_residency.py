"""用假的 C 接口检查模型切换和参考声音的寿命，不加载模型或显卡"""
import ctypes as C
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import threading
import unittest
from unittest import mock
import wave

from native_tts import load_runtime
from native_tts_abi import COMMIT, InitParams, VoiceReference
from native_tts_engine import NativeVoiceEngines
from neural_tts import NeuralTtsError
import test_native_tts as native_fixture


class LifecycleApi(native_fixture.FakeApi):
    def __init__(self):
        super().__init__([1])
        self.alive, self.order, self.next_handle = {}, [], 1
        self.max_alive = self.extract_count = self.reference_frees = self.synthesis_count = 0
        self.on_init = None
        self.wrong_type = None

    def qt_init_default_params(self, pointer):
        C.cast(pointer, C.POINTER(InitParams)).contents.abi_version = 6

    def qt_init(self, pointer):
        params = C.cast(pointer, C.POINTER(InitParams)).contents
        voice = Path(params.talker_path.decode()).stem
        handle = self.next_handle
        self.next_handle += 1
        self.alive[handle] = voice
        self.order.append(('init', voice, handle))
        self.max_alive = max(self.max_alive, len(self.alive))
        if self.on_init:
            self.on_init()
        return handle

    def qt_model_type(self, handle):
        if self.wrong_type == self.alive[handle]:
            return b'wrong_model'
        return b'custom_voice' if self.alive[handle] == 'male' else b'base'

    def qt_n_speakers(self, handle):
        return 1

    def qt_speaker_name(self, handle, index):
        return b'Uncle_Fu'

    def qt_extract_voice_ref(self, handle, samples, count, pointer):
        assert self.alive[handle] == 'female'
        self.extract_count += 1
        self.embedding = (C.c_float * 2)(.25, .5)
        self.codes = (C.c_int32 * 2)(7, 8)
        ref = C.cast(pointer, C.POINTER(VoiceReference)).contents
        ref.ref_spk_emb, ref.ref_spk_dim = self.embedding, 2
        ref.ref_codes, ref.ref_T, ref.num_codebooks = self.codes, 1, 2
        return 0

    def qt_free(self, handle):
        voice = self.alive.pop(handle)
        self.order.append(('free', voice, handle))
        super().qt_free(handle)

    def qt_voice_ref_free(self, pointer):
        self.reference_frees += 1
        C.memset(pointer, 0, C.sizeof(VoiceReference))

    def qt_synthesize(self, handle, pointer, output):
        assert handle in self.alive
        self.synthesis_count += 1
        return super().qt_synthesize(handle, pointer, output)


class NativeResidencyTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.path = Path(self.folder.name)
        reference = self.path / 'reference.wav'
        with wave.open(str(reference), 'wb') as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(24000)
            audio.writeframes(b'\0\0' * 1920)
        self.config = {'model_revision': 'male_revision', 'seed': 20261113, 'max_new_tokens': 256,
                       'synthesis_timeout_seconds': 20, 'max_pcm_bytes': 576000,
                       'voices': {'male': {'speaker': 'Uncle_Fu'},
                                  'female': {'speaker': 'Xiaofang_B', 'model_revision': 'female_revision',
                                             'reference_path': str(reference),
                                             'reference_sha256': hashlib.sha256(reference.read_bytes()).hexdigest(),
                                             'reference_text': '参考原话'}}}
        self.runtime = {'schema_version': 1, 'source_commit': COMMIT, 'device': 'CUDA0', 'quantization': 'Q8_0',
                        'library': {'path': 'fake.dll'}, 'dependencies': [],
                        'models': {v: {'path': v+'.gguf', 'source_revision': v+'_revision'}
                                   for v in ('male', 'female', 'codec')}}
        self.api = LifecycleApi()
        self.addCleanup(mock.patch.stopall)
        mock.patch('native_tts_engine.NativeLibrary', return_value=SimpleNamespace(lib=self.api)).start()
        mock.patch('native_tts_engine.project_path', side_effect=Path).start()
        mock.patch('native_tts_engine.native_identities', return_value={'male': {'speaker': 'Uncle_Fu'},
                                                                      'female': {'speaker': 'Xiaofang_B'}}).start()

    def task(self, voice='male'):
        return native_fixture.NativeTtsTests.task(self, voice, 0)

    def engine(self, mode='active'):
        self.runtime['voice_residency'] = mode
        engine = NativeVoiceEngines(self.config, self.runtime)
        self.addCleanup(engine.close)
        return engine

    def test_active_keeps_one_model_and_reuses_independent_reference(self):
        engine = self.engine()
        self.assertEqual([row[:2] for row in self.api.order], [('init', 'female'), ('free', 'female'), ('init', 'male')])
        reference_address = C.addressof(engine.reference.ref_spk_emb.contents)
        output = []
        for voice in ('male', 'male', 'female', 'male'):
            output.append(engine.synthesize(self.task(voice), threading.Event(), None)['pcm'])
        self.assertEqual(len(set(output)), 1)
        self.assertEqual(self.api.extract_count, 1)
        self.assertEqual(self.api.max_alive, 1)
        self.assertEqual(self.api.next_handle, 5)
        self.assertEqual(C.addressof(engine.reference.ref_spk_emb.contents), reference_address)
        engine.close()
        engine.close()
        self.assertEqual(self.api.reference_frees, 1)
        self.assertFalse(self.api.alive)
        self.assertEqual(len(set(self.api.freed_contexts)), 4)

    def test_default_both_keeps_both_models(self):
        engine = NativeVoiceEngines(self.config, self.runtime)
        self.addCleanup(engine.close)
        for voice in ('male', 'female', 'male'):
            engine.synthesize(self.task(voice), threading.Event(), None)
        self.assertEqual(self.api.max_alive, 2)
        self.assertEqual(self.api.next_handle, 3)

    def test_cancel_during_load_frees_new_handle_and_next_task_recovers(self):
        engine = self.engine()
        task = self.task('female')
        self.api.on_init = task.cancel.set
        with self.assertRaisesRegex(NeuralTtsError, '^cancelled$'):
            engine.synthesize(task, threading.Event(), None)
        self.assertEqual(self.api.synthesis_count, 0)
        self.assertFalse(engine.contexts)
        self.assertFalse(self.api.alive)
        self.assertEqual(self.api.reference_frees, 0)
        self.api.on_init = None
        engine.synthesize(self.task('female'), threading.Event(), None)
        self.assertEqual(self.api.synthesis_count, 1)
        self.assertEqual(self.api.extract_count, 1)

    def test_closed_or_cancelled_task_cannot_start_a_load(self):
        engine = self.engine()
        task = self.task('female')
        task.cancel.set()
        before = list(self.api.order)
        with self.assertRaisesRegex(NeuralTtsError, '^cancelled$'):
            engine.synthesize(task, threading.Event(), None)
        self.assertEqual(self.api.order, before)
        closed = threading.Event()
        self.api.on_init = closed.set
        with self.assertRaisesRegex(NeuralTtsError, '^cancelled$'):
            engine.synthesize(self.task('female'), closed, None)
        self.assertFalse(self.api.alive)
        self.assertEqual(self.api.synthesis_count, 0)
        engine.close()
        with self.assertRaisesRegex(NeuralTtsError, '^cancelled$'):
            engine.synthesize(self.task(), threading.Event(), None)

    def test_wrong_model_during_switch_is_freed_and_can_recover(self):
        engine = self.engine()
        self.api.wrong_type = 'female'
        with self.assertRaisesRegex(NeuralTtsError, 'model_type_changed'):
            engine.synthesize(self.task('female'), threading.Event(), None)
        self.assertFalse(self.api.alive)
        self.assertFalse(engine.contexts)
        self.api.wrong_type = None
        engine.synthesize(self.task('male'), threading.Event(), None)
        self.assertEqual(self.api.max_alive, 1)

    def test_reference_failure_releases_initial_model(self):
        self.config['voices']['female']['reference_sha256'] = 'wrong'
        with self.assertRaisesRegex(NeuralTtsError, 'clone_reference_changed'):
            self.engine()
        self.assertFalse(self.api.alive)

    def test_runtime_accepts_only_explicit_residency_modes(self):
        path = self.path / 'runtime.json'
        for mode in (None, 'both', 'active', 'other', '', 1):
            runtime = dict(self.runtime)
            if mode is not None:
                runtime['voice_residency'] = mode
            path.write_text(json.dumps(runtime), encoding='utf-8')
            with mock.patch('native_tts.project_path', side_effect=Path):
                if mode in (None, 'both', 'active'):
                    self.assertEqual(load_runtime(path), runtime)
                else:
                    with self.assertRaisesRegex(NeuralTtsError, 'invalid_runtime'):
                        load_runtime(path)


if __name__ == '__main__':
    unittest.main()
