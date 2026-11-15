"""检查小模型的范围、目标恢复和取消，测试不加载模型或显卡"""
from copy import deepcopy
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import threading
import time
import unittest
import uuid
from unittest.mock import patch

from combat_model import LocalCombatClient, choose_combat_action, parse_combat_choice, prepare_combat_context
from dialogue_background import character_background


def context():
    own, target, event = (str(uuid.uuid4()) for _ in range(3))
    return {'protocol_version': 1, 'capability_version': 1,
            **{key: str(uuid.uuid4()) for key in ('world_epoch', 'npc_id', 'request_id')},
            'context_revision': 2, 'plan_revision': 3, 'persona': '爱面子，也怕疼', 'profile_id': 'chunlai',
            'self_subject_id': own, 'self_name': '大壮', 'request_source': 'ambient', 'pending_player_message': False,
            'allowed_actions': ['Wait', 'Fight', 'Flee'],
            'subjects': [{'subject_id': own, 'name': '大壮', 'kind': 'npc'},
                         {'subject_id': target, 'name': '翠花', 'kind': 'npc'}],
            'evidence': [{'event_id': event, 'subject_id': target, 'kind': 'FightThreat',
                          'description': '这个人朝我冲过来，准备动手'}],
            'candidates': [{'action': action, 'target_id': '' if action == 'Wait' else target,
                            'duration': 3., 'evidence_ids': [] if action == 'Wait' else [event]}
                           for action in ('Fight', 'Flee', 'Wait')],
            'current_action': {'action': 'Fight', 'target_id': target, 'source': 'Model',
                               'remaining_seconds': 2., 'action_id': str(uuid.uuid4())}}


class FixedClient:
    def __init__(self, action='Fight'):
        self.action = action
        self.calls = 0

    def choose(self, prompt, candidates, cancel, measure):
        self.calls += 1
        return self.action


