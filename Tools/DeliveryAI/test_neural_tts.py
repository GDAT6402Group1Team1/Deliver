"""检查 Qwen 进程协议与取消顺序，测试不加载模型或 CUDA"""
import base64
import json
import subprocess
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from neural_tts import NeuralTtsError, QwenSpeechSynthesizer, load_voice_config, uses_neural_voice
from neural_tts_worker import WorkerRequests, generation_controls


class FakeProcess:
    def __init__(self, client):
        self.client = client
        self.commands = []
        self.exit_code = None
        self.killed = False
        self.started = threading.Event()
        self.cancel_received = threading.Event()
        self.auto_audio = True
        self.stdin = SimpleNamespace(write=self.write, flush=lambda: None, close=lambda: None)
        self.stdout = SimpleNamespace(close=lambda: None)

    def write(self, line):
        command = json.loads(line)
        self.commands.append(command)
        if command['type'] == 'synthesize':
            self.started.set()
            if self.auto_audio:
                self.reply(command['task_id'])
        elif command['type'] == 'cancel':
            self.cancel_received.set()
        elif command['type'] == 'close':
            self.exit_code = 0

    def reply(self, task_id, kind='audio', pcm=b'\0\0\1\0'):
        command = next(item for item in reversed(self.commands) if item.get('task_id') == task_id
                       and item['type'] == 'synthesize')
        self.client.messages.put({'type': kind, 'task_id': task_id,
                                  'voice_name': self.client.speakers[command['voice']],
                                  'voice_identity': self.client.identities[command['voice']],
                                  'sample_rate': 24000, 'pcm': base64.b64encode(pcm).decode(), 'code': 'cancelled'})

    def poll(self):
        return self.exit_code

    def kill(self):
        self.killed = True
        self.exit_code = -1

    def wait(self, timeout):
        if self.exit_code is None:
            raise subprocess.TimeoutExpired('fake-worker', timeout)
        return self.exit_code


