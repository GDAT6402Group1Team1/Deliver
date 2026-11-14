"""用假对象检查同机测量的时间表、取消和标签隔离，不调用模型或 GPU"""
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest

from benchmark_combat_coexist import (LunaCombatClient, evaluate_choice, evaluation_context,
                                     load_cases, run_window, summary, synthesize_line, warmup_model)
from combat_model import CombatDecisionResult, prepare_combat_context
from test_combat_model import context


class FakeSpeech:
    def synthesize_stream(self, text, voice, emotion, cancel, receive, chunk_frames=8):
        for _ in range(2):
            if cancel.wait(.003):
                raise RuntimeError('cancelled')
            receive(b'\0'*2400)
        return 4800, 24000


class FakeDecision:
    last_output = ''
    last_usage = None

    def choose(self, prompt, candidates, cancel, measure):
        if cancel.wait(.005):
            raise RuntimeError('cancelled')
        if 'acceptable_actions' in json.dumps(prompt):
            raise AssertionError('验收标签不能发给模型')
        if 'profile_id' in prompt:
            raise AssertionError('补充的路由编号不能进入模型输入')
        self.last_output = '{"action":"Fight","target_id":"opponent"}'
        return 'Fight'


def cases(count=2):
    return [{'id': str(index), 'usage': 'acceptance_only', 'context': {**context(), 'profile_id': ''},
             'label': {'acceptable_actions': ['Fight']}} for index in range(count)]


class CoexistBenchmarkTests(unittest.TestCase):
    def test_longer_warmup_does_not_change_real_request_deadline(self):
        class Client:
            timeout_seconds = 1.5
            observed_timeouts = []
            def choose(self, prompt, candidates, cancel):
                self.observed_timeouts.append(self.timeout_seconds)
                self.assert_no_labels = 'acceptable_actions' not in json.dumps(prompt)
                return 'Wait'
        client, report = Client(), {}
        warmup_model(client, report, 10.)
        self.assertEqual(report['warmup_timeout_seconds'], 10.)
        self.assertEqual(report['warmup_model_status'], 'completed')
        self.assertGreaterEqual(report['warmup_model_ms'], 0.)
        self.assertTrue(client.assert_no_labels)
        client.choose({}, {}, threading.Event())
        self.assertEqual(client.observed_timeouts, [10., 1.5])
        warmup_model(client, report)
        self.assertEqual(report['warmup_timeout_seconds'], 1.5)

    def test_failed_warmup_restores_deadline_and_records_elapsed_time(self):
        class Client:
            timeout_seconds = 1.5
            def choose(self, prompt, candidates, cancel):
                raise TimeoutError('warmup_example')
        client, report = Client(), {}
        with self.assertRaises(TimeoutError):
            warmup_model(client, report, 10.)
        self.assertEqual(client.timeout_seconds, 1.5)
        self.assertEqual(report['warmup_model_status'], 'failed')
        self.assertIn('TimeoutError', report['warmup_model_error'])
        self.assertGreaterEqual(report['warmup_model_ms'], 0.)
        for value in (0., -1., 31., float('nan'), float('inf')):
            with self.subTest(timeout=value), self.assertRaises(ValueError):
                warmup_model(client, {}, value)
        self.assertEqual(client.timeout_seconds, 1.5)

    def test_load_requires_forty_unique_evaluation_cases(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'cases.json'
            path.write_text(json.dumps(cases(40)), encoding='utf-8')
            loaded, digest = load_cases(path)
            self.assertEqual(len(loaded), 40)
            self.assertEqual(len(digest), 64)
            for name, profile in (('大壮', 'chunlai'), ('翠花', 'xiumei')):
                original = {**loaded[0]['context'], 'self_name': name}
                routed = evaluation_context(original)
                self.assertEqual(original['profile_id'], '')
                self.assertEqual(routed['profile_id'], profile)
                self.assertEqual(prepare_combat_context(original), prepare_combat_context(routed))
                routed['evidence'][0]['description'] = '仅修改副本'
                self.assertNotEqual(original['evidence'][0]['description'], '仅修改副本')
            self.assertEqual(evaluation_context({**loaded[0]['context'], 'self_name': '二狗'})['profile_id'], '')
            self.assertEqual(evaluation_context({**loaded[0]['context'], 'profile_id': 'other'})['profile_id'], 'other')
            self.assertEqual(json.loads(path.read_text(encoding='utf-8'))[0]['context']['profile_id'], '')
            path.write_text(json.dumps(cases(39)), encoding='utf-8')
            with self.assertRaises(ValueError):
                load_cases(path)

    def test_speech_reports_actual_chunks_and_simulation_limits(self):
        result = synthesize_line(FakeSpeech(), ('short', 'male', '你先听我说。'), threading.Event())
        self.assertEqual(result['status'], 'completed')
        self.assertEqual(result['chunk_count'], 2)
        self.assertEqual(result['delivered_audio_seconds'], .1)
        self.assertGreater(result['generation_rtf'], 0)
        self.assertTrue(result['buffer_simulation']['simulation_only_not_ue_playback'])
        self.assertFalse(result['ue_playback_measured'])

    def test_one_speech_lane_and_serial_decisions_stop_at_window_end(self):
        cadence = [{'interval_seconds': .02}]*4
        result = run_window(FakeSpeech(), FakeDecision(), cases(), cadence, .055, decision_interval=.01)
        self.assertFalse(result['errors'])
        self.assertTrue(result['speech'])
        self.assertTrue(result['decisions'])
        self.assertTrue(all(row['protocol_valid'] for row in result['decisions'] if row['fallback_reason'] != 'cancelled'))
        self.assertTrue(all(row['profile_added_for_evaluation'] for row in result['decisions']))
        self.assertEqual([row['case_index'] for row in result['decisions'][:2]], [0, 1])
        self.assertTrue(all(row['started_at_seconds'] >= row['scheduled_at_seconds'] for row in result['speech']))
        self.assertFalse(summary(result)['all_40_cases_completed'])
        self.assertFalse(summary(result)['quality_acceptance_claimed'])

    def test_none_baseline_does_not_submit_model_requests(self):
        result = run_window(FakeSpeech(), None, cases(), [{'interval_seconds': .02}]*4, .03)
        self.assertEqual(result['decisions'], [])
        self.assertTrue(result['speech'])

    def test_invalid_target_is_counted_even_when_protocol_rejects_it(self):
        result = evaluate_choice(cases()[0], CombatDecisionResult(fallback_reason='invalid_combat_choice'),
                                 '{"action":"Fight","target_id":"someone_else"}')
        self.assertTrue(result['illegal_target'])
        self.assertFalse(result['protocol_valid'])
        self.assertFalse(result['matches_evaluation_label'])

    def test_luna_uses_same_small_output_contract_without_tools(self):
        class FakeAppServer:
            last_token_usage = {'output': 12}
            def stream(self, prompt, instructions, cancel, on_delta, model=None, measure=None):
                self.prompt, self.instructions = prompt, instructions
                on_delta('{"action":"Wait","target_id":""}')
            def close(self):
                pass
        client = LunaCombatClient(1)
        client.service = FakeAppServer()
        action = client.choose({'events': []}, {'Wait': {}}, threading.Event())
        self.assertEqual(action, 'Wait')
        self.assertEqual(client.last_output, '{"action":"Wait","target_id":""}')
        self.assertNotIn('acceptable_actions', client.service.prompt)


if __name__ == '__main__':
    unittest.main()
