"""检查短编号保留人物、事件、动作的对应关系，游戏继续收到原编号"""
import copy
import json
import unittest
import uuid
from pathlib import Path

from benchmark_context import crowded_case
from dialogue_background import select_resident_profiles
from review_dialogue import make_cases
from server import validate_action, validate_context
from streaming import compact_context, model_context, SentenceParser
from test_companion_context import errand_request


class ModelContextTests(unittest.TestCase):
    def setUp(self):
        path = Path(__file__).resolve().parents[2] / 'Design/AI/NpcProfiles.json'
        self.profiles = select_resident_profiles(json.loads(path.read_text(encoding='utf-8')))
        self.profile = self.profiles[0]
        self.neighbor = next(row for row in self.profiles if row['id'] != self.profile['id'])

    def test_review_uses_other_profiles_current_name(self):
        for names in (None, ('街坊甲', '街坊乙')):
            current = copy.deepcopy(self.profiles)
            if names:
                for profile, name in zip(current, names):
                    profile['display_name'] = name
            for profile in current:
                with self.subTest(character=profile['display_name']):
                    neighbor = next(row for row in current if row['id'] != profile['id'])
                    context = dict(make_cases(profile, current))['named_provoke']
                    self.assertEqual(context['self_name'], profile['display_name'])
                    npc_names = {row['name'] for row in context['subjects'] if row['kind'] == 'npc'}
                    self.assertEqual(npc_names, {profile['display_name'], neighbor['display_name']})
                    for event in context['evidence']:
                        self.assertIn(neighbor['display_name'], event['description'])

    def test_relations_and_text_survive_number_changes(self):
        for name, context in make_cases(self.profile):
            with self.subTest(case=name):
                original = copy.deepcopy(context)
                source, encoded = compact_context(context), model_context(context)
                event_ids, subject_ids = {}, {}
                for before, after in zip(source['evidence'], encoded['evidence']):
                    event_ids[before['event_id']] = after['event_id']
                    if before.get('subject_id'):
                        subject_ids[before['subject_id']] = after['subject_id']
                    self.assertEqual(before['description'], after['description'])
                    self.assertEqual(before['kind'], after['kind'])
                self.assertEqual(len(set(event_ids.values())), len(event_ids))
                self.assertEqual(len(set(subject_ids.values())), len(subject_ids))
                for before, after in zip(source['evidence'], encoded['evidence']):
                    if before.get('reply_to_event_id'):
                        self.assertEqual(after['reply_to_event_id'], event_ids[before['reply_to_event_id']])
                for key in ('new_event_ids', 'recent_self_speech_ids'):
                    self.assertEqual(encoded[key], [event_ids[x] for x in source[key]])
                for key in ('current_player_message', 'pending_player_message'):
                    before, after = source[key], encoded[key]
                    if before is None:
                        self.assertIsNone(after)
                    else:
                        self.assertEqual(after['event_id'], event_ids[before['event_id']])
                        if before.get('subject_id'):
                            self.assertEqual(after['subject_id'], subject_ids[before['subject_id']])
                for before, after in zip(source['candidates'], encoded['candidates']):
                    self.assertEqual(before['action'], after['action'])
                    self.assertEqual(before['duration'], after['duration'])
                    self.assertEqual(after['target_id'], subject_ids.get(before['target_id'], ''))
                    self.assertEqual(after['evidence_ids'], [event_ids[x] for x in before['evidence_ids']])
                self.assertEqual(context, original, '准备模型输入不能修改服务器原请求')

    def test_late_vehicle_candidate_still_returns_original_game_identity(self):
        _, context = crowded_case(self.profile)
        validate_context(context)
        encoded = model_context(context)
        index = len(context['candidates'])-1
        self.assertGreater(index, 32)
        self.assertEqual(len(encoded['candidates']), len(context['candidates']))
        self.assertEqual(encoded['candidates'][index]['action'], 'HoldVehicle')
        self.assertLess(len(json.dumps(encoded)), len(json.dumps(compact_context(context))))
        received = []
        parser = SentenceParser(context, validate_action, lambda kind, value: received.append((kind, value)))
        parser.feed(json.dumps({'candidate_index':index, 'speech':'先把话说清楚。', 'emotion':'Angry'}, ensure_ascii=False))
        parser.finish()
        proposal = received[0][1]
        self.assertEqual(proposal['target_id'], context['candidates'][index]['target_id'])
        self.assertEqual(proposal['evidence_ids'], context['candidates'][index]['evidence_ids'])
        self.assertNotEqual(proposal['target_id'], encoded['candidates'][index]['target_id'])

    def test_names_speaker_and_addressee_survive_aliases(self):
        context = dict(make_cases(self.profile))['overheard_reply']
        original = copy.deepcopy(context)
        encoded = model_context(context)
        names = {item['subject_id']: item['name'] for item in encoded['subjects']}
        question, answer = encoded['evidence']
        self.assertEqual(names[encoded['self_subject_id']], self.profile['display_name'])
        self.assertEqual(names[question['subject_id']], '快递员')
        self.assertEqual(names[question['addressee_id']], self.neighbor['display_name'])
        self.assertEqual(answer['subject_id'], question['addressee_id'])
        self.assertEqual(answer['addressee_id'], question['subject_id'])
        self.assertEqual(answer['reply_to_event_id'], question['event_id'])
        self.assertIsNone(encoded['pending_player_message'], '旁听的提问不能要求这个 NPC 替另一个人回答')
        self.assertEqual(context, original)

    def test_reply_keeps_original_question_outside_recent_events(self):
        context = dict(make_cases(self.profile))['overheard_reply']
        question, answer = context['evidence']
        self_reply = {'event_id':str(uuid.uuid4()), 'subject_id':context['self_subject_id'],
                      'kind':'SelfSpeech', 'description':'听见了。', 'reply_to_event_id':''}
        unrelated = [{'event_id':str(uuid.uuid4()), 'subject_id':'', 'kind':'Seen', 'description':'路边没有变化'}
                     for _ in range(14)]
        context['evidence'] = [question, self_reply] + unrelated + [answer]
        context['candidates'] = [{'action':'Wait', 'target_id':'', 'duration':1, 'evidence_ids':[]}]
        compact = compact_context(context)
        self.assertIn(question['event_id'], [item['event_id'] for item in compact['evidence']])

    def test_reply_to_another_npc_does_not_mark_a_player_message_complete(self):
        context = dict(make_cases(self.profile))['repeated_insult']
        heard, own, latest = context['evidence']
        own['reply_to_event_id'] = heard['event_id']
        compact = compact_context(context)
        self.assertIsNone(compact['last_complete_player_reply_id'])
        self.assertEqual(compact['recent_self_speech_ids'], [own['event_id']])

    def test_neighbor_speech_during_playback_remains_new_until_next_decision(self):
        context = dict(make_cases(self.profile))['overheard_reply']
        question, answer = context['evidence']
        context['last_decision_event_ids'] = [question['event_id']]
        # 模型先看见提问，生成和播放期间听见邻居回答，自己的语音随后才播完
        own = {'event_id':str(uuid.uuid4()), 'subject_id':context['self_subject_id'],
               'kind':'SelfSpeech', 'description':'我听着呢。', 'reply_to_event_id':''}
        context['evidence'].append(own)
        compact = compact_context(context)
        self.assertIn(answer['event_id'], compact['new_event_ids'])
        self.assertNotIn(question['event_id'], compact['new_event_ids'])
        self.assertNotIn(own['event_id'], compact['new_event_ids'])
        # 下一次确实处理过这句话后，不会每轮都把它当作新话重复接下去
        context['last_decision_event_ids'] = [item['event_id'] for item in context['evidence']]
        self.assertEqual(compact_context(context)['new_event_ids'], [])

    def test_last_decision_events_allow_expiry_and_validate_identity(self):
        context = dict(make_cases(self.profile))['overheard_reply']
        for seen in ([], [str(uuid.uuid4())]):
            validate_context({**context, 'last_decision_event_ids':seen})
        event = str(uuid.uuid4())
        for seen in (None, event, [event, event], ['not-an-id'],
                     [str(uuid.uuid4()) for _ in range(33)]):
            with self.subTest(seen=seen), self.assertRaises(ValueError):
                validate_context({**context, 'last_decision_event_ids':seen})

    def test_fight_result_keeps_the_actual_outcome_and_does_not_complete_player_reply(self):
        context = dict(make_cases(self.profile))['named_provoke']
        neighbor = next(item for item in context['subjects']
                        if item['kind'] == 'npc' and item['subject_id'] != context['self_subject_id'])
        player = next(item for item in context['evidence'] if item['kind'] == 'PlayerSpeech')
        result = {'event_id':str(uuid.uuid4()), 'subject_id':context['self_subject_id'],
                  'addressee_id':neighbor['subject_id'], 'kind':'ActionFinished',
                  'source':'ActionResult', 'reply_to_event_id':player['event_id'],
                  'description':'Fight Succeeded；挥拳2次，命中0次；目标离开范围，已停止追赶'}
        context['evidence'].append(result)
        original = copy.deepcopy(context)
        encoded = model_context(context)
        outcome = encoded['evidence'][-1]
        names = {item['subject_id']:item['name'] for item in encoded['subjects']}
        self.assertEqual(outcome['description'], result['description'])
        self.assertEqual(outcome['source'], 'ActionResult')
        self.assertEqual(names[outcome['subject_id']], self.profile['display_name'])
        self.assertEqual(names[outcome['addressee_id']], neighbor['name'])
        self.assertEqual(encoded['pending_player_message']['event_id'], outcome['reply_to_event_id'])
        self.assertIsNone(encoded['last_complete_player_reply_id'])
        self.assertEqual(context, original)

    def test_optional_identity_fields_are_checked_without_breaking_old_clients(self):
        context = dict(make_cases(self.profile))['named_provoke']
        validate_context(context)
        legacy = {k:v for k,v in context.items() if k not in ('subjects', 'self_name', 'self_subject_id')}
        validate_context(legacy)
        mutations = [
            {'self_name':'大'*65}, {'self_subject_id':'not-a-guid'},
            {'subjects':context['subjects']*2}, {'subjects':[]},
            {'subjects':[{**context['subjects'][0], 'kind':'unknown'}]},
            {'evidence':[{**context['evidence'][0], 'addressee_id':'not-a-guid'}]},
        ]
        for mutation in mutations:
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                validate_context({**context, **mutation})

    def test_speech_target_returns_game_id_and_can_differ_from_action_target(self):
        context = dict(make_cases(self.profile))['named_provoke']
        encoded = model_context(context)
        neighbor = next(item for item in context['subjects']
                        if item['kind'] == 'npc' and item['subject_id'] != context['self_subject_id'])['subject_id']
        player = next(item for item in context['subjects'] if item['kind'] == 'player')['subject_id']
        player_alias = next(item for item in encoded['subjects'] if item['kind'] == 'player')['subject_id']
        candidate = next(index for index, item in enumerate(context['candidates'])
                         if item['action'] == 'LookAt' and item['target_id'] == neighbor)
        received = []
        parser = SentenceParser(context, validate_action, lambda kind, data: received.append((kind, data)))
        parser.feed(json.dumps({'candidate_index':candidate, 'speech':'你少挑事。',
                                'emotion':'Angry', 'speech_target_id':player_alias}, ensure_ascii=False))
        parser.finish()
        self.assertEqual(parser.proposal['target_id'], neighbor)
        self.assertEqual(parser.proposal['speech_target_id'], player)
        self.assertNotEqual(parser.proposal['speech_target_id'], player_alias)

    def test_invalid_speech_targets_never_emit_a_proposal(self):
        context = dict(make_cases(self.profile))['named_provoke']
        encoded = model_context(context)
        self_alias = encoded['self_subject_id']
        vehicle = next(item['subject_id'] for item in context['subjects'] if item['kind'] == 'vehicle')
        for target in (self_alias, vehicle, str(uuid.uuid4()), 'p999', None):
            emitted = []
            parser = SentenceParser(context, validate_action, lambda *data: emitted.append(data))
            with self.subTest(target=target), self.assertRaises(ValueError):
                parser.feed(json.dumps({'candidate_index':0, 'speech':'听见了。', 'emotion':'Calm',
                                        'speech_target_id':target}, ensure_ascii=False))
            self.assertEqual(emitted, [])

    def test_empty_and_omitted_speech_targets_have_different_meanings(self):
        context = dict(make_cases(self.profile))['named_provoke']
        for supplied in (False, True):
            item = {'candidate_index':0, 'speech':'这两个人。', 'emotion':'Calm'}
            if supplied:
                item['speech_target_id'] = ''
            parser = SentenceParser(context, validate_action, lambda *args: None)
            parser.feed(json.dumps(item, ensure_ascii=False))
            parser.finish()
            self.assertEqual('speech_target_id' in parser.proposal, supplied)

    def test_v3_errand_aliases_restore_owner_evidence_and_original_envelope(self):
        context = errand_request()
        owner = context['evidence'][0]['subject_id']
        context['self_subject_id'] = str(uuid.uuid4())
        context['subjects'] = [
            {'subject_id': context['self_subject_id'], 'name': '牛二狗', 'kind': 'npc'},
            {'subject_id': owner, 'name': '快递员甲', 'kind': 'player'}]
        context['companion_context'].update(has_package=True, owner_has_package=False,
            errand_action='DeliverPackage', errand_phase='GoToDelivery',
            errand_status='Failed', errand_reason='DeliveryTargetLost')
        validate_context(context)
        original = copy.deepcopy(context)
        encoded = model_context(context)
        owner_alias = next(item['subject_id'] for item in encoded['subjects'] if item['kind'] == 'player')
        selected = next(i for i, item in enumerate(encoded['candidates']) if item['action'] == 'ReturnPackage')
        self.assertEqual(encoded['companion_context'], context['companion_context'])
        self.assertEqual(encoded['candidates'][selected]['duration'], 0)
        self.assertEqual(encoded['candidates'][selected]['target_id'], '')
        self.assertNotEqual(encoded['candidates'][selected]['evidence_ids'], context['candidates'][selected]['evidence_ids'])
        parser = SentenceParser(context, validate_action, lambda *args: None)
        parser.feed(json.dumps({'candidate_index': selected, 'speech': '大哥，我把这件带回来。',
                                'emotion': 'Calm', 'speech_target_id': owner_alias}, ensure_ascii=False))
        parser.finish()
        self.assertEqual(parser.proposal['capability_version'], 3)
        self.assertEqual(parser.proposal['request_id'], context['request_id'])
        self.assertEqual(parser.proposal['speech_target_id'], owner)
        self.assertEqual(parser.proposal['action'], 'ReturnPackage')
        self.assertEqual(parser.proposal['evidence_ids'], context['candidates'][selected]['evidence_ids'])
        self.assertEqual(context, original)




