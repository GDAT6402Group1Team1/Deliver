"""模型结束后可以继续决定，旧声音仍能正常发送或取消"""
import concurrent.futures
from contextlib import contextmanager
import json
import os
from pathlib import Path
import tempfile
import threading
import unittest
import uuid
from types import SimpleNamespace
from unittest.mock import patch

from scheduler import Job, Scheduler
from server import validate_action, validate_context
from streaming import run_job
from combat_model import CombatDecisionResult
from test_streaming import request
from test_combat_model import context as combat_request
import dialogue_log


class ModelCompletionTests(unittest.TestCase):
    def test_old_speech_does_not_hold_model_slot_or_finish_new_request(self):
        first_audio = concurrent.futures.Future()
        second_started = threading.Event()
        release_second = threading.Event()

        def run(job):
            if job.context['label'] == 'first':
                job.completion = first_audio
            else:
                second_started.set()
                release_second.wait(2)

        scheduler = Scheduler(run, workers=1, capacity=2)
        try:
            first = scheduler.submit({**request(), 'label': 'first'})
            with scheduler.condition:
                self.assertTrue(scheduler.condition.wait_for(lambda: first.model_finished, 1))
            self.assertFalse(first.finished)
            self.assertIn(first.key, scheduler.jobs)
            second = scheduler.submit({**request(), 'label': 'second'})
            self.assertTrue(second_started.wait(1), '旧语音不能阻挡新决定')
            self.assertEqual(scheduler.running_ambient, 1)
            self.assertTrue(scheduler.cancel(first.key), '模型结束后仍应能取消旧声音')
            self.assertFalse(first.finished)
            first_audio.set_result(None)
            self.assertTrue(first.finished)
            self.assertFalse(second.finished)
            self.assertEqual(scheduler.running_ambient, 1, '旧声音结束不能释放新模型名额')
            release_second.set()
            with scheduler.condition:
                self.assertTrue(scheduler.condition.wait_for(lambda: second.finished, 1))
            self.assertEqual(scheduler.running_ambient, 0)
        finally:
            release_second.set()
            if not first_audio.done():
                first_audio.set_result(None)
            scheduler.close()

    def test_v2_model_done_precedes_audio_done_and_v1_stays_compatible(self):
        for version in (1, 2):
            with self.subTest(version=version):
                data = request()
                data['stream_protocol_version'] = version
                audio_started = threading.Event()
                release_audio = threading.Event()

                class Model:
                    def stream(self, prompt, instructions, cancel, on_delta, **kwargs):
                        on_delta(json.dumps({'candidate_index': 0, 'speech': '你先别急。', 'emotion': 'Calm'}))

                class Voice:
                    def synthesize(self, text, voice, emotion, cancel):
                        audio_started.set()
                        if not release_audio.wait(2):
                            raise TimeoutError('test voice not released')
                        return b'\x00\x00', 16000

                job = Job(data)
                try:
                    with patch('streaming._local', SimpleNamespace(model=Model())), \
                         patch('streaming.uses_neural_speech', return_value=False), \
                         patch('streaming.speech_synthesizer', return_value=Voice()):
                        completion = run_job(job, 'codex-local', validate_action, None)
                        self.assertTrue(audio_started.wait(1))
                        self.assertFalse(completion.done())
                        before = list(job.events.queue)
                        self.assertEqual([event['type'] for event in before],
                                         ['proposal', 'model_done'] if version == 2 else ['proposal'])
                        if version == 2:
                            self.assertEqual(before[-1]['sentences'], 1)
                            diagnostics = before[-1]['diagnostics']
                            self.assertIs(type(diagnostics['first_sentence_ms']), int)
                            self.assertGreaterEqual(diagnostics['first_sentence_ms'], diagnostics['queue_ms'])
                            self.assertLessEqual(diagnostics['first_sentence_ms'], diagnostics['backend_elapsed_ms'])
                            self.assertFalse(completion.done(), '首句文字时间不能冒充语音完成时间')
                        release_audio.set()
                        completion.result(2)
                        self.assertEqual([event['type'] for event in job.events.queue],
                                         ['proposal', 'model_done', 'sentence', 'done'] if version == 2
                                         else ['proposal', 'sentence', 'done'])
                finally:
                    release_audio.set()

    def test_failed_or_cancelled_model_never_reports_model_done(self):
        for cancelled in (False, True):
            with self.subTest(cancelled=cancelled):
                job = Job({**request(), 'stream_protocol_version': 2})

                class Model:
                    def stream(self, prompt, instructions, cancel, on_delta, **kwargs):
                        if cancelled:
                            job.cancel_request()
                        raise RuntimeError('model stopped')

                with patch('streaming._local', SimpleNamespace(model=Model())), \
                     self.assertRaises(RuntimeError):
                    run_job(job, 'codex-local', validate_action, None)
                self.assertTrue(job.completion.done())
                self.assertNotIn('model_done', [event['type'] for event in job.events.queue])
                self.assertNotIn('done', [event['type'] for event in job.events.queue])

    def test_transport_version_does_not_change_capability_version(self):
        for version in (1, 2):
            data = request()
            data['stream_protocol_version'] = version
            self.assertIs(validate_context(data), data)
        for version in (True, 0, 3, '2', None):
            with self.subTest(version=version), self.assertRaises(ValueError):
                validate_context({**request(), 'stream_protocol_version': version})

    def test_current_action_requires_actual_model_action_and_finite_time(self):
        data = request()
        current = dict(action='Fight', source='Model', target_id=str(uuid.uuid4()),
                       action_id=str(uuid.uuid4()), remaining_seconds=4)
        data.update(current_action=current, pending_player_message=False)
        self.assertIs(validate_context(data), data)
        for fields in ({'source': 'GameplayRule'}, {'action': 'StrikeOnce'},
                       {'target_id': ''}, {'action_id': ''}, {'remaining_seconds': True},
                       {'remaining_seconds': float('nan')}, {'remaining_seconds': 9},
                       {'remaining_seconds': 0}, {'action': 'Flee', 'remaining_seconds': 6}):
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                validate_context({**data, 'current_action': {**current, **fields}})
        with self.assertRaises(ValueError):
            validate_context({**data, 'pending_player_message': 'false'})

    def test_local_action_uses_existing_proposal_protocol_without_speech(self):
        data = {**request(), 'stream_protocol_version': 2}
        proposal = validate_action(data, {**data['candidates'][0], 'speech': '', 'emotion': 'Calm'})
        job = Job(data)
        with patch('streaming.choose_combat_action', return_value=CombatDecisionResult(proposal)), \
             patch('streaming.prepare_runtime_combat_context', return_value=({'events': [{'kind': 'SelfCombat'}],
                 'subjects': [{'id': 'self'}, {'id': 'opponent'}], 'candidates': ['Fight', 'Wait']}, {})), \
             patch('streaming.AppServer') as model, patch('streaming.speech_synthesizer') as voice:
            run_job(job, 'codex-local', validate_action, None).result(1)
        self.assertEqual([event['type'] for event in job.events.queue], ['proposal', 'model_done', 'done'])
        diagnostics = list(job.events.queue)[1]['diagnostics']
        self.assertEqual(diagnostics['model_provider'], 'local_combat')
        self.assertEqual(diagnostics['model_name'], 'delivery-npc-qwen3')
        self.assertEqual(diagnostics['decision_route'], 'local_combat')
        self.assertEqual(diagnostics['fallback_reason'], '')
        self.assertEqual(diagnostics['input_event_count'], 1)
        self.assertEqual(diagnostics['input_subject_count'], 2)
        self.assertEqual(diagnostics['input_candidate_count'], 2)
        self.assertNotIn('first_sentence_ms', diagnostics)
        for field in ('input_tokens', 'cached_input_tokens', 'output_tokens'):
            self.assertNotIn(field, diagnostics, '本地接口没提供本轮计数时不编造 token')
        self.assertEqual(job.events.get_nowait()['proposal'], proposal)
        model.assert_not_called()
        voice.assert_not_called()

    def test_local_failure_falls_back_but_cancellation_does_not(self):
        for reason in ('timeout', 'cancelled'):
            with self.subTest(reason=reason):
                job = Job({**request(), 'stream_protocol_version': 2})
                calls = []

                class Model:
                    def stream(self, prompt, instructions, cancel, on_delta, **kwargs):
                        calls.append('luna')
                        on_delta(json.dumps({'candidate_index': 0, 'speech': '', 'emotion': 'Calm'}))

                with patch('streaming.choose_combat_action', return_value=CombatDecisionResult(fallback_reason=reason)), \
                     patch('streaming._local', SimpleNamespace(model=Model())), \
                     patch.dict(os.environ, {'DELIVERY_CODEX_MODEL': 'gpt-6-luna'}):
                    if reason == 'cancelled':
                        with self.assertRaisesRegex(RuntimeError, 'cancelled'):
                            run_job(job, 'codex-local', validate_action, None)
                        self.assertEqual(calls, [])
                        self.assertNotIn('proposal', [event['type'] for event in job.events.queue])
                        self.assertNotIn('model_done', [event['type'] for event in job.events.queue])
                    else:
                        run_job(job, 'codex-local', validate_action, None).result(1)
                        self.assertEqual(calls, ['luna'])
                        self.assertEqual([event['type'] for event in job.events.queue], ['proposal', 'model_done', 'done'])
                        diagnostics = list(job.events.queue)[1]['diagnostics']
                        self.assertEqual(diagnostics['model_provider'], 'codex-local')
                        self.assertEqual(diagnostics['model_name'], 'gpt-6-luna')
                        self.assertEqual(diagnostics['decision_route'], 'ambient_dialogue')
                        self.assertEqual(diagnostics['fallback_reason'], 'timeout')

    def test_diagnostics_distinguish_queue_model_and_request_time(self):
        job = Job({**request(), 'stream_protocol_version': 2, 'request_source': 'player'})
        job.created = 10.
        clock = [10.125]

        class Model:
            def stream(self, prompt, instructions, cancel, on_delta, **kwargs):
                clock[0] = 10.5
                on_delta(json.dumps({'candidate_index': 0, 'speech': '', 'emotion': 'Calm'}))

        with patch('streaming.time.monotonic', side_effect=lambda: clock[0]), \
             patch('streaming.choose_combat_action', return_value=CombatDecisionResult(fallback_reason='player_or_unknown_request')), \
             patch('streaming._local', SimpleNamespace(model=Model())), \
             patch.dict(os.environ, {'DELIVERY_CODEX_PLAYER_MODEL': 'gpt-6-luna'}):
            run_job(job, 'codex-local', validate_action, None).result(1)
        diagnostics = list(job.events.queue)[1]['diagnostics']
        self.assertEqual((diagnostics['queue_ms'], diagnostics['decision_ms'], diagnostics['backend_elapsed_ms']),
                         (125, 375, 500))
        self.assertEqual(diagnostics['request_source'], 'player')
        self.assertEqual(diagnostics['decision_route'], 'player_dialogue')
        self.assertNotIn('first_sentence_ms', diagnostics)

    def test_token_diagnostics_use_only_valid_last_counts_without_inventing_missing_fields(self):
        usage_cases = [None,
            {'last': {}, 'total': {'inputTokens': 999, 'outputTokens': 888}},
            {'last': {'inputTokens': 120, 'cachedInputTokens': 80, 'outputTokens': 0},
             'total': {'inputTokens': 999, 'cachedInputTokens': 900, 'outputTokens': 888}},
            {'last': {'inputTokens': -1, 'cachedInputTokens': True, 'outputTokens': 100_000_001}, 'total': {}}]
        for usage in usage_cases:
            with self.subTest(usage=usage):
                class Model:
                    last_token_usage = usage
                    def stream(self, prompt, instructions, cancel, on_delta, **kwargs):
                        on_delta(json.dumps({'candidate_index': 0, 'speech': '', 'emotion': 'Calm'}))
                job = Job({**request(), 'stream_protocol_version': 2})
                with patch('streaming._local', SimpleNamespace(model=Model())), \
                     patch.dict(os.environ, {'DELIVERY_CODEX_MODEL': 'gpt-6-luna'}):
                    run_job(job, 'codex-local', validate_action, None).result(1)
                diagnostics = list(job.events.queue)[1]['diagnostics']
                for source, field in (('inputTokens', 'input_tokens'), ('cachedInputTokens', 'cached_input_tokens'),
                                      ('outputTokens', 'output_tokens')):
                    value = (usage or {}).get('last', {}).get(source)
                    if type(value) is int and 0 <= value <= 100_000_000:
                        self.assertEqual(diagnostics[field], value)
                    else:
                        self.assertNotIn(field, diagnostics)

    def test_unknown_provider_does_not_claim_model_identity(self):
        job = Job({**request(), 'stream_protocol_version': 2})
        proposal = validate_action(job.context, {**job.context['candidates'][0], 'speech': '', 'emotion': 'Calm'})
        run_job(job, 'private-provider-name', validate_action, lambda *_: proposal).result(1)
        diagnostics = list(job.events.queue)[1]['diagnostics']
        self.assertEqual(diagnostics['model_provider'], 'unknown')
        self.assertEqual(diagnostics['model_name'], '')
        self.assertEqual(diagnostics['decision_route'], 'provider_dispatch')
        self.assertNotIn('input_event_count', diagnostics)
        self.assertNotIn('private-provider-name', json.dumps(diagnostics))

    def test_diagnostics_allow_only_public_model_names_and_never_return_private_values(self):
        unsafe = ['Saved/AIResearch/private-model.gguf', 'models/private-checkpoint',
                  'https://example.invalid/private-model', r'C:\private\model.gguf',
                  'sk-test-not-a-real-key', 'sk_test_not_a_real_key', 'not-in-public-model-list']
        for name in ['gpt-6-luna', *unsafe]:
            with self.subTest(name=name):
                job = Job({**request(), 'stream_protocol_version': 2})
                def api(prompt, cancel, on_delta, **kwargs):
                    on_delta(json.dumps({'candidate_index': 0, 'speech': '', 'emotion': 'Calm'}))
                with patch.dict(os.environ, {'DELIVERY_AI_MODEL': name,
                        'DELIVERY_AI_MODEL_URL': 'https://example.invalid/private-endpoint',
                        'DELIVERY_AI_API_KEY': 'synthetic-secret-value'}), \
                     patch('streaming.choose_combat_action', return_value=CombatDecisionResult(
                         fallback_reason='failure at C:/private/secret.txt?key=synthetic-secret-value')), \
                     patch('streaming.stream_api', side_effect=api):
                    run_job(job, 'llm', validate_action, None).result(1)
                diagnostics = list(job.events.queue)[1]['diagnostics']
                self.assertEqual(diagnostics['model_name'], name if name == 'gpt-6-luna' else '')
                self.assertEqual(diagnostics['fallback_reason'], 'model_error')
                rendered = json.dumps(diagnostics)
                for value in ('private-endpoint', 'synthetic-secret-value', 'secret.txt'):
                    self.assertNotIn(value, rendered)

    def test_logged_local_model_completes_with_real_combat_context(self):
        data = {**combat_request(), 'stream_protocol_version': 2}
        # 原候选带有其他动作，不能用它的下标读取筛选后的战斗候选
        data['candidates'] = [dict(action='LookAt', target_id=data['current_action']['target_id'],
                                   duration=2, evidence_ids=[]), *reversed(data['candidates'])]
        data['allowed_actions'].append('LookAt')
        seen = []

        def choose(client, prompt, candidates, cancel, measure):
            seen.append(prompt)
            return 'Fight'

        with tempfile.TemporaryDirectory(prefix='delivery-combat-log-') as folder:
            path = Path(folder) / 'dialogue.ndjson'
            with patch.dict(os.environ, {'DELIVERY_AI_DIALOGUE_LOG': '1',
                                         'DELIVERY_COMBAT_MODEL_URL': 'http://127.0.0.1:8771'}), \
                 patch.object(dialogue_log, '_path', path), \
                 patch('combat_model.LocalCombatClient.choose', autospec=True, side_effect=choose), \
                 patch('streaming.AppServer') as dialogue_model, patch('streaming.speech_synthesizer') as voice:
                job = Job(data)
                run_job(job, 'codex-local', validate_action, None).result(1)
            rows = [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()]
        self.assertEqual([event['type'] for event in job.events.queue], ['proposal', 'model_done', 'done'])
        self.assertFalse(job.cancel.is_set())
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['model_context'], seen[0])
        self.assertEqual(rows[0]['chosen'], 'Fight:opponent')
        self.assertEqual(rows[0]['current_action'], data['current_action'])
        self.assertEqual((rows[0]['npc_id'], rows[0]['world_epoch']), (data['npc_id'], data['world_epoch']))
        self.assertIs(rows[0]['pending_player_message'], False)
        self.assertEqual(rows[0]['evidence'][0]['who'], 'opponent')
        self.assertEqual(rows[0]['speech'], [])
        dialogue_model.assert_not_called()
        voice.assert_not_called()


