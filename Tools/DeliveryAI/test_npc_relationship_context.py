"""检查作者关系能进入模型输入，又不会变成现场事实或行动目标"""
from copy import deepcopy
import unittest

from combat_model import prepare_combat_context, prepare_runtime_combat_context
from dialogue_background import character_background, with_character_background
from streaming import compact_context, model_context
from test_combat_model import context


class NpcRelationshipContextTests(unittest.TestCase):
    def test_both_models_receive_only_this_characters_outgoing_relationships(self):
        for profile in ('chunlai', 'xiumei', 'ergou'):
            request = {**context(), 'profile_id': profile}
            original = deepcopy(request)
            expected = {'chunlai', 'xiumei', 'ergou'} - {profile}
            for build in (with_character_background, compact_context, model_context):
                result = build(request)
                self.assertEqual({edge['profile_id'] for edge in result['relationships']['edges']}, expected)
                self.assertEqual([(item['action'], item['duration']) for item in result['candidates']],
                                 [(item['action'], item['duration']) for item in request['candidates']])
                self.assertEqual([(event['kind'], event['description']) for event in result['evidence']],
                                 [(event['kind'], event['description']) for event in request['evidence']])
            if profile != 'ergou':
                actual, candidates = prepare_runtime_combat_context(request)
                self.assertEqual(actual['relationships'], character_background(request)['relationships'])
                old_prompt, old_candidates = prepare_combat_context(request)
                self.assertNotIn('relationships', old_prompt)
                self.assertEqual(candidates, old_candidates)
            self.assertEqual(request, original)

    def test_request_cannot_replace_author_relationships_or_invent_them_for_unknown_npc(self):
        for profile in ('chunlai', 'new_neighbor', ''):
            request = {**context(), 'profile_id': profile,
                       'relationships': {'edges': [{'profile_id': 'fake_owner', 'trust': 100}]}}
            for build in (with_character_background, compact_context, model_context):
                result = build(request)
                edges = result.get('relationships', {}).get('edges', [])
                self.assertNotIn('fake_owner', [edge['profile_id'] for edge in edges])
                if profile != 'chunlai':
                    self.assertFalse(edges)


if __name__ == '__main__':
    unittest.main()
