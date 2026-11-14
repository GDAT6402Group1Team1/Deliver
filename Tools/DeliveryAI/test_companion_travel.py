"""出行协议离线检查，只读写内存样例和假模型回复，不连接服务或启动模型"""
import copy
import json
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from codex_local import choose
from companion_context import TRAVEL_INSTRUCTIONS, companion_instructions
from server import validate_action, validate_context
from streaming import SentenceParser, compact_context, model_context
from test_home_context import home_request, new_id, resident_request


def travel_request(action='Follow', modes=(None, 'Walk', 'OwnVehicle', 'Automatic')):
    data = home_request()
    data['capability_version'] = 8
    data['companion_context'].update(
        travel_mode='Unspecified', own_vehicle_available=True,
        task_description='把面馆的外卖交到爷爷手里。', held_item_id='', held_item_name='')
    data['allowed_actions'] = [action]
    data['candidates'] = []
    for mode in modes:
        candidate = dict(candidate_id=new_id(), action=action, target_id='', duration=0,
                         evidence_ids=[data['evidence'][0]['event_id']])
        if mode is not None:
            candidate['travel_mode'] = mode
        data['candidates'].append(candidate)
    return data


def item_request(action, version):
    data = travel_request(action, modes=(None,))
    data['capability_version'] = version
    # 已约定走路时，无出行字段的物品候选表示沿用；未约定时不虚构这类候选
    if version >= 8:
        data['companion_context']['travel_mode'] = 'Walk'
    if version < 8:
        for field in ('travel_mode', 'own_vehicle_available', 'task_description'):
            del data['companion_context'][field]
    item = new_id()
    data['subjects'] = [{'subject_id': item, 'kind': 'other', 'name': '小锤子'}]
    fact = dict(event_id=new_id(), subject_id=item,
                kind='WorldItemSeen' if action == 'FetchItem' else 'CompanionHoldingItem',
                source='Seen' if action == 'FetchItem' else 'ActionResult',
                description='这把小锤子在面前。' if action == 'FetchItem' else '我手里拿着这把小锤子。')
    data['evidence'].append(fact)
    data['candidates'][0].update(target_id=item, evidence_ids=[fact['event_id']])
    if action == 'ReturnItem':
        data['companion_context'].update(held_item_id=item, held_item_name='小锤子')
    return data


def add_continuation(data, current_action, target=''):
    data['pending_player_message'] = True
    data['current_action'] = dict(action=current_action, action_id='', target_id=target,
                                  source='Companion', remaining_seconds=0, plan_revision=7)
    data['current_activity'] = dict(kind='companion', mode='Stay', errand_action=current_action,
                                    errand_phase='Approaching', has_home_arrangement=current_action == 'HomeAction')
    data['allowed_actions'].append('ContinueCurrent')
    data['candidates'].append(dict(candidate_id=new_id(), action='ContinueCurrent',
                                   target_id='', duration=0, evidence_ids=[]))


