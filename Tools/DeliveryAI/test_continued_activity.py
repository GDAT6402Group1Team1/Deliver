"""v6 活动继续协议离线回归，不启动模型、音频、服务或 UE"""
import copy
import json
import unittest

from test_home_context import resident_request, home_request, new_id
from server import validate_context, validate_action
from streaming import SentenceParser, model_context
from continued_activity import continuation_instructions


def current_request():
    data = resident_request()
    data.update(capability_version=6, pending_player_message=True, stream_protocol_version=2)
    target = data['evidence'][0]['subject_id']
    data['subjects'] = [{'subject_id': target, 'name': '快递员', 'kind': 'player'}]
    data['current_action'] = dict(action='Fight', action_id=new_id(), target_id=target,
                                  source='Model', remaining_seconds=3, plan_revision=2)
    data['allowed_actions'].append('ContinueCurrent')
    data['candidates'].append(dict(candidate_id=new_id(), action='ContinueCurrent',
                                   target_id='', duration=0, evidence_ids=[]))
    return data


class ContinuedActivityTests(unittest.TestCase):
    def test_continuation_keeps_exact_candidate_without_mutating_activity(self):
        context = current_request()
        before = copy.deepcopy(context)
        validate_context(context)
        result = validate_action(context, {**context['candidates'][-1], 'speech': '知道了，我先忙着。'})
        self.assertEqual(result['candidate_id'], context['candidates'][-1]['candidate_id'])
        self.assertEqual(result['duration'], 0)
        self.assertEqual(context, before)

    def test_old_versions_still_parse_but_cannot_advertise_continue(self):
        for version in range(1, 6):
            data = resident_request(version)
            with self.subTest(version=version):
                validate_context(data)
                data['allowed_actions'].append('ContinueCurrent')
                with self.assertRaisesRegex(ValueError, 'version 6'):
                    validate_context(data)

    def test_stale_candidate_and_activity_rewrite_are_rejected(self):
        context = current_request()
        original = {**context['candidates'][-1], 'speech': '你说，我听着。'}
        for changes in ({'candidate_id': new_id()}, {'duration': 1}, {'target_id': new_id()},
                        {'evidence_ids': [context['evidence'][0]['event_id']]}, {'capability_version': 5}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                validate_action(context, {**original, **changes})

    def test_ongoing_activity_is_required(self):
        data = current_request()
        del data['current_action']
        with self.assertRaisesRegex(ValueError, 'requires current activity'):
            validate_context(data)

    def test_pending_player_requires_words_even_when_continuing(self):
        context = current_request()
        for speech in ('', '  ', None):
            with self.subTest(speech=speech), self.assertRaisesRegex(ValueError, 'player reply'):
                validate_action(context, {**context['candidates'][-1], 'speech': speech})
        self.assertEqual(validate_action(context, {**context['candidates'][-1], 'speech': '好'})['speech'], '好')
        context['pending_player_message'] = False
        context['request_source'] = 'ambient'
        self.assertEqual(validate_action(context, context['candidates'][-1])['speech'], '')

    def test_home_arrangement_can_continue_without_reissuing_home_action(self):
        data = home_request()
        data.update(capability_version=6, pending_player_message=True)
        data['current_action'] = dict(action='HomeAction', action_id='', target_id='',
                                      source='Companion', remaining_seconds=0, plan_revision=7)
        data['current_activity'] = dict(kind='companion', mode='Stay', errand_action='',
                                        errand_phase='', has_home_arrangement=True)
        candidate = dict(candidate_id=new_id(), action='ContinueCurrent', target_id='', duration=0, evidence_ids=[])
        data['allowed_actions'].append('ContinueCurrent')
        data['candidates'].append(candidate)
        before = copy.deepcopy(data)
        validate_context(data)
        decision = validate_action(data, {**candidate, 'speech': '我摆好这个再聊。'})
        self.assertNotIn('home', decision)
        self.assertEqual(data, before)

    def test_compact_prompt_keeps_activity_and_aliases_target_consistently(self):
        data = current_request()
        before = copy.deepcopy(data)
        prompt = model_context(data)
        self.assertEqual(prompt['current_action']['action'], 'Fight')
        self.assertEqual(prompt['current_action']['target_id'], prompt['subjects'][0]['subject_id'])
        self.assertEqual(prompt['current_action']['remaining_seconds'], 3)
        self.assertIn('不重新开始动作', continuation_instructions(data))
        self.assertEqual(data, before)

    def test_streaming_first_sentence_still_validates_exact_v6_candidate(self):
        data = current_request()
        events = []
        parser = SentenceParser(data, validate_action, lambda kind, value: events.append((kind, value)))
        parser.feed(json.dumps(dict(candidate_index=1, speech='你说，我听着。', emotion='Calm'), ensure_ascii=False) + '\n')
        parser.finish()
        self.assertEqual(events[0][0], 'proposal')
        self.assertEqual(events[0][1]['candidate_id'], data['candidates'][1]['candidate_id'])
        self.assertEqual(events[0][1]['capability_version'], 6)
        self.assertEqual(events[1][0], 'sentence')

    def test_v6_activity_malformed_snapshot_is_rejected(self):
        data = current_request()
        for patch in ({'source': 'unknown'}, {'action_id': ''}, {'remaining_seconds': float('nan')},
                      {'plan_revision': True}, {'action': 'ContinueCurrent'}):
            with self.subTest(patch=patch), self.assertRaises(ValueError):
                validate_context({**data, 'current_action': {**data['current_action'], **patch}})


if __name__ == '__main__':
    unittest.main()
