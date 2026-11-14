"""沿真实 HTTP 与回复协议检查本地动作接入，不加载模型或语音"""
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from combat_model import choose_combat_action
from scheduler import Job
from server import validate_action, validate_context
from streaming import run_job
from test_combat_model import context, FixedClient
from test_continued_activity import current_request


@contextmanager
def local_endpoint(block_completion=False):
    calls = []
    started, release, processing = threading.Event(), threading.Event(), threading.Event()
    if not block_completion:
        release.set()

    class Handler(BaseHTTPRequestHandler):
        def reply(self, value):
            data = json.dumps(value).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass

        def do_GET(self):
            self.reply([{'is_processing': processing.is_set()}])

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            calls.append((self.path, body))
            if self.path == '/apply-template':
                self.reply({'prompt': 'test template'})
            elif self.path == '/tokenize':
                self.reply({'tokens': [1, 2, 3]})
            elif self.path == '/v1/chat/completions':
                processing.set()
                started.set()
                release.wait(4)
                processing.clear()
                self.reply({'choices': [{'finish_reason': 'stop',
                                        'message': {'content': '{"action":"Fight","target_id":"opponent"}'}}]})
            else:
                self.send_error(404)

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield SimpleNamespace(url=f'http://127.0.0.1:{server.server_port}', calls=calls,
                              started=started, release=release, processing=processing)
    finally:
        release.set()
        server.shutdown()
        server.server_close()
        thread.join(1)