class CombatModelTests(unittest.TestCase):
    def test_combat_summary_uses_real_events_and_separate_current_action(self):
        request = context()
        request['current_action']['action'] = 'Flee'
        body = '这阵子实际挨了 1 记轻拳；上一轮交战挥拳 2 次，实际命中 0 次。现在站着，可以行动。'
        for ending in ('追近、出拳和受击仍在继续；可决定继续、逃跑或停手',
                       '当前动作还未结束；下一步可以继续、逃跑或停手'):
            with self.subTest(ending=ending):
                summary = {'kind': 'SelfCombat', 'subject_id': request['self_subject_id'],
                           'description': body + '；我正在对翠花执行Flee，剩余约2.0秒。' + ending}
                request['evidence'].append(summary)
                original = deepcopy(request)
                prompt, _ = prepare_combat_context(request)
                self.assertEqual(prompt['events'][-1]['description'], body)
                self.assertEqual(prompt['events'][0]['description'], request['evidence'][0]['description'])
                self.assertEqual(prompt['current_action']['action'], 'Flee')
                self.assertEqual(request, original)
                request['evidence'].pop()

    def test_summary_cleanup_does_not_change_speech_hits_or_other_actions(self):
        request = context()
        suffix = '；我正在对翠花执行Fight，剩余约2.0秒。追近、出拳和受击仍在继续；可决定继续、逃跑或停手'
        for kind, subject, text in (
                ('Speech', request['self_subject_id'], suffix),
                ('LightMeleeHit', request['current_action']['target_id'], '这个人打了我一拳'),
                ('SelfCombat', request['current_action']['target_id'], suffix),
                ('SelfCombat', request['self_subject_id'], suffix.replace('翠花', '旁人')),
                ('SelfCombat', request['self_subject_id'], suffix.replace('Fight', 'Flee')),
                ('SelfCombat', request['self_subject_id'], '刚挨打，正在后仰')):
            with self.subTest(kind=kind, text=text):
                request['evidence'] = [{'kind': kind, 'subject_id': subject, 'description': text}]
                prompt, _ = prepare_combat_context(request)
                self.assertEqual(prompt['events'][0]['description'], text)

    def test_only_continuing_model_action_is_eligible(self):
        base = context()
        changes = [{'request_source': 'player'}, {'pending_player_message': True},
                   {'pending_player_message': None}, {'current_action': None},
                   {'current_action': {**base['current_action'], 'source': 'GameplayRule'}},
                   {'current_action': {**base['current_action'], 'action': 'Wait'}},
                   {'current_action': {**base['current_action'], 'remaining_seconds': 0}},
                   {'current_action': {**base['current_action'], 'remaining_seconds': float('nan')}}]
        for change in changes:
            client = FixedClient()
            with self.subTest(change=change):
                reply = choose_combat_action({**base, **change}, threading.Event(), client=client)
                self.assertIsNone(reply.proposal)
                self.assertTrue(reply.fallback_reason)
                self.assertEqual(client.calls, 0)

    def test_choice_preserves_candidate_and_request_identity_without_mutation(self):
        request = context()
        original = deepcopy(request)
        result = choose_combat_action(request, threading.Event(), client=FixedClient('Flee'))
        self.assertEqual(result.proposal['target_id'], request['current_action']['target_id'])
        self.assertEqual(result.proposal['request_id'], request['request_id'])
        self.assertEqual(result.proposal['duration'], 3.)
        self.assertEqual(result.proposal['speech'], '')
        self.assertEqual(result.proposal['speech_target_id'], '')
        self.assertEqual(request, original)

    def test_npc_message_needing_a_reply_uses_dialogue_during_combat(self):
        request = context()
        own = request['self_subject_id']
        target = request['current_action']['target_id']
        message = {'kind': 'NpcMessage', 'source': 'ToldBy', 'event_id': str(uuid.uuid4()),
                   'subject_id': target, 'addressee_id': own, 'needs_reply': True,
                   'description': '我刚才那句是冲他说的，你急什么？'}
        receipt = {'source': 'ActionResult', 'event_id': str(uuid.uuid4()),
                   'subject_id': own, 'addressee_id': target, 'description': '你早说啊。'}
        unrelated_reply = {**receipt, 'kind': 'SelfSpeech',
                           'reply_to_event_id': request['evidence'][0]['event_id']}
        for action in ('Fight', 'Flee'):
            with self.subTest(action=action):
                request['current_action']['action'] = action
                pending = {**request, 'evidence': request['evidence'] + [message, unrelated_reply]}
                client = FixedClient()
                result = choose_combat_action(pending, threading.Event(), client=client)
                self.assertIsNone(result.proposal)
                self.assertEqual(result.fallback_reason, 'pending_npc_reply')
                self.assertEqual(client.calls, 0)

        old_message = dict(message)
        old_message.pop('needs_reply')
        histories = {'旧协议消息': [old_message], '无需接话': [{**message, 'needs_reply': False}]}
        for kind in ('SelfSpeech', 'ActionReply', 'NpcMessageSent'):
            histories[kind] = [message, {**receipt, 'kind': kind, 'reply_to_event_id': message['event_id']}]
        for description, history in histories.items():
            with self.subTest(description=description):
                completed = {**request, 'evidence': request['evidence'] + history}
                client = FixedClient('Flee')
                result = choose_combat_action(completed, threading.Event(), client=client)
                self.assertEqual(client.calls, 1)
                self.assertEqual(result.proposal['action'], 'Flee')

    def test_live_model_receives_author_preferences_without_changing_fixed_evaluation(self):
        class ObservedClient(FixedClient):
            def choose(self, prompt, candidates, cancel, measure):
                self.prompt = prompt
                return super().choose(prompt, candidates, cancel, measure)

        for profile in ('chunlai', 'xiumei'):
            with self.subTest(profile=profile):
                request = {**context(), 'profile_id': profile, 'combat_preferences': {'protect_owner': 999}}
                historical, _ = prepare_combat_context(request)
                client = ObservedClient()
                result = choose_combat_action(request, threading.Event(), client=client)
                expected = character_background(request)['combat_preferences']
                self.assertEqual(result.proposal['action'], 'Fight')
                self.assertNotIn('combat_preferences', historical)
                self.assertNotIn('relationships', historical)
                self.assertEqual(client.prompt, {**historical, 'combat_preferences': expected,
                                                'relationships': character_background(request)['relationships']})

    def test_new_opponent_and_duplicate_pair_are_not_selected(self):
        request = context()
        request['candidates'].append({**request['candidates'][0], 'target_id': str(uuid.uuid4())})
        _, candidates = prepare_combat_context(request)
        self.assertEqual(candidates['Fight']['target_id'], request['current_action']['target_id'])
        request['candidates'].append(dict(request['candidates'][0]))
        with self.assertRaisesRegex(ValueError, 'ambiguous'):
            prepare_combat_context(request)

    def test_schema_choice_rejects_wrong_target_extra_text_and_extra_fields(self):
        candidates = {'Fight': {}, 'Wait': {}}
        self.assertEqual(parse_combat_choice('{"action":"Fight","target_id":"opponent"}', candidates), 'Fight')
        for text in ('{"action":"Fight","target_id":""}', '{"action":"Wait","target_id":"opponent"}',
                     '{"action":"Flee","target_id":"opponent"}', '{"action":"Wait","target_id":"","speech":"hi"}'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                parse_combat_choice(text, candidates)

    def test_cancelled_result_does_not_return_late_action(self):
        cancelled = threading.Event()
        class CancelsWhileChoosing(FixedClient):
            def choose(self, *_args):
                cancelled.set()
                return 'Fight'
        result = choose_combat_action(context(), cancelled, client=CancelsWhileChoosing())
        self.assertIsNone(result.proposal)
        self.assertEqual(result.fallback_reason, 'cancelled')

    def test_compact_model_can_only_choose_a_complete_offered_action(self):
        class CompletionHandler(BaseHTTPRequestHandler):
            result = {'content': 'Flee', 'stop_type': 'eos', 'truncated': False}
            calls = []

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                self.calls.append((self.path, body))
                if self.path == '/apply-template':
                    response = {'prompt': 'template'}
                elif self.path == '/tokenize':
                    response = {'tokens': [7] if body['content'] == 'template' else [1, 2, 3]}
                else:
                    response = self.result
                self.send_response(200)
                self.end_headers()
                self.wfile.write(json.dumps(response).encode())

            def log_message(self, *_args):
                pass

        server = ThreadingHTTPServer(('127.0.0.1', 0), CompletionHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            client = LocalCombatClient(f'http://127.0.0.1:{server.server_port}', compact_output=True)
            request = context()
            request['candidates'] = [row for row in request['candidates'] if row['action'] != 'Fight']
            reply = choose_combat_action(request, threading.Event(), client=client)
            self.assertEqual(reply.proposal['action'], 'Flee')
            self.assertEqual(reply.proposal['target_id'], request['current_action']['target_id'])
            prefix = CompletionHandler.calls[-2]
            self.assertEqual(prefix, ('/tokenize', {'content': '{"action":"', 'add_special': False}))
            path, body = CompletionHandler.calls[-1]
            self.assertEqual(path, '/completion')
            self.assertEqual(body['prompt'], [7, 1, 2, 3])
            self.assertEqual(body['grammar'], 'root ::= "Flee" | "Wait"')
            self.assertNotIn('id_slot', body)
            for mutation in ({'content': 'Fight'}, {'content': 'Flee\n'}, {'content': 'F', 'stop_type': 'limit'},
                             {'content': 'Flee', 'stop_type': 'limit'}, {'truncated': True}):
                with self.subTest(mutation=mutation):
                    CompletionHandler.result = {'content': 'Flee', 'stop_type': 'eos', 'truncated': False, **mutation}
                    reply = choose_combat_action(request, threading.Event(), client=client)
                    self.assertIsNone(reply.proposal)
                    self.assertIn(reply.fallback_reason, ('invalid_combat_choice', 'incomplete_combat_choice'))

            CompletionHandler.result = {'content': 'Flee', 'stop_type': 'eos', 'truncated': False}
            with patch.dict('os.environ', {'DELIVERY_COMBAT_MODEL_URL': f'http://127.0.0.1:{server.server_port}',
                                           'DELIVERY_COMBAT_MODEL_RESIDENT_CACHE': '1'}, clear=True):
                for profile, slot in (('chunlai', 0), ('xiumei', 1), ('chunlai', 0)):
                    with self.subTest(profile=profile):
                        request.update(profile_id=profile, self_name='改过显示名', id_slot=9)
                        CompletionHandler.calls.clear()
                        reply = choose_combat_action(request, threading.Event())
                        self.assertEqual(reply.proposal['target_id'], request['current_action']['target_id'])
                        self.assertEqual(CompletionHandler.calls[-1][1]['id_slot'], slot)
                        self.assertTrue(all('id_slot' not in payload for _, payload in CompletionHandler.calls[:-1]))
            for invalid_slot in (True, -1, 2, '0'):
                with self.subTest(invalid_slot=invalid_slot), self.assertRaisesRegex(ValueError, 'invalid_combat_cache_slot'):
                    client.choose({}, {'Wait': {}}, threading.Event(), cache_slot=invalid_slot)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(1)

    def test_invalid_original_candidate_is_still_rejected(self):
        request = context()
        request['candidates'][0]['duration'] = 99
        result = choose_combat_action(request, threading.Event(), client=FixedClient())
        self.assertIsNone(result.proposal)
        self.assertIn('duration', result.fallback_reason)

    def test_connection_close_slow_body_obeys_total_deadline(self):
        class SlowHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                self.rfile.read(int(self.headers['Content-Length']))
                self.send_response(200)
                self.send_header('Connection', 'close')
                self.end_headers()
                try:
                    for byte in b'{"prompt":"slow body"}':
                        self.wfile.write(bytes([byte]))
                        self.wfile.flush()
                        time.sleep(.04)
                except (BrokenPipeError, ConnectionResetError, OSError):
                    pass
            def log_message(self, *_args):
                pass
        server = ThreadingHTTPServer(('127.0.0.1', 0), SlowHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            client = LocalCombatClient(f'http://127.0.0.1:{server.server_port}', timeout_seconds=.12)
            prompt, candidates = prepare_combat_context(context())
            started = time.monotonic()
            with self.assertRaises(TimeoutError):
                client.choose(prompt, candidates, threading.Event())
            self.assertLess(time.monotonic()-started, .4)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(1)

    def test_disabled_configuration_does_not_contact_a_service(self):
        with patch.dict('os.environ', {}, clear=True):
            result = choose_combat_action(context(), threading.Event())
        self.assertEqual(result.fallback_reason, 'disabled')

    def test_other_profiles_and_companion_cannot_use_resident_training(self):
        for mutation in ({'profile_id': 'ergou'}, {'profile_id': ''}, {'profile_id': 'new_npc'},
                         {'profile_id': 'chunlai', 'companion_context': {}}):
            client = FixedClient()
            result = choose_combat_action({**context(), **mutation}, threading.Event(), client=client)
            self.assertIsNone(result.proposal)
            self.assertEqual(client.calls, 0)
        with patch.dict('os.environ', {'DELIVERY_COMBAT_MODEL_PROFILE_IDS': 'xiumei'}):
            result = choose_combat_action(context(), threading.Event(), client=FixedClient())
            self.assertEqual(result.fallback_reason, 'profile_not_in_training_scope')

    def test_timeout_keeps_local_slot_reserved_until_server_confirms_idle(self):
        idle = threading.Event()
        class TimedOutClient(LocalCombatClient):
            def choose(self, *_args):
                self.requires_drain = True
                raise TimeoutError('test_timeout')
            def is_idle(self):
                return idle.is_set()
        client = TimedOutClient('http://127.0.0.1:8771')
        result = choose_combat_action(context(), threading.Event(), client=client)
        self.assertEqual(result.fallback_reason, 'timeout')
        try:
            waiting = choose_combat_action(context(), threading.Event(), client=FixedClient())
            self.assertEqual(waiting.fallback_reason, 'busy')
        finally:
            idle.set()
        deadline = time.monotonic()+1
        while time.monotonic() < deadline:
            available = choose_combat_action(context(), threading.Event(), client=FixedClient())
            if available.proposal:
                break
            time.sleep(.02)
        self.assertIsNotNone(available.proposal)


if __name__ == '__main__':
    unittest.main()