class ReplyTextCompletionTests(unittest.TestCase):
    @contextmanager
    def reply(self, records, *, version=2, cancel_at=None, model_error=False,
              tts_error=False, provider='codex-local', fallback_text=''):
        class PendingAudio:
            def __init__(self):
                self.pending = []

            def submit(self, function, *args):
                future = concurrent.futures.Future()
                self.pending.append((future, function, args))
                return future

            def finish(self):
                pending, self.pending = self.pending, []
                for future, function, args in pending:
                    if not future.set_running_or_notify_cancel():
                        continue
                    try:
                        future.set_result(function(*args))
                    except Exception as error:
                        future.set_exception(error)

        job = Job({**request(), 'stream_protocol_version': version})
        audio = PendingAudio()
        voice_calls = []

        class Model:
            def stream(self, prompt, instructions, cancel, on_delta, **kwargs):
                if cancel_at == 0:
                    job.cancel_request()
                    raise RuntimeError('model cancelled')
                for index, record in enumerate(records, 1):
                    on_delta(json.dumps(record, ensure_ascii=False))
                    if cancel_at == index:
                        job.cancel_request()
                        raise RuntimeError('model cancelled')
                if model_error:
                    raise RuntimeError('model connection failed')

        class Voice:
            def synthesize(self, text, voice, emotion, cancel):
                voice_calls.append(text)
                if tts_error:
                    raise RuntimeError('fake TTS failure')
                return b'\x00\x00', 16000

        def fallback(context, selected_provider):
            return validate_action(context, {**context['candidates'][0],
                'speech': fallback_text, 'emotion': 'Calm'})

        with patch('streaming._local', SimpleNamespace(model=Model())), \
             patch('streaming.choose_combat_action', return_value=CombatDecisionResult()), \
             patch('streaming.uses_neural_speech', return_value=False), \
             patch('streaming._speech_pool', audio), \
             patch('streaming.speech_synthesizer', return_value=Voice()), \
             patch('streaming.neural_speech_synthesizer', side_effect=AssertionError('real TTS forbidden')), \
             patch('streaming.neural_speech_queue', side_effect=AssertionError('real speech queue forbidden')), \
             patch('streaming.AppServer', side_effect=AssertionError('real model forbidden')), \
             patch('streaming.stream_api', side_effect=AssertionError('real HTTP forbidden')), \
             patch('streaming.dialogue_log.record'):
            try:
                yield SimpleNamespace(job=job, audio=audio, voice_calls=voice_calls,
                    run=lambda: run_job(job, provider, validate_action, fallback))
            finally:
                if job.completion is not None and not job.completion.done():
                    job.cancel_request()
                audio.finish()

    def first_record(self, speech):
        return {'candidate_index': 0, 'speech': speech, 'emotion': 'Calm'}

    def model_event(self, job):
        events = [event for event in job.events.queue if event['type'] == 'model_done']
        self.assertEqual(len(events), 1)
        return events[0]

    def assert_finished_once(self, handle):
        handle.audio.finish()
        self.assertTrue(handle.job.completion.done())
        self.assertEqual(sum(event['type'] == 'done' for event in handle.job.events.queue), 1)
        self.assertEqual(sum(event['type'] == 'model_done' for event in handle.job.events.queue), 1)

    def test_reply_text_contains_first_and_followup_before_audio(self):
        first, following = '先把东西拿起来。然后跟着你。', ' 你慢慢走。'
        with self.reply([self.first_record(first), {'speech': following}]) as handle:
            completion = handle.run()
            before = list(handle.job.events.queue)
            self.assertEqual([event['type'] for event in before], ['proposal', 'model_done'])
            self.assertEqual(before[0]['proposal']['speech'], first)
            self.assertEqual(self.model_event(handle.job)['reply_text'], first + following)
            self.assertEqual(self.model_event(handle.job)['sentences'], 3)
            self.assertFalse(completion.done())
            self.assertEqual(handle.voice_calls, [])
            self.assert_finished_once(handle)
            self.assertEqual(''.join(handle.voice_calls), first + following)

    def test_invalid_followup_keeps_only_approved_prefix(self):
        first = '我拿着呢。'
        invalid = {'speech': '这一句不符合协议。', 'emotion': 'Angry'}
        with self.reply([self.first_record(first), invalid]) as handle:
            handle.run()
            self.assertEqual(self.model_event(handle.job)['reply_text'], first)
            self.assertEqual(self.model_event(handle.job)['sentences'], 1)
            self.assert_finished_once(handle)
            self.assertEqual(handle.voice_calls, [first])

    def test_silent_reply_has_empty_text_without_audio(self):
        with self.reply([self.first_record('')]) as handle:
            self.assertTrue(handle.run().done())
            event = self.model_event(handle.job)
            self.assertEqual((event['reply_text'], event['sentences']), ('', 0))
            self.assertEqual(handle.audio.pending, [])
            self.assertEqual(handle.voice_calls, [])
            self.assert_finished_once(handle)

    def test_cancel_before_or_after_first_sentence_never_finishes_successfully(self):
        for cancel_at in (0, 1):
            with self.subTest(cancel_at=cancel_at), \
                 self.reply([self.first_record('已经检查的首句。')], cancel_at=cancel_at) as handle:
                with self.assertRaises(RuntimeError):
                    handle.run()
                handle.audio.finish()
                types = [event['type'] for event in handle.job.events.queue]
                self.assertNotIn('model_done', types)
                self.assertNotIn('done', types)
                self.assertTrue(handle.job.completion.done())
                self.assertEqual(handle.voice_calls, [])
                for event in handle.job.events.queue:
                    self.assertIsNone(handle.job.event_for_client(event))

    def test_model_failure_after_valid_prefix_does_not_publish_reply_text(self):
        with self.reply([self.first_record('模型还没结束。')], model_error=True) as handle:
            with self.assertRaises(RuntimeError):
                handle.run()
            handle.audio.finish()
            types = [event['type'] for event in handle.job.events.queue]
            self.assertNotIn('model_done', types)
            self.assertNotIn('done', types)
            self.assertTrue(handle.job.completion.done())
            self.assertEqual(handle.voice_calls, [])

    def test_tts_failure_keeps_approved_text_separate_from_audio_result(self):
        text = '这句文字已经生成。'
        with self.reply([self.first_record(text)], tts_error=True) as handle:
            handle.run()
            self.assertEqual(self.model_event(handle.job)['reply_text'], text)
            self.assertEqual(handle.voice_calls, [])
            self.assert_finished_once(handle)
            failed = [event for event in handle.job.events.queue if event['type'] == 'sentence']
            self.assertEqual(len(failed), 1)
            self.assertEqual(failed[0]['audio_error'], 'RuntimeError')
            self.assertEqual(failed[0]['pcm'], '')
            self.assertEqual(self.model_event(handle.job)['reply_text'], text)

    def test_legacy_stream_has_no_new_completion_event(self):
        text = '旧协议仍然播这句。'
        with self.reply([self.first_record(text)], version=1) as handle:
            handle.run()
            self.assertEqual([event['type'] for event in handle.job.events.queue], ['proposal'])
            handle.audio.finish()
            self.assertEqual([event['type'] for event in handle.job.events.queue], ['proposal', 'sentence', 'done'])
            self.assertFalse(any('reply_text' in event for event in handle.job.events.queue))

    def test_existing_python_character_limit_is_preserved(self):
        for count in (120, 121):
            with self.subTest(count=count), self.reply([self.first_record('字' * count)]) as handle:
                if count == 120:
                    handle.run()
                    self.assertEqual(self.model_event(handle.job)['reply_text'], '字' * count)
                    self.assert_finished_once(handle)
                else:
                    with self.assertRaises(ValueError):
                        handle.run()
                    self.assertNotIn('model_done', [event['type'] for event in handle.job.events.queue])
                    self.assertEqual(handle.voice_calls, [])

    def test_fallback_uses_same_complete_text_collection(self):
        text = '第一句。 第二句。'
        with self.reply([], provider='fixture', fallback_text=text) as handle:
            handle.run()
            self.assertEqual(self.model_event(handle.job)['reply_text'], text)
            self.assert_finished_once(handle)
            self.assertEqual(''.join(handle.voice_calls), text)


if __name__ == '__main__':
    unittest.main()
