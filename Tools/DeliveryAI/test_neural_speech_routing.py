"""检查玩家语音的排队、声音选择和取消，不加载模型"""
import json
import threading
import unittest
from unittest.mock import patch

import streaming
from scheduler import Job
from neural_tts import load_voice_config
from server import validate_action
from test_sentence_pipeline import request


class NeuralSpeechRoutingTests(unittest.TestCase):
    def setUp(self):
        self.environment = patch.dict('os.environ', {'DELIVERY_TTS_PROVIDER': 'qwen'})
        self.environment.start()
        config = load_voice_config()
        config['voices']['female'] = {'provider': 'qwen', 'speaker': 'Serena'}
        self.voices = patch('neural_tts.load_voice_config', return_value=config)
        self.voices.start()
        streaming.prepare_resources(local_speech=False)

    def tearDown(self):
        streaming.close_resources()
        self.voices.stop()
        self.environment.stop()

    def run_reply(self, text, source='player', voice='male'):
        context = request()
        context.update(persona=text, request_source=source, voice=voice)
        job = Job(context)
        def model(prompt, cancel, on_delta):
            on_delta(json.dumps({'candidate_index': 0, 'speech': text, 'emotion': 'Calm'}))
        with patch('streaming.stream_api', side_effect=model):
            streaming.run_job(job, 'llm', validate_action, None)
        return job

    def test_player_sentence_precedes_waiting_chatter_and_reuses_one_model(self):
        started, release = threading.Event(), threading.Event()
        spoken = []
        class Voice:
            voice_name = 'Uncle_Fu'
            def synthesize(self, text, voice, emotion, cancel):
                spoken.append((text, voice))
                if text == '第一句。':
                    started.set()
                    if not release.wait(2):
                        raise TimeoutError('test_not_released')
                return b'\x01\x00'*240, 24000
            def close(self):
                release.set()
        with patch('streaming.QwenSpeechSynthesizer', return_value=Voice()) as factory:
            try:
                first = self.run_reply('第一句。', source='ambient')
                self.assertTrue(started.wait(1))
                chatter = self.run_reply('闲聊。', source='ambient')
                player = self.run_reply('回答玩家。', voice='female')
                self.assertFalse(first.completion.done())
                release.set()
                for job in (first, player, chatter):
                    job.completion.result(timeout=2)
                self.assertEqual(spoken, [('第一句。', 'male'), ('回答玩家。', 'female'), ('闲聊。', 'male')])
                factory.assert_called_once()
            finally:
                release.set()

    def test_running_cancel_waits_for_real_exit_and_drops_late_audio(self):
        started, release = threading.Event(), threading.Event()
        class Voice:
            def synthesize(self, text, voice, emotion, cancel):
                started.set()
                if not release.wait(2):
                    raise TimeoutError('test_not_released')
                return b'\x01\x00'*240, 24000
            def close(self):
                release.set()
        with patch('streaming.QwenSpeechSynthesizer', return_value=Voice()):
            try:
                job = self.run_reply('被打断的话。')
                self.assertTrue(started.wait(1))
                job.cancel.set()
                self.assertFalse(job.completion.done())
                release.set()
                job.completion.result(timeout=2)
                self.assertEqual([item['type'] for item in list(job.events.queue)], ['proposal'])
            finally:
                release.set()

    def test_neither_voice_switches_to_windows_after_neural_failure(self):
        class BrokenNeuralVoice:
            def synthesize(self, text, voice, emotion, cancel):
                raise RuntimeError('test_neural_failed')
            def close(self):
                pass
        with patch('streaming.QwenSpeechSynthesizer', return_value=BrokenNeuralVoice()) as neural, \
             patch('streaming.speech_synthesizer', side_effect=AssertionError('unexpected fallback')) as windows:
            female = self.run_reply('小芳的话。', voice='female')
            female.completion.result(timeout=2)
            male = self.run_reply('大军的话。')
            male.completion.result(timeout=2)
            neural.assert_called_once()
            windows.assert_not_called()
            for job in (female, male):
                sentence = next(item for item in list(job.events.queue) if item['type'] == 'sentence')
                self.assertEqual(sentence['pcm'], '')
                self.assertEqual(sentence['audio_error'], 'RuntimeError')

    def test_failed_close_keeps_resource_for_retry_and_closes_other_resources(self):
        calls = []
        class Resource:
            def __init__(self, name, fail=False):
                self.name, self.fail = name, fail
            def close(self):
                calls.append(self.name)
                if self.fail:
                    self.fail = False
                    raise RuntimeError('test_close_pending')
        first, second = Resource('neural', True), Resource('female')
        streaming.register_resource(first)
        streaming.register_resource(second)
        self.assertFalse(streaming.close_resources())
        self.assertEqual(calls, ['neural', 'female'])
        with self.assertRaisesRegex(RuntimeError, 'server_close_incomplete'):
            streaming.prepare_resources(local_speech=False)
        self.assertTrue(streaming.close_resources())
        self.assertEqual(calls, ['neural', 'female', 'neural'])
        streaming.prepare_resources(local_speech=False)


if __name__ == '__main__':
    unittest.main()
