"""检查交谈日志保留实际模型输入，不连接模型或游戏"""
import copy
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import dialogue_log


class DialogueLogTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory(prefix='delivery-dialogue-log-')
        self.addCleanup(self.folder.cleanup)
        self.path = Path(self.folder.name) / 'logs' / 'dialogue.ndjson'
        self.addCleanup(patch.stopall)
        patch.object(dialogue_log, '_path', self.path).start()
        patch.dict(os.environ, {'DELIVERY_AI_DIALOGUE_LOG': '1'}).start()
        self.original = {'profile_id': 'xiumei', 'candidates': [
            {'action': 'Wait', 'target_id': '', 'duration': 1, 'evidence_ids': []},
            {'action': 'LookAt', 'target_id': 'neighbor-id', 'duration': 4, 'evidence_ids': ['event-id']},
        ]}
        self.context = {
            'persona': '你是翠花，村口退休大妈，脾气急，也要把对方的话听明白。',
            'self_name': '翠花', 'self_subject_id': 'p0',
            'subjects': [{'subject_id': 'p0', 'name': '翠花', 'kind': 'npc'},
                         {'subject_id': 'p1', 'name': '大壮', 'kind': 'npc'}],
            'background': {'shared': ['街坊在路边聊天。'], 'personal': ['你和大壮认识多年。']},
            'dialogue_examples': [{'heard': '你是不是说我怂？', 'reply': '我没说你怂啊！'}],
            'action_examples': [{'situation': '听到对方质问', 'action': 'LookAt',
                                 'target': 'neighbor', 'speech': '你先把话说清楚嘛。'}],
            'pending_player_message': None, 'new_event_ids': ['e0'],
            'evidence': [{'event_id': 'e0', 'subject_id': 'p1', 'addressee_id': 'p0',
                          'reply_to_event_id': 'e_previous', 'kind': 'Speech',
                          'source': 'Heard', 'description': '你是不是说我怂？'}],
            'candidates': [{'action': 'Wait', 'target_id': '', 'duration': 1, 'evidence_ids': []},
                           {'action': 'LookAt', 'target_id': 'p1', 'duration': 4, 'evidence_ids': ['e0']}],
        }
        self.proposal = {'action': 'LookAt', 'target_id': 'neighbor-id', 'emotion': 'Angry'}
        self.sentences = ['我没说你怂啊，别血口喷人！']

    def record(self, **options):
        dialogue_log.record('request-id', 'ambient', 'model-name', self.original,
                            self.context, self.proposal, self.sentences, **options)

    def test_disabled_log_creates_no_file_or_directory(self):
        with patch.dict(os.environ, {'DELIVERY_AI_DIALOGUE_LOG': ''}):
            self.record()
        self.assertFalse(self.path.parent.exists())

    def test_enabled_log_preserves_model_input_and_existing_fields(self):
        before = copy.deepcopy((self.original, self.context, self.proposal, self.sentences))
        self.record()
        rows = self.path.read_text(encoding='utf-8').splitlines()
        self.assertEqual(len(rows), 1)
        row = json.loads(rows[0])
        self.assertEqual(row['profile_id'], 'xiumei')
        self.assertEqual(row['full_persona'], self.context['persona'])
        self.assertEqual(row['model_context'], self.context)
        heard = row['model_context']['evidence'][0]
        self.assertEqual((heard['subject_id'], heard['addressee_id'], heard['reply_to_event_id']),
                         ('p1', 'p0', 'e_previous'))
        self.assertEqual(row['persona'], self.context['persona'][:12])
        self.assertEqual(row['chosen'], 'LookAt:p1')
        self.assertEqual(row['candidates'], ['Wait:-', 'LookAt:p1'])
        self.assertEqual(row['speech'], self.sentences)
        self.assertNotIn('instructions_sha256', row)
        self.assertEqual((self.original, self.context, self.proposal, self.sentences), before)

    def test_optional_instructions_record_only_the_hash(self):
        instructions = '按人物性格接话，不替对方决定。'
        self.record(instructions=instructions)
        text = self.path.read_text(encoding='utf-8')
        row = json.loads(text)
        self.assertEqual(row['instructions_sha256'],
                         hashlib.sha256(instructions.encode('utf-8')).hexdigest())
        self.assertNotIn(instructions, text)

    def test_failed_request_without_profile_or_context_can_still_be_recorded(self):
        dialogue_log.record('failed-id', 'player', 'model-name', {}, {}, None, [], error='TimeoutError')
        row = json.loads(self.path.read_text(encoding='utf-8'))
        self.assertEqual(row['profile_id'], '')
        self.assertEqual(row['full_persona'], '')
        self.assertEqual(row['model_context'], {})
        self.assertIsNone(row['chosen'])
        self.assertEqual(row['error'], 'TimeoutError')


if __name__ == '__main__':
    unittest.main()
