"""检查短编号保留人物、事件、动作的对应关系，游戏继续收到原编号"""
import copy
import json
import unittest
import uuid
from pathlib import Path

from benchmark_context import crowded_case
from review_dialogue import make_cases
from server import validate_action, validate_context
from streaming import compact_context, model_context, SentenceParser


class ModelContextTests(unittest.TestCase):
    def setUp(self):
        path = Path(__file__).resolve().parents[2] / 'Design/AI/NpcProfiles.json'
        self.profiles = json.loads(path.read_text(encoding='utf-8'))['characters']
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


if __name__ == '__main__':
    unittest.main()
