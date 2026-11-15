"""检查作者配置的街坊关系与人物表是否一致"""
from copy import deepcopy
import json
from pathlib import Path
import unittest
from unittest.mock import patch

from npc_relationships import (PROFILE_PATH, RELATIONSHIPS_PATH, TENDENCIES, _read_relationships,
                               character_relationships, prepare_relationships)


class NpcRelationshipsTests(unittest.TestCase):
    def setUp(self):
        _read_relationships.cache_clear()
        self.addCleanup(_read_relationships.cache_clear)
        self.profiles = json.loads(PROFILE_PATH.read_text(encoding='utf-8'))
        self.source = json.loads(RELATIONSHIPS_PATH.read_text(encoding='utf-8'))

    def author_sources(self, source, profiles=None):
        profiles = self.profiles if profiles is None else profiles
        return patch.object(Path, 'read_text', autospec=True,
                            side_effect=lambda path, **_: json.dumps(profiles if path == PROFILE_PATH else source))

    def test_each_resident_gets_its_two_directions_from_the_table(self):
        expected = {('chunlai', 'xiumei'): (90, 40, 85, 45), ('xiumei', 'chunlai'): (90, 35, 90, 45),
                    ('chunlai', 'ergou'): (45, 55, 15, 60), ('xiumei', 'ergou'): (50, 60, 10, 65),
                    ('ergou', 'chunlai'): (45, 50, 10, 55), ('ergou', 'xiumei'): (45, 60, 8, 60)}
        names = {row['id']: row['display_name'] for row in self.profiles['characters']}
        received = {}
        for profile_id in ('chunlai', 'xiumei', 'ergou'):
            relationships = character_relationships(profile_id)
            self.assertTrue(relationships['guidance'])
            for edge in relationships['edges']:
                self.assertEqual(set(edge), {'profile_id', 'display_name', 'description', *TENDENCIES})
                self.assertEqual(edge['display_name'], names[edge['profile_id']])
                received[profile_id, edge['profile_id']] = tuple(edge[key] for key in TENDENCIES)
        self.assertEqual(received, expected)

    def test_startup_load_is_reused_and_readers_cannot_overwrite_it(self):
        with self.author_sources(self.source) as read:
            prepare_relationships()
            result = character_relationships('chunlai')
            result['edges'][0]['trust'] = 100
            result['edges'].pop()
            checked = character_relationships('chunlai')
            self.assertEqual(len(checked['edges']), 2)
            self.assertEqual(checked['edges'][0]['trust'], 40)
            self.assertEqual(read.call_count, 2)

    def test_new_profile_without_edges_does_not_look_up_by_display_name(self):
        self.profiles['characters'].append({'id': 'future_npc', 'display_name': '大壮'})
        with self.author_sources(self.source):
            for unknown in ('future_npc', 'unknown', '大壮', '', None, ['chunlai']):
                self.assertEqual(character_relationships(unknown), [])
            self.assertTrue(character_relationships('chunlai'))

    def test_rejects_invalid_edges_during_preparation(self):
        adjustments = ({'other_profile_id': 'unknown'}, {'source_profile_id': 'unknown'},
                       {'other_profile_id': 'chunlai'}, {'trust': True}, {'trust': -1},
                       {'trust': 101}, {'description': ' '}, {'target_id': 'unsupported'})
        for change in adjustments:
            with self.subTest(change=change):
                invalid = deepcopy(self.source)
                invalid['edges'][0].update(change)
                with self.author_sources(invalid), self.assertRaises(ValueError):
                    prepare_relationships()
        self.source['edges'].append(self.source['edges'][0])
        with self.author_sources(self.source), self.assertRaisesRegex(ValueError, '关系重复'):
            prepare_relationships()

    def test_rejects_missing_endpoint_display_name(self):
        self.profiles['characters'][0]['display_name'] = ' '
        with self.author_sources(self.source), self.assertRaisesRegex(ValueError, '缺少显示名'):
            prepare_relationships()


if __name__ == '__main__':
    unittest.main()
