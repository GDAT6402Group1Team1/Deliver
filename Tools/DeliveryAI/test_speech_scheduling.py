"""用可控模型和语音检查结束时机，不连接模型或 TTS 服务"""
import json
import queue
import threading
import unittest
import urllib.request
from unittest.mock import patch

from server import DecisionServer
from test_sentence_pipeline import request


def player_request(label):
    data = request()
    data.update(persona=label, request_source='player')
    return data


class SpeechSchedulingTests(unittest.TestCase):
    def wait_finished(self, server, job):
        with server.scheduler.condition:
            self.assertTrue(server.scheduler.condition.wait_for(lambda: job.finished, 1))

    def events(self, job):
        result = []
        while True:
            try:
                result.append(job.events.get_nowait())
            except queue.Empty:
                return result

    def test_two_slow_voices_release_model_slots_but_keep_players_ahead_of_ambient(self):
        release = threading.Event()
        voices_started = threading.Barrier(3)
        ambient_started = threading.Event()
        class Voice:
            def synthesize(self, text, voice, emotion, cancel):
                voices_started.wait(timeout=2)
                if not release.wait(2):
                    raise TimeoutError('test_voice_not_released')
                return b'\x01\x00'*160, 16000
        def model(prompt, cancel, on_delta):
            label = json.loads(prompt)['persona']
            if label == 'ambient':
                ambient_started.set()
            on_delta(json.dumps({'candidate_index': 0, 'speech': label+'。' if label in ('first', 'second') else '', 'emotion': 'Calm'}))

        with patch('streaming.stream_api', side_effect=model), patch('streaming.speech_synthesizer', return_value=Voice()):
            server = DecisionServer(('127.0.0.1', 0), 'fixture')
            server.provider = 'llm'
            try:
                first = server.scheduler.submit(player_request('first'))
                second = server.scheduler.submit(player_request('second'))
                voices_started.wait(timeout=2)
                ambient = server.scheduler.submit({**player_request('ambient'), 'request_source': 'ambient'})
                third = server.scheduler.submit(player_request('third'))
                self.wait_finished(server, third)
                self.assertFalse(first.finished or second.finished or ambient_started.is_set())
                # 两个模型位置都已归还，慢语音仍由原 Job 记录
                acquired = 0
                try:
                    for _ in range(2):
                        self.assertTrue(server.slots.acquire(timeout=1))
                        acquired += 1
                finally:
                    for _ in range(acquired):
                        server.slots.release()
                with server.scheduler.condition:
                    self.assertEqual(server.scheduler.running_players, 2)
                    self.assertIn(first.key, server.scheduler.jobs)
                    self.assertIn(second.key, server.scheduler.jobs)
                self.assertEqual([event['type'] for event in self.events(first)], ['proposal'])
                self.assertEqual([event['type'] for event in self.events(second)], ['proposal'])
                release.set()
                self.wait_finished(server, first)
                self.wait_finished(server, second)
                self.wait_finished(server, ambient)
                for job in (first, second):
                    self.assertEqual([event['type'] for event in self.events(job)], ['sentence', 'done'])
                self.assertTrue(ambient_started.is_set())
            finally:
                release.set()
                server.server_close()

    def test_cancel_after_model_end_keeps_job_until_running_voices_exit(self):
        release = threading.Event()
        voices_started = threading.Barrier(3)
        model_returned = threading.Event()
        spoken = []
        class Voice:
            def synthesize(self, text, voice, emotion, cancel):
                spoken.append(text)
                voices_started.wait(timeout=2)
                # 故意不因 cancel 返回，检查后台不会把尚未停止的工作当作已完成
                if not release.wait(2):
                    raise TimeoutError('test_voice_not_released')
                return b'\x01\x00'*160, 16000
        def model(prompt, cancel, on_delta):
            on_delta(json.dumps({'candidate_index': 0, 'speech': '一。二。三。', 'emotion': 'Calm'}))
            model_returned.set()

        with patch('streaming.stream_api', side_effect=model), patch('streaming.speech_synthesizer', return_value=Voice()):
            server = DecisionServer(('127.0.0.1', 0), 'fixture')
            server.provider = 'llm'
            server.scheduler.capacity = 1
            try:
                job = server.scheduler.submit(player_request('cancel'))
                voices_started.wait(timeout=2)
                self.assertTrue(model_returned.wait(1))
                # HTTP 断线也直接设置 Event，不依赖 Scheduler.cancel 才能正确结束
                job.cancel.set()
                self.assertFalse(job.finished)
                self.assertFalse(job.completion.done())
                with self.assertRaisesRegex(RuntimeError, 'busy'):
                    server.scheduler.submit(player_request('next'))
                with self.assertRaisesRegex(RuntimeError, 'busy'):
                    server.scheduler.submit(job.context)
                release.set()
                self.wait_finished(server, job)
                self.assertCountEqual(spoken, ['一。', '二。'])
                self.assertEqual([event['type'] for event in self.events(job)], ['proposal'])
                self.assertNotIn(job.key, server.scheduler.jobs)
            finally:
                release.set()
                server.server_close()

    def test_model_failure_keeps_started_voice_until_it_really_stops(self):
        started = threading.Event()
        release = threading.Event()
        class Voice:
            def synthesize(self, text, voice, emotion, cancel):
                started.set()
                if not release.wait(2):
                    raise TimeoutError('test_voice_not_released')
                return b'\x01\x00'*160, 16000
        def model(prompt, cancel, on_delta):
            if json.loads(prompt)['persona'] == 'next':
                on_delta(json.dumps({'candidate_index': 0, 'speech': '', 'emotion': 'Calm'}))
                return
            on_delta(json.dumps({'candidate_index': 0, 'speech': '首句。', 'emotion': 'Calm'}))
            if not started.wait(1):
                raise TimeoutError('voice_not_started')
            raise ValueError('invalid_followup')

        with patch('streaming.stream_api', side_effect=model), patch('streaming.speech_synthesizer', return_value=Voice()):
            server = DecisionServer(('127.0.0.1', 0), 'fixture')
            server.provider = 'llm'
            try:
                job = server.scheduler.submit(player_request('failed'))
                self.assertTrue(started.wait(1))
                self.assertTrue(job.cancel.wait(1))
                self.assertFalse(job.finished)
                self.assertIn(job.key, server.scheduler.jobs)
                following = server.scheduler.submit(player_request('next'))
                self.wait_finished(server, following)
                release.set()
                self.wait_finished(server, job)
                self.assertEqual([event['type'] for event in self.events(job)], ['proposal', 'error'])
            finally:
                release.set()
                server.server_close()

    def test_voice_failure_keeps_text_and_sends_one_done(self):
        def model(prompt, cancel, on_delta):
            on_delta(json.dumps({'candidate_index': 0, 'speech': '首句。', 'emotion': 'Calm'}))
        class Voice:
            def synthesize(self, *args):
                raise ValueError('voice_unavailable')
        with patch('streaming.stream_api', side_effect=model), patch('streaming.speech_synthesizer', return_value=Voice()):
            server = DecisionServer(('127.0.0.1', 0), 'fixture')
            server.provider = 'llm'
            try:
                job = server.scheduler.submit(player_request('failed_voice'))
                self.wait_finished(server, job)
                events = self.events(job)
                self.assertEqual([event['type'] for event in events], ['proposal', 'sentence', 'done'])
                self.assertEqual((events[1]['text'], events[1]['pcm'], events[1]['audio_error']), ('首句。', '', 'ValueError'))
            finally:
                server.server_close()

    def test_http_remains_open_until_async_voice_finishes(self):
        release = threading.Event()
        received = threading.Event()
        events, errors = [], []
        class Voice:
            def synthesize(self, text, voice, emotion, cancel):
                if not release.wait(2):
                    raise TimeoutError('test_voice_not_released')
                return b'\x01\x00'*160, 16000
        def model(prompt, cancel, on_delta):
            on_delta(json.dumps({'candidate_index': 0, 'speech': '首句。', 'emotion': 'Calm'}))
        with patch('streaming.stream_api', side_effect=model), patch('streaming.speech_synthesizer', return_value=Voice()):
            server = DecisionServer(('127.0.0.1', 0), 'fixture')
            server.provider = 'llm'
            serving = threading.Thread(target=server.serve_forever, daemon=True)
            serving.start()
            reader = None
            try:
                url = f'http://127.0.0.1:{server.server_port}/v1/decisions/stream'
                request_data = urllib.request.Request(url, json.dumps(player_request('http')).encode(), {'Content-Type': 'application/json'})
                with urllib.request.urlopen(request_data, timeout=2) as response:
                    self.assertEqual(json.loads(response.readline())['type'], 'proposal')
                    def read():
                        try:
                            events.extend(json.loads(line) for line in response)
                        except Exception as error:
                            errors.append(error)
                        finally:
                            received.set()
                    reader = threading.Thread(target=read, daemon=True)
                    reader.start()
                    self.assertFalse(received.wait(.05))
                    release.set()
                    self.assertTrue(received.wait(1))
                    reader.join(1)
                self.assertEqual(errors, [])
                self.assertEqual([event['type'] for event in events if event['type'] != 'keepalive'], ['sentence', 'done'])
            finally:
                release.set()
                server.shutdown()
                server.server_close()
                serving.join(2)
                if reader is not None:
                    reader.join(2)

    def test_close_waits_for_voice_exit_after_model_has_returned(self):
        started = threading.Event()
        release = threading.Event()
        closed = threading.Event()
        class Voice:
            def synthesize(self, text, voice, emotion, cancel):
                started.set()
                if not release.wait(2):
                    raise TimeoutError('test_voice_not_released')
                return b'\x01\x00'*160, 16000
        def model(prompt, cancel, on_delta):
            on_delta(json.dumps({'candidate_index': 0, 'speech': '首句。', 'emotion': 'Calm'}))
        with patch('streaming.stream_api', side_effect=model), patch('streaming.speech_synthesizer', return_value=Voice()):
            server = DecisionServer(('127.0.0.1', 0), 'fixture')
            server.provider = 'llm'
            job = server.scheduler.submit(player_request('closing'))
            def close():
                server.server_close()
                closed.set()
            closer = threading.Thread(target=close, daemon=True)
            try:
                self.assertTrue(started.wait(1))
                closer.start()
                self.assertTrue(job.cancel.wait(1))
                self.assertFalse(closed.wait(.05))
                self.assertFalse(job.finished)
                release.set()
                self.assertTrue(closed.wait(1))
                self.assertTrue(job.finished)
                self.assertFalse(server.scheduler.jobs)
                self.assertFalse(any(worker.is_alive() for worker in server.scheduler.threads))
                self.assertEqual([event['type'] for event in self.events(job)], ['proposal'])
            finally:
                release.set()
                if closer.ident is not None:
                    closer.join(2)
                server.server_close()


if __name__ == '__main__':
    unittest.main()