class NeuralTtsTests(unittest.TestCase):
    def setUp(self):
        self.config = load_voice_config()
        self.config['voices']['female'] = {'provider': 'qwen', 'speaker': 'Serena'}
        override = patch('neural_tts.load_voice_config', return_value=self.config)
        override.start()
        self.addCleanup(override.stop)

    def client(self):
        client = QwenSpeechSynthesizer()
        process = FakeProcess(client)
        client.process = process
        client.ready = True
        client.config = {**client.config, 'cancel_grace_seconds': .08, 'process_exit_timeout_seconds': .1}
        self.addCleanup(client.close)
        return client, process

    def test_explicit_routing_for_both_configured_voices(self):
        with patch.dict('os.environ', {}, clear=True):
            self.assertFalse(uses_neural_voice('male'))
        with patch.dict('os.environ', {'DELIVERY_TTS_PROVIDER': 'qwen'}):
            self.assertTrue(uses_neural_voice('male'))
            self.assertTrue(uses_neural_voice('female'))
            self.assertFalse(uses_neural_voice('unconfigured'))

    def test_prepare_reuses_worker_and_returns_complete_pcm(self):
        client, process = self.client()
        with patch('neural_tts.subprocess.Popen', side_effect=AssertionError('second worker')):
            self.assertTrue(client.prepare(threading.Event()))
            self.assertTrue(client.prepare(threading.Event()))
            self.assertEqual(client.synthesize('先听我说。', 'male', 'Calm', threading.Event()), (b'\0\0\1\0', 24000))
        self.assertEqual(len(process.commands), 1)

    def test_switching_speaker_reuses_worker_and_keeps_male_configuration(self):
        client, process = self.client()
        with patch('neural_tts.subprocess.Popen', side_effect=AssertionError('second worker')):
            for voice, expected in (('male', 'Uncle_Fu'), ('female', 'Serena'), ('male', 'Uncle_Fu')):
                self.assertEqual(client.synthesize('你先说。', voice, 'Calm', threading.Event())[1], 24000)
                self.assertEqual(client.voice_name, expected)
        self.assertEqual([item['voice'] for item in process.commands], ['male', 'female', 'male'])

    def test_wrong_speaker_audio_is_rejected(self):
        client, process = self.client()
        process.reply = lambda task_id: client.messages.put({
            'type': 'audio', 'task_id': task_id, 'voice_name': 'Uncle_Fu', 'sample_rate': 24000,
            'pcm': base64.b64encode(b'\0\0').decode()})
        with self.assertRaisesRegex(NeuralTtsError, 'invalid_audio_format'):
            client.synthesize('小芳的话。', 'female', 'Calm', threading.Event())
        self.assertTrue(process.killed)

    def test_cancel_waits_for_worker_terminal_and_discards_late_audio(self):
        client, process = self.client()
        process.auto_audio = False
        client.config['cancel_grace_seconds'] = 1
        cancel = threading.Event()
        errors = []
        def run():
            try:
                client.synthesize('先听我说。', 'male', 'Calm', cancel)
            except NeuralTtsError as error:
                errors.append(error.code)
        thread = threading.Thread(target=run)
        thread.start()
        self.assertTrue(process.started.wait(1))
        cancel.set()
        self.assertTrue(process.cancel_received.wait(1))
        self.assertTrue(thread.is_alive())
        process.reply(process.commands[0]['task_id'])
        thread.join(1)
        self.assertFalse(thread.is_alive())
        self.assertEqual(errors, ['cancelled'])
        self.assertFalse(process.killed)
        process.auto_audio = True
        self.assertEqual(client.synthesize('再说一句。', 'female', 'Calm', threading.Event())[1], 24000)
        self.assertEqual(client.voice_name, 'Serena')

    def test_cancel_timeout_stops_owned_worker_before_return(self):
        client, process = self.client()
        process.auto_audio = False
        cancel = threading.Event()
        threading.Timer(.02, cancel.set).start()
        with self.assertRaisesRegex(NeuralTtsError, 'cancelled'):
            client.synthesize('先听我说。', 'male', 'Calm', cancel)
        self.assertTrue(process.killed)
        self.assertIsNotNone(process.poll())
        self.assertIsNone(client.process)

    def test_waiting_cancel_does_not_submit_work(self):
        client, process = self.client()
        cancel = threading.Event()
        client.lock.acquire()
        try:
            threading.Timer(.02, cancel.set).start()
            with self.assertRaisesRegex(NeuralTtsError, 'cancelled'):
                client.synthesize('先听我说。', 'male', 'Calm', cancel)
        finally:
            client.lock.release()
        self.assertEqual(process.commands, [])

    def test_oversized_pcm_is_rejected_and_close_is_repeatable(self):
        client, process = self.client()
        process.reply = lambda task_id: client.messages.put({
            'type': 'audio', 'task_id': task_id, 'voice_name': 'Uncle_Fu', 'sample_rate': 24000,
            'voice_identity': client.identities['male'],
            'pcm': base64.b64encode(b'\0' * 384002).decode()})
        with self.assertRaisesRegex(NeuralTtsError, 'invalid_pcm'):
            client.synthesize('先听我说。', 'male', 'Calm', threading.Event())
        client.close()
        client.close()
        with self.assertRaisesRegex(NeuralTtsError, 'cancelled'):
            client.prepare(threading.Event())

    def test_worker_cancel_targets_only_current_task(self):
        replies = []
        requests = WorkerRequests(replies.append, {'male': 'Uncle_Fu', 'female': 'Serena'})
        command = {'type': 'synthesize', 'task_id': 'a' * 32, 'text': '你好。', 'voice': 'male', 'emotion': 'Calm'}
        requests.receive(command)
        current = requests.pending.get_nowait()
        self.assertEqual(current.speaker, 'Uncle_Fu')
        requests.receive({**command, 'task_id': 'b' * 32})
        self.assertEqual(replies[0]['code'], 'neural_tts_busy')
        requests.receive({'type': 'cancel', 'task_id': 'b' * 32})
        self.assertFalse(current.cancel.is_set())
        requests.receive({'type': 'cancel', 'task_id': 'a' * 32})
        self.assertTrue(current.cancel.is_set())
        requests.finish(current)
        requests.receive({**command, 'task_id': 'c' * 32, 'voice': 'female'})
        self.assertEqual(requests.pending.get_nowait().speaker, 'Serena')
        requests.close()
        requests.close()

    def test_stopping_criterion_interrupts_and_restores_original_methods(self):
        cancel, closed = threading.Event(), threading.Event()
        class Predictor:
            def generate(self, **kwargs):
                return 'original_predictor'
        class Talker:
            def __init__(self):
                self.code_predictor = Predictor()
            def generate(self, **kwargs):
                cancel.set()
                kwargs['stopping_criteria'][-1](None, None)
                raise AssertionError('partial audio accepted')
        talker = Talker()
        original = talker.generate.__func__
        engine = SimpleNamespace(torch=None, model=SimpleNamespace(model=SimpleNamespace(talker=talker)),
                                 StoppingCriteria=object, StoppingCriteriaList=list)
        with self.assertRaisesRegex(NeuralTtsError, 'cancelled'):
            with generation_controls(engine, cancel, closed, time.monotonic() + 2):
                talker.generate()
        self.assertIs(talker.generate.__func__, original)
        self.assertNotIn('generate', vars(talker.code_predictor))
        self.assertEqual(talker.code_predictor.generate(), 'original_predictor')


if __name__ == '__main__':
    unittest.main()