class NpcMessageContextTests(unittest.TestCase):
    def request(self):
        from test_server import npc_message_context
        return npc_message_context()

    def receipt(self, request, reply_to):
        return {'event_id': str(uuid.uuid4()), 'kind': 'NpcMessageSent', 'source': 'ActionResult',
                'subject_id': request['self_subject_id'], 'addressee_id': request['evidence'][0]['subject_id'],
                'description': '我给你传过这句话。', 'needs_reply': False, 'reply_to_event_id': reply_to}

    def test_pending_and_cancelled_history_keep_their_actual_source(self):
        for state in (True, False, None):
            request = self.request()
            if state is None:
                request['evidence'][0].pop('needs_reply')
            else:
                request['evidence'][0]['needs_reply'] = state
            validate_context(request)
            original = copy.deepcopy(request)
            for encode in (compact_context, model_context):
                value = encode(request)
                message = value['evidence'][0]
                self.assertEqual(message['kind'], 'NpcMessage')
                self.assertEqual(message['source'], 'ToldBy')
                self.assertEqual(message['description'], original['evidence'][0]['description'])
                self.assertEqual(message['event_id'] in value['new_event_ids'], state is True)
                self.assertIsNone(value['pending_player_message'])
                self.assertEqual(value['recent_self_speech_ids'], [])
            self.assertEqual(request, original)

    def test_sent_receipt_preserves_original_message_beyond_recent_history(self):
        request = self.request()
        message = request['evidence'][0]
        receipt = self.receipt(request, message['event_id'])
        request['evidence'].append(receipt)
        request['evidence'].extend({'event_id': str(uuid.uuid4()), 'subject_id': '',
            'kind': 'VisibleNow', 'source': 'Seen', 'description': '身边的路还是那条路'} for _ in range(20))
        request['last_decision_event_ids'] = [e['event_id'] for e in request['evidence']]
        validate_context(request)
        compact = compact_context(request)
        self.assertIn(message['event_id'], [e['event_id'] for e in compact['evidence']])
        self.assertIn(receipt['event_id'], [e['event_id'] for e in compact['evidence']])
        self.assertEqual(compact['new_event_ids'], [])
        encoded = model_context(request)
        received = next(e for e in encoded['evidence'] if e['kind'] == 'NpcMessage')
        sent = next(e for e in encoded['evidence'] if e['kind'] == 'NpcMessageSent')
        self.assertEqual(sent['reply_to_event_id'], received['event_id'])
        self.assertEqual(sent['subject_id'], received['addressee_id'])
        self.assertEqual(sent['addressee_id'], received['subject_id'])
        self.assertFalse(sent['needs_reply'])

    def test_receipt_never_answers_player_even_with_bad_parent(self):
        request = self.request()
        player = {'event_id': str(uuid.uuid4()), 'subject_id': request['subjects'][2]['subject_id'],
                  'addressee_id': request['self_subject_id'], 'kind': 'PlayerSpeech', 'source': 'Heard',
                  'description': '先听我说。'}
        request['evidence'].append(player)
        request['evidence'].append(self.receipt(request, player['event_id']))
        # validate_context 会拒绝这种回执，压缩层同样不把它当玩家已答
        compact = compact_context(request)
        self.assertEqual(compact['pending_player_message']['event_id'], player['event_id'])
        self.assertFalse(compact['current_player_message']['has_complete_reply'])
        self.assertIsNone(compact['last_complete_player_reply_id'])
        self.assertNotIn(request['evidence'][-1]['event_id'], compact['new_event_ids'])

    def test_reply_receipt_is_not_a_new_question_in_both_request_forms(self):
        for modern in (True, False):
            request = self.request()
            message = request['evidence'][0]
            request['evidence'].append(self.receipt(request, message['event_id']))
            if not modern:
                request.pop('last_decision_event_ids')
            validate_context(request)
            compact = compact_context(request)
            self.assertEqual(compact['new_event_ids'], [])
            self.assertEqual(compact['recent_self_speech_ids'], [])
            self.assertIsNone(compact['last_complete_player_reply_id'])


if __name__ == '__main__':
    unittest.main()