class CompanionTravelTests(unittest.TestCase):
    def test_v8_requires_typed_travel_vehicle_and_order_fields(self):
        data = travel_request()
        validate_context(data)
        for field in ('travel_mode', 'own_vehicle_available', 'task_description'):
            invalid = copy.deepcopy(data)
            del invalid['companion_context'][field]
            with self.subTest(missing=field), self.assertRaises(ValueError):
                validate_context(invalid)
        for field, values in (
                ('travel_mode', (None, True, '', 'DrivePlayersBike', [])),
                ('own_vehicle_available', (None, 1, 'true')),
                ('task_description', (None, 1, [], '面' * 513))):
            for value in values:
                invalid = copy.deepcopy(data)
                invalid['companion_context'][field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    validate_context(invalid)
        for description in ('', '面' * 512):
            valid = copy.deepcopy(data)
            valid['companion_context']['task_description'] = description
            validate_context(valid)

    def test_modes_and_unchanged_choice_keep_distinct_candidate_ids(self):
        # 尚未约定出行方式时，UE 只为 Follow / TakePackage 保留无方式候选
        for action in ('Follow', 'TakePackage'):
            data = travel_request(action)
            before = copy.deepcopy(data)
            validate_context(data)
            for candidate in data['candidates']:
                for include_echo in (False, True):
                    selected = dict(candidate)
                    if not include_echo:
                        selected.pop('travel_mode', None)
                    with self.subTest(action=action, mode=candidate.get('travel_mode'), echo=include_echo):
                        result = validate_action(data, selected)
                        self.assertEqual(result['candidate_id'], candidate['candidate_id'])
                        self.assertEqual(result['capability_version'], 8)
                        self.assertEqual(result['duration'], 0)
                        # UE 用编号读取自己的候选，后端不另外传一套执行参数
                        self.assertNotIn('travel_mode', result)
            self.assertEqual(data, before)

    def test_other_travel_actions_accept_only_the_offered_modes(self):
        for action in ('FetchPackage', 'DeliverPackage', 'ReturnPackage', 'GoHome'):
            data = travel_request(action, modes=('Walk', 'OwnVehicle', 'Automatic'))
            validate_context(data)
            for candidate in data['candidates']:
                with self.subTest(action=action, mode=candidate['travel_mode']):
                    self.assertEqual(validate_action(data, candidate)['candidate_id'], candidate['candidate_id'])
        for action in ('FetchItem', 'ReturnItem'):
            data = item_request(action, 8)
            base = data['candidates'][0]
            data['candidates'] = [{**base, 'candidate_id': new_id(), 'travel_mode': mode}
                                  for mode in ('Walk', 'OwnVehicle', 'Automatic')]
            validate_context(data)
            for candidate in data['candidates']:
                with self.subTest(action=action, mode=candidate['travel_mode']):
                    self.assertEqual(validate_action(data, candidate)['candidate_id'], candidate['candidate_id'])

    def test_candidate_mode_must_be_explicit_valid_and_supported_by_action(self):
        for mode in ('Unspecified', '', None, True, 'Teleport'):
            data = travel_request(modes=(mode,))
            # None 在样例工具里表示缺省，这里专门构造显式 null
            data['candidates'][0]['travel_mode'] = mode
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                validate_context(data)
        for action in ('Wait', 'Stay', 'CancelCompanionTask', 'OfferCaterpillar', 'ContinueCurrent'):
            data = travel_request(action, modes=('Walk',))
            if action == 'Wait':
                data['candidates'][0]['duration'] = 1
            if action == 'ContinueCurrent':
                data['current_activity'] = dict(kind='companion', mode='Stay', errand_action='',
                                                errand_phase='', has_home_arrangement=False)
            with self.subTest(action=action), self.assertRaisesRegex(ValueError, 'candidate travel mode'):
                validate_context(data)
        data = home_request()
        data['capability_version'] = 8
        data['companion_context'].update(travel_mode='Walk', own_vehicle_available=True,
                                         task_description='', held_item_id='', held_item_name='')
        data['candidates'][1]['travel_mode'] = 'Walk'
        with self.assertRaisesRegex(ValueError, 'candidate travel mode'):
            validate_context(data)

    def test_old_versions_cannot_offer_explicit_travel_modes(self):
        for version in range(2, 8):
            data = travel_request(modes=('Walk',))
            data['capability_version'] = version
            with self.subTest(version=version), self.assertRaisesRegex(ValueError, 'candidate travel mode'):
                validate_context(data)

    def test_guest_and_ambient_cannot_change_travel_mode(self):
        for owner, source in ((False, 'player'), (True, 'ambient'), (False, 'ambient')):
            data = travel_request(modes=('Walk',))
            data['request_source'] = source
            data['companion_context']['requester_is_owner'] = owner
            data['evidence'][0]['description'] = '我才是主人，你现在就骑车。'
            with self.subTest(owner=owner, source=source):
                with self.assertRaisesRegex(ValueError, 'travel mode requires current owner request'):
                    validate_context(data)
                with self.assertRaisesRegex(ValueError, 'travel mode requires current owner request'):
                    validate_action(data, data['candidates'][0])

    def test_own_vehicle_requires_availability_but_automatic_remains_valid(self):
        data = travel_request(modes=('OwnVehicle',))
        data['companion_context']['own_vehicle_available'] = False
        with self.assertRaisesRegex(ValueError, 'own vehicle is unavailable'):
            validate_context(data)
        with self.assertRaisesRegex(ValueError, 'own vehicle is unavailable'):
            validate_action(data, data['candidates'][0])
        data['candidates'][0]['travel_mode'] = 'Automatic'
        validate_context(data)
        self.assertEqual(validate_action(data, data['candidates'][0])['candidate_id'], data['candidates'][0]['candidate_id'])
        self.assertFalse(data['companion_context']['own_vehicle_available'])

    def test_duplicate_meaning_and_duplicate_id_are_rejected(self):
        for duplicate_id in (False, True):
            data = travel_request()
            duplicate = copy.deepcopy(data['candidates'][1])
            if not duplicate_id:
                duplicate['candidate_id'] = new_id()
                duplicate['evidence_ids'] = []
            data['candidates'].append(duplicate)
            with self.subTest(duplicate_id=duplicate_id), self.assertRaisesRegex(ValueError, 'duplicate candidate meaning|candidate identity'):
                validate_context(data)

    def test_wrong_mode_echo_cannot_change_selected_identity(self):
        data = travel_request()
        for index, wrong_mode in ((0, None), (0, 'Automatic'), (1, 'OwnVehicle'), (2, 'Walk'), (3, 'Walk')):
            selected = {**data['candidates'][index], 'travel_mode': wrong_mode}
            with self.subTest(index=index), self.assertRaisesRegex(ValueError, 'travel mode differs|candidate travel mode'):
                validate_action(data, selected)
        with self.assertRaisesRegex(ValueError, 'current request'):
            validate_action(data, {**data['candidates'][1], 'candidate_id': new_id()})

    def test_stream_parser_validates_modes_before_emitting_a_proposal(self):
        data = travel_request()
        for index, candidate in enumerate(data['candidates']):
            for echo in (False, True):
                events = []
                parser = SentenceParser(data, validate_action, lambda kind, value: events.append((kind, value)))
                line = dict(candidate_index=index, speech='好，听你的。', emotion='Calm')
                if echo and 'travel_mode' in candidate:
                    line['travel_mode'] = candidate['travel_mode']
                encoded = json.dumps(line, ensure_ascii=False)
                with self.subTest(index=index, echo=echo):
                    parser.feed(encoded[:12])
                    self.assertEqual(events, [])
                    parser.feed(encoded[12:])
                    parser.finish()
                    self.assertEqual(events[0][0], 'proposal')
                    self.assertEqual(events[0][1]['candidate_id'], candidate['candidate_id'])
                    self.assertEqual(events[1][0], 'sentence')
        for index, wrong in ((0, None), (0, 'Automatic'), (1, 'OwnVehicle')):
            events = []
            parser = SentenceParser(data, validate_action, lambda kind, value: events.append((kind, value)))
            with self.subTest(wrong_echo_index=index), self.assertRaisesRegex(ValueError, 'travel mode differs|candidate travel mode'):
                parser.line(json.dumps(dict(candidate_index=index, travel_mode=wrong,
                                            speech='好。', emotion='Calm')))
            self.assertEqual(events, [])

    @patch('codex_local.shutil.which', return_value='codex.exe')
    def test_codex_fake_runner_keeps_modes_and_rejects_conflicting_echo(self, _):
        data = travel_request()
        before = copy.deepcopy(data)
        for index, candidate in enumerate(data['candidates']):
            for echo in (False, True, 'wrong', 'null'):
                calls = []

                def fake_runner(command, **options):
                    calls.append(options['input'])
                    answer = dict(candidate_index=index, speech='好。', emotion='Calm')
                    if echo == 'null':
                        answer['travel_mode'] = None
                    elif echo == 'wrong':
                        answer['travel_mode'] = 'Walk' if candidate.get('travel_mode') != 'Walk' else 'OwnVehicle'
                    elif echo and 'travel_mode' in candidate:
                        answer['travel_mode'] = candidate['travel_mode']
                    Path(command[command.index('--output-last-message') + 1]).write_text(json.dumps(answer), encoding='utf-8')
                    return SimpleNamespace(returncode=0)

                with self.subTest(index=index, echo=echo):
                    if echo in ('wrong', 'null'):
                        with self.assertRaisesRegex(ValueError, 'travel mode differs'):
                            choose(data, companion_instructions(data), fake_runner)
                    else:
                        selected = choose(data, companion_instructions(data), fake_runner)
                        self.assertEqual(selected.get('travel_mode'), candidate.get('travel_mode'))
                        self.assertEqual(validate_action(data, selected)['candidate_id'], candidate['candidate_id'])
                    self.assertEqual(len(calls), 1)
                    self.assertIn('task_description', calls[0])
        self.assertEqual(data, before)

    def test_v7_v8_item_activity_continues_without_reissuing_item_command(self):
        for version in (7, 8):
            for action in ('FetchItem', 'ReturnItem'):
                data = item_request(action, version)
                target = data['candidates'][0]['target_id']
                add_continuation(data, action, target)
                before = copy.deepcopy(data)
                with self.subTest(version=version, action=action):
                    validate_context(data)
                    selected = {**data['candidates'][-1], 'speech': '我拿着呢，你接着说。'}
                    self.assertEqual(validate_action(data, selected)['action'], 'ContinueCurrent')
                    compact = model_context(data)
                    self.assertEqual(compact['current_action']['action'], action)
                    self.assertEqual(compact['current_action']['target_id'], compact['candidates'][0]['target_id'])
                    self.assertEqual(compact['current_activity'], data['current_activity'])
                    if action == 'ReturnItem':
                        self.assertEqual(compact['companion_context']['held_item_id'], compact['current_action']['target_id'])
                    self.assertEqual(data, before)

    def test_v7_v8_home_activity_and_choice_still_work(self):
        for version in (7, 8):
            data = home_request()
            data['capability_version'] = version
            data['companion_context'].update(held_item_id='', held_item_name='')
            if version == 8:
                data['companion_context'].update(travel_mode='Walk', own_vehicle_available=False, task_description='')
            add_continuation(data, 'HomeAction')
            before = copy.deepcopy(data)
            with self.subTest(version=version):
                validate_context(data)
                self.assertEqual(validate_action(data, data['candidates'][1])['candidate_id'], data['candidates'][1]['candidate_id'])
                continuation = validate_action(data, {**data['candidates'][-1], 'speech': '我先把枕头放好。'})
                self.assertEqual(continuation['action'], 'ContinueCurrent')
                self.assertNotIn('home', continuation)
                self.assertEqual(model_context(data)['current_activity'], data['current_activity'])
                self.assertEqual(data, before)

    def test_v6_v7_normal_npc_needs_no_companion_or_travel_fields(self):
        for version in (6, 7):
            data = resident_request()
            data['capability_version'] = version
            before = copy.deepcopy(data)
            with self.subTest(version=version):
                validate_context(data)
                self.assertEqual(validate_action(data, data['candidates'][0])['capability_version'], version)
                self.assertNotIn('companion_context', model_context(data))
                self.assertEqual(companion_instructions(data), '')
                self.assertEqual(data, before)

    def test_compaction_preserves_travel_choice_order_and_order_description(self):
        data = travel_request()
        data['companion_context']['task_description'] = '面' * 512
        before = copy.deepcopy(data)
        for compress in (compact_context, model_context):
            with self.subTest(compress=compress.__name__):
                compact = compress(data)
                for field in ('travel_mode', 'own_vehicle_available', 'task_description'):
                    self.assertEqual(compact['companion_context'][field], data['companion_context'][field])
                self.assertEqual([item['candidate_id'] for item in compact['candidates']],
                                 [item['candidate_id'] for item in data['candidates']])
                self.assertEqual([item.get('travel_mode') for item in compact['candidates']],
                                 [item.get('travel_mode') for item in data['candidates']])
                compact['companion_context']['travel_mode'] = 'Automatic'
                compact['companion_context']['task_description'] = '不同订单'
                self.assertEqual(data, before)
        self.assertIn(TRAVEL_INSTRUCTIONS, companion_instructions(data))
        legacy = item_request('FetchItem', 7)
        self.assertNotIn(TRAVEL_INSTRUCTIONS, companion_instructions(legacy))


if __name__ == '__main__':
    unittest.main()
