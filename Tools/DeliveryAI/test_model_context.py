"""检查短编号保留人物、事件、动作的对应关系，游戏继续收到原编号"""
import copy
import json
import unittest
from pathlib import Path

from benchmark_context import crowded_case
from review_dialogue import make_cases
from server import validate_action, validate_context
from streaming import compact_context, model_context, SentenceParser


class ModelContextTests(unittest.TestCase):
    def setUp(self):
        path = Path(__file__).resolve().parents[2] / 'Design/AI/NpcProfiles.json'
        self.profile = json.loads(path.read_text(encoding='utf-8'))['characters'][0]

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


if __name__ == '__main__':
    unittest.main()