class CombatModelIntegrationTests(unittest.TestCase):
    def test_v4_actual_http_produces_identity_checked_silent_reply(self):
        data = {**context(), 'capability_version': 4, 'stream_protocol_version': 2}
        validate_context(data)
        original = json.dumps(data, sort_keys=True)
        with local_endpoint() as endpoint, \
             patch.dict(os.environ, {'DELIVERY_COMBAT_MODEL_URL': endpoint.url,
                                     'DELIVERY_COMBAT_MODEL_PROFILE_IDS': 'chunlai,xiumei',
                                     'DELIVERY_COMBAT_MODEL_TIMEOUT_SECONDS': '1.5',
                                     'DELIVERY_AI_DIALOGUE_LOG': ''}), \
             patch('streaming.AppServer') as dialogue, patch('streaming.speech_synthesizer') as speech:
            job = Job(data)
            run_job(job, 'codex-local', validate_action, None).result(1)
        events = list(job.events.queue)
        self.assertEqual([event['type'] for event in events], ['proposal', 'model_done', 'done'])
        proposal = events[0]['proposal']
        self.assertEqual(proposal['request_id'], data['request_id'])
        self.assertEqual(proposal['capability_version'], 4)
        self.assertEqual(proposal['target_id'], data['current_action']['target_id'])
        self.assertEqual(proposal['evidence_ids'], data['candidates'][0]['evidence_ids'])
        self.assertEqual(proposal['speech'], '')
        self.assertEqual(events[1]['sentences'], 0)
        self.assertEqual(json.dumps(data, sort_keys=True), original)
        self.assertEqual([path for path, _ in endpoint.calls],
                         ['/apply-template', '/tokenize', '/v1/chat/completions'])
        payload = endpoint.calls[-1][1]
        self.assertEqual(payload['max_tokens'], 64)
        self.assertFalse(payload['chat_template_kwargs']['enable_thinking'])
        self.assertTrue(payload['cache_prompt'])
        dialogue.assert_not_called()
        speech.assert_not_called()

    def test_player_continue_current_bypasses_local_combat_model(self):
        data = current_request()
        data.update(profile_id='chunlai', request_source='player')
        validate_context(data)
        small = FixedClient()
        self.assertEqual(choose_combat_action(data, threading.Event(), client=small).fallback_reason,
                         'player_or_unknown_request')
        self.assertEqual(small.calls, 0)
        original = json.dumps(data['current_action'], sort_keys=True)
        chosen = data['candidates'][-1]

        class Dialogue:
            def stream(self, prompt, instructions, cancel, on_delta, **_kwargs):
                on_delta(json.dumps({'candidate_index': len(data['candidates']) - 1,
                                     'speech': '你说吧，我听着呢。', 'emotion': 'Calm'}))

        class Voice:
            def synthesize(self, text, voice, emotion, cancel):
                return b'\0\0', 16000

        with patch.dict(os.environ, {'DELIVERY_COMBAT_MODEL_URL': 'http://127.0.0.1:8771',
                                     'DELIVERY_AI_DIALOGUE_LOG': ''}), \
             patch('combat_model.LocalCombatClient.choose') as choose, \
             patch('streaming._local', SimpleNamespace(model=Dialogue())), \
             patch('streaming.uses_neural_speech', return_value=False), \
             patch('streaming.speech_synthesizer', return_value=Voice()):
            job = Job(data)
            run_job(job, 'codex-local', validate_action, None).result(1)
        choose.assert_not_called()
        proposal = list(job.events.queue)[0]['proposal']
        self.assertEqual(proposal['action'], 'ContinueCurrent')
        self.assertEqual(proposal['candidate_id'], chosen['candidate_id'])
        self.assertEqual(proposal['duration'], 0)
        self.assertEqual(proposal['target_id'], '')
        self.assertTrue(proposal['speech'])
        self.assertEqual(json.dumps(data['current_action'], sort_keys=True), original)

    def test_cancel_during_http_never_publishes_late_attack_and_waits_for_idle(self):
        data = {**context(), 'capability_version': 4, 'stream_protocol_version': 2}
        validate_context(data)
        errors = []
        job = Job(data)
        with local_endpoint(block_completion=True) as endpoint, \
             patch.dict(os.environ, {'DELIVERY_COMBAT_MODEL_URL': endpoint.url,
                                     'DELIVERY_COMBAT_MODEL_PROFILE_IDS': 'chunlai,xiumei',
                                     'DELIVERY_COMBAT_MODEL_TIMEOUT_SECONDS': '1.5',
                                     'DELIVERY_AI_DIALOGUE_LOG': ''}), \
             patch('streaming.AppServer') as fallback:
            def run():
                try:
                    run_job(job, 'codex-local', validate_action, None).result(1)
                except RuntimeError as error:
                    errors.append(str(error))
            task = threading.Thread(target=run, daemon=True)
            task.start()
            try:
                self.assertTrue(endpoint.started.wait(1))
                job.cancel_request()
                # Windows 等待响应头时可能到 1.5 秒 socket 截止才返回取消
                task.join(2)
                self.assertFalse(task.is_alive())
                self.assertEqual(errors, ['cancelled'])
                self.assertNotIn('proposal', [event['type'] for event in job.events.queue])
                self.assertNotIn('model_done', [event['type'] for event in job.events.queue])
                self.assertEqual(choose_combat_action(data, threading.Event(), client=FixedClient()).fallback_reason, 'busy')
                endpoint.release.set()
                deadline = time.monotonic() + 1
                result = None
                while time.monotonic() < deadline:
                    result = choose_combat_action(data, threading.Event(), client=FixedClient('Wait'))
                    if result.proposal:
                        break
                    time.sleep(.02)
                self.assertIsNotNone(result.proposal)
                self.assertEqual(result.proposal['action'], 'Wait')
                self.assertNotIn('proposal', [event['type'] for event in job.events.queue])
                fallback.assert_not_called()
            finally:
                endpoint.release.set()
                task.join(2)
                # 断言失败时也让假服务确认空闲，不能把其他测试的全局名额一直占住
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    drained = choose_combat_action(data, threading.Event(), client=FixedClient('Wait'))
                    if drained.fallback_reason != 'busy':
                        break
                    time.sleep(.02)


if __name__ == '__main__':
    unittest.main()
