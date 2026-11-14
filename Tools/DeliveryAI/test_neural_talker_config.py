"""检查 Talker Graph 必须明确启用，以及新旧声音进程不会混用"""
import copy
import json
import os
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from neural_tts import CONFIG_PATH, NeuralTtsError, load_voice_config, neural_voice_identities
from neural_tts_worker import QwenVoiceEngines, talker_generation


class NeuralTalkerConfigTests(unittest.TestCase):
    def setUp(self):
        # 各用例自己选择模式，正式配置与运行服务的环境不会改变测试分支
        self.fixture_config = json.loads(CONFIG_PATH.read_text(encoding='utf-8-sig'))
        self.fixture_config.pop('talker_mode', None)
        environment = patch.dict(os.environ, {}, clear=True)
        environment.start()
        self.addCleanup(environment.stop)
        source = patch('neural_tts.json.loads', side_effect=lambda _text: copy.deepcopy(self.fixture_config))
        source.start()
        self.addCleanup(source.stop)

    def test_missing_talker_mode_defaults_to_original_path(self):
        with patch.dict(os.environ, {}, clear=True):
            config = load_voice_config()
        self.assertEqual(config['talker_mode'], 'official')
        self.assertTrue(all('talker_mode' not in identity for identity in neural_voice_identities(config).values()))

    def test_config_can_enable_graph_without_environment_override(self):
        self.fixture_config['talker_mode'] = 'cuda_graph'
        config = load_voice_config()
        self.assertEqual(config['talker_mode'], 'cuda_graph')
        self.assertTrue(all(identity['talker_mode'] == 'cuda_graph'
                            for identity in neural_voice_identities(config).values()))

    def test_explicit_graph_mode_marks_both_voice_identities(self):
        with patch.dict(os.environ, {'DELIVERY_TTS_TALKER_MODE': 'cuda_graph'}):
            config = load_voice_config()
        identities = neural_voice_identities(config)
        self.assertEqual({entry['talker_mode'] for entry in identities.values()}, {'cuda_graph'})
        self.assertIn('reference_sha256', identities['female'])
        self.assertEqual(identities['male']['speaker'], 'Uncle_Fu')

    def test_environment_can_explicitly_disable_configured_graph(self):
        with patch.dict(os.environ, {}, clear=True):
            config = load_voice_config()
        config = copy.deepcopy(config)
        config['talker_mode'] = 'cuda_graph'
        with patch('neural_tts.json.loads', return_value=config), patch.dict(
                os.environ, {'DELIVERY_TTS_TALKER_MODE': 'official'}):
            self.assertEqual(load_voice_config()['talker_mode'], 'official')

    def test_unknown_mode_is_rejected(self):
        with patch.dict(os.environ, {'DELIVERY_TTS_TALKER_MODE': 'fast'}):
            with self.assertRaisesRegex(NeuralTtsError, 'invalid_talker_mode'):
                load_voice_config()

    def test_graph_mode_does_not_silently_run_without_its_graph(self):
        with self.assertRaisesRegex(NeuralTtsError, 'talker_graph_missing'):
            talker_generation(SimpleNamespace(config={'talker_mode': 'cuda_graph'}))
        with talker_generation(SimpleNamespace(config={})):
            pass

    def test_graph_startup_warms_both_voices_before_returning(self):
        with patch.dict(os.environ, {}, clear=True):
            config = load_voice_config()
        calls = []
        def synthesize(task, closed, on_chunk):
            self.assertFalse(closed.is_set())
            self.assertEqual(task.chunk_frames, 8)
            self.assertEqual(len(task.task_id), 32)
            self.assertEqual(task.speaker, config['voices'][task.voice]['speaker'])
            calls.append(task.voice)
            on_chunk({'type': 'audio_chunk'})
            return {'type': 'audio_end'}
        fake = SimpleNamespace(synthesize=synthesize)
        for mode, startup_check, expected in (('official', False, []), ('cuda_graph', True, []),
                                               ('cuda_graph', False, ['male', 'female'])):
            with self.subTest(mode=mode, startup_check=startup_check):
                config['talker_mode'] = mode
                calls.clear()
                with patch('neural_tts_worker.QwenEngine', return_value=fake), patch(
                        'neural_tts_worker.QwenCloneEngine', return_value=fake):
                    QwenVoiceEngines(config, threading.Event(), check_startup=startup_check)
                self.assertEqual(calls, expected)

    def test_warmup_failure_is_reported_as_startup_failure(self):
        with patch.dict(os.environ, {}, clear=True):
            config = load_voice_config()
        config['talker_mode'] = 'cuda_graph'
        def synthesize(*args, **kwargs):
            raise NeuralTtsError('neural_tts_timeout')
        fake = SimpleNamespace(synthesize=synthesize)
        with patch('neural_tts_worker.QwenEngine', return_value=fake), patch(
                'neural_tts_worker.QwenCloneEngine', return_value=fake):
            with self.assertRaisesRegex(NeuralTtsError, 'warmup_failed'):
                QwenVoiceEngines(config, threading.Event())

    def test_cancelled_startup_does_not_start_warmup(self):
        with patch.dict(os.environ, {}, clear=True):
            config = load_voice_config()
        config['talker_mode'] = 'cuda_graph'
        fake = SimpleNamespace(synthesize=lambda *args, **kwargs: self.fail('关闭时不能继续预热'))
        closed = threading.Event()
        closed.set()
        with patch('neural_tts_worker.QwenEngine', return_value=fake), patch(
                'neural_tts_worker.QwenCloneEngine', return_value=fake):
            with self.assertRaisesRegex(NeuralTtsError, 'cancelled'):
                QwenVoiceEngines(config, closed)


if __name__ == '__main__':
    unittest.main()
