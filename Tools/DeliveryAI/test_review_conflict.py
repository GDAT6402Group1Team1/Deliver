"""只检查离线对照工具的事件和轮数，不调用模型或判断台词好坏"""
import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import review_conflict


PROFILES = {
    'version': 1,
    'world_background': [],
    'characters': [
        {'id': 'chunlai', 'display_name': '大壮', 'persona': '工具测试人物'},
        {'id': 'xiumei', 'display_name': '翠花', 'persona': '工具测试人物'},
    ],
}


class ReviewConflictTests(unittest.TestCase):
    def test_player_message_keeps_one_identity_and_its_addressee(self):
        scene = review_conflict.ConversationScene(PROFILES, 'greeting')
        event_id = scene.player_says('工具测试发言')
        direct, overheard = scene.history['xiumei'][-1], scene.history['chunlai'][-1]
        self.assertEqual(direct['event_id'], event_id)
        self.assertEqual(overheard['event_id'], event_id)
        self.assertEqual(direct['kind'], 'PlayerSpeech')
        self.assertEqual(overheard['kind'], 'OverheardSpeech')
        for event in (direct, overheard):
            self.assertEqual(event['subject_id'], scene.ids['player'])
            self.assertEqual(event['addressee_id'], scene.ids['xiumei'])

    def test_npc_reply_distinguishes_direct_speech_from_overhearing(self):
        scene = review_conflict.ConversationScene(PROFILES, 'greeting')
        for target, expected in (('chunlai', 'Speech'), ('player', 'OverheardSpeech')):
            with self.subTest(target=target):
                scene.npc_says('xiumei', scene.ids[target], '工具测试回复')
                own, heard = scene.history['xiumei'][-1], scene.history['chunlai'][-1]
                self.assertEqual(own['kind'], 'SelfSpeech')
                self.assertEqual(heard['kind'], expected)
                self.assertEqual(own['event_id'], heard['event_id'])
                self.assertEqual(heard['addressee_id'], scene.ids[target])
                self.assertEqual(heard['subject_id'], scene.ids['xiumei'])

    def test_new_control_scenarios_do_not_prewrite_an_apology_result(self):
        smalltalk = review_conflict.ConversationScene(PROFILES, 'smalltalk')
        deescalation = review_conflict.ConversationScene(PROFILES, 'deescalation')
        self.assertEqual(len(smalltalk.player_lines()), 2)
        self.assertEqual(smalltalk.fixture, [])
        self.assertEqual(len(deescalation.player_lines()), 2)
        self.assertEqual(len(deescalation.fixture), 2)
        self.assertTrue(all(item['source'] == 'authored_control' for item in deescalation.fixture))
        for events in deescalation.history.values():
            self.assertFalse(any(item['kind'] in ('PlayerSpeech', 'ActionReply', 'ActionFinished')
                                 for item in events))

    @staticmethod
    def scripted_reply(service, scene, speaker, player_round, followup, number, model, instructions):
        # 固定回复仅用于核对循环次数，报告不会被当作模型效果样本
        context = scene.context(speaker, number, followup == 0)
        target = scene.ids[scene.neighbor(speaker)]
        scene.seen[speaker] = [event['event_id'] for event in context['evidence']]
        scene.npc_says(speaker, target, '工具测试回复')
        return {'valid': True, 'call': number, 'player_round': player_round,
                'followup': followup, 'speaker': speaker, 'speech': ['工具测试回复'],
                'speech_target_id': target, 'proposal': {'action': 'LookAt', 'target_id': target}}

    def run_without_model(self, neighbor_replies, max_calls=12):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            profile_path = directory / 'profiles.json'
            profile_path.write_text(json.dumps(PROFILES, ensure_ascii=False), encoding='utf-8')
            args = SimpleNamespace(profiles=str(profile_path), instructions=None,
                                   output=str(directory / 'result.json'), variant='tool_test',
                                   scenario='paraphrase', model='unused', max_calls=max_calls,
                                   neighbor_replies=neighbor_replies)
            with patch.object(review_conflict, 'AppServer') as service, \
                    patch.object(review_conflict, 'evaluate_reply', side_effect=self.scripted_reply), \
                    contextlib.redirect_stdout(io.StringIO()):
                report = review_conflict.run(args)
                service.return_value.stream.assert_not_called()
                service.return_value.close.assert_called_once()
                return report

    def test_followup_limit_preserves_all_three_player_rounds(self):
        for neighbor_replies in range(4):
            with self.subTest(neighbor_replies=neighbor_replies):
                report = self.run_without_model(neighbor_replies)
                self.assertEqual(report['status'], 'no_attack_selected')
                self.assertEqual(len(report['calls']), 3 * (neighbor_replies + 1))
                self.assertEqual(report['limits']['player_rounds'], 3)
                self.assertEqual(report['limits']['npc_followups_per_player_round'], neighbor_replies)
                self.assertEqual({row['player_round'] for row in report['calls']}, {1, 2, 3})
                for player_round in (1, 2, 3):
                    calls = [row for row in report['calls'] if row['player_round'] == player_round]
                    self.assertEqual([row['followup'] for row in calls], list(range(neighbor_replies + 1)))

    def test_call_limit_does_not_report_the_third_player_round_as_completed(self):
        report = self.run_without_model(3, max_calls=8)
        self.assertEqual(report['status'], 'call_limit')
        self.assertEqual({row['player_round'] for row in report['calls']}, {1, 2})
        self.assertEqual(len(report['calls']), 8)
        self.assertIsNone(report['attack_selected'])


if __name__ == '__main__':
    unittest.main()
