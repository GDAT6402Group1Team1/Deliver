"""出行协议离线检查，只读写内存样例和假模型回复，不连接服务或启动模型"""
import copy
import json
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from codex_local import choose
from companion_context import (ACTIVE_ASSIST_INSTRUCTIONS, CARRIED_ITEM_LOCATION_INSTRUCTIONS, DROP_PACKAGE_INSTRUCTIONS, ERRAND_TASK_INSTRUCTIONS,
                               ERRAND_INSTRUCTIONS, FOLLOW_ERRAND_INTENT_INSTRUCTIONS,
                               JOIN_ASSIST_INSTRUCTIONS,
                               OWNER_PACKAGE_LOCATION_INSTRUCTIONS, PACE_INSTRUCTIONS, QUIET_PREFERENCE_INSTRUCTIONS,
                               RETURN_PACKAGE_INSTRUCTIONS,
                               SEPARATE_TRAVEL_INSTRUCTIONS, STOP_ASSIST_INSTRUCTIONS, STORE_PACKAGE_ACTION, STORE_PACKAGE_INSTRUCTIONS,
                               TASK_DELIVERED_INSTRUCTIONS, TASK_DOOR_STATE_INSTRUCTIONS,
                               TASK_DOOR_UNKNOWN_INSTRUCTIONS, TRAVEL_INSTRUCTIONS, VEHICLE_STATE_INSTRUCTIONS,
                               companion_instructions, with_object_action_options)
from server import decide, validate_action, validate_context
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


def speech_preference_request():
    data = travel_request()
    add_continuation(data, 'Follow')
    data['candidates'] = [data['candidates'][-1]]
    data['allowed_actions'] = ['ContinueCurrent']
    for preference in ('Quiet', 'Normal'):
        data['candidates'].append({**data['candidates'][0], 'candidate_id': new_id(),
                                   'speech_preference': preference})
    return data


def package_drop_request():
    data = travel_request('DropPackage', modes=(None, 'Walk'))
    data['capability_version'] = 9
    data['companion_context'].update(task_id='Task_001', task_phase='InProgress', has_package=True,
                                     carried_item_location='Hand')
    return data


class CompanionTravelTests(unittest.TestCase):
    def test_drop_requires_v9_direct_owner_current_order_and_held_package(self):
        data = package_drop_request()
        data['candidates'] = data['candidates'][:1]
        validate_context(data)
        for field, value, error in (
                ('capability_version', 8, 'drop package requires capability version 9'),
                ('capability_version', 11, 'unsupported version'),
                ('request_source', 'ambient', 'drop package requires current owner request'),
                ('requester_is_owner', False, 'companion command requires owner'),
                ('has_package', False, 'drop package requires current owner request'),
                ('task_id', ' ', 'drop package requires current owner request')):
            invalid = copy.deepcopy(data)
            del invalid['companion_context']['carried_item_location']
            target = invalid if field in ('capability_version', 'request_source') else invalid['companion_context']
            target[field] = value
            with self.subTest(field=field, value=value), self.assertRaisesRegex(ValueError, error):
                validate_context(invalid)
        with self.assertRaisesRegex(ValueError, 'current request'):
            validate_action(data, {**data['candidates'][0], 'candidate_id': new_id()})

    @patch('codex_local.shutil.which', return_value='codex.exe')
    def test_hand_ground_and_backpack_confirmation_keep_distinct_candidates_in_both_adapters(self, _):
        for action, question, index in (('ReturnPackage', '是要我把包裹递给你吗？', 2),
                                        ('DropPackage', '是要我把包裹放你脚边吗？', 1),
                                        (STORE_PACKAGE_ACTION, '是要我把包裹收进你的背包吗？', 4)):
            data = package_drop_request()
            data['capability_version'] = 10
            handoff_options = ('ReturnPackage', 'DeliverPackage', STORE_PACKAGE_ACTION)
            data['allowed_actions'].extend(handoff_options)
            first = data['evidence'][0]
            first['description'] = '这件包裹怎么给我？'
            question_event = dict(event_id=new_id(), subject_id=data['npc_id'], kind='SelfSpeech',
                                  source='ActionResult', reply_to_event_id=first['event_id'], description=question)
            latest = {**first, 'event_id': new_id(), 'description': '对。'}
            data['evidence'].extend((question_event, latest))
            data['pending_player_message'] = True
            for extra in handoff_options:
                data['candidates'].append({**data['candidates'][1], 'candidate_id': new_id(),
                                           'action': extra, 'evidence_ids': [latest['event_id']]})
            data['companion_context'].update(travel_mode='OwnVehicle', errand_action='DeliverPackage',
                                             errand_status='Running', errand_phase='GoToDelivery')
            add_continuation(data, 'DeliverPackage')
            validate_context(data)
            instructions = companion_instructions(data)
            reply = json.dumps(dict(candidate_index=index, speech='好，我这就拿过来。', emotion='Calm'),
                               ensure_ascii=False)

            def fake_runner(command, **options):
                self.assertIn(question, options['input'])
                Path(command[command.index('--output-last-message') + 1]).write_text(reply, encoding='utf-8')
                return SimpleNamespace(returncode=0)

            with self.subTest(action=action):
                self.assertIn(RETURN_PACKAGE_INSTRUCTIONS, instructions)
                self.assertIn(DROP_PACKAGE_INSTRUCTIONS, instructions)
                self.assertIn(STORE_PACKAGE_INSTRUCTIONS, instructions)
                self.assertIn('正式包裹进玩家手持槽', instructions)
                self.assertIn('新的明确交接覆盖旧的代送目标', instructions)
                for shape in (compact_context(data), model_context(data)):
                    self.assertEqual(shape['pending_player_message']['description'], '对。')
                    self.assertEqual(shape['current_action']['action'], 'DeliverPackage')
                    self.assertEqual(shape['companion_context']['travel_mode'], 'OwnVehicle')
                    retained_question = next(item for item in shape['evidence'] if item['description'] == question)
                    self.assertEqual(retained_question['reply_to_event_id'], shape['evidence'][0]['event_id'])
                    self.assertEqual(shape['candidates'][index]['action'], action)
                adapter_input = with_object_action_options(data)
                for shape in (adapter_input, model_context(data)):
                    destinations = {row['action']: row['destination'] for row in shape['object_actions']['options']}
                    self.assertEqual(destinations[STORE_PACKAGE_ACTION], '主人背包')
                    self.assertEqual(destinations['ReturnPackage'], '主人手中')
                    self.assertEqual(destinations['DropPackage'], '主人附近安全地面')
                selected = validate_action(data, choose(adapter_input, instructions, fake_runner))
                events = []
                parser = SentenceParser(data, validate_action, lambda kind, value: events.append((kind, value)))
                parser.feed(reply)
                parser.finish()
                self.assertEqual(events[0], ('proposal', selected))
                self.assertEqual(selected['action'], action)
                self.assertEqual(selected['candidate_id'], data['candidates'][index]['candidate_id'])
                self.assertEqual(data['candidates'][index]['travel_mode'], 'Walk')

    def test_area_search_cites_current_owner_speech_without_inventing_item(self):
        data = travel_request('SearchNearOwner', modes=(None,))
        data['capability_version'] = 10
        data['pending_player_message'] = True
        data['evidence'][0].update(kind='PlayerSpeech', source='Heard', description='锤子可能在我旁边，找找。')
        validate_context(data)
        candidate = data['candidates'][0]
        self.assertEqual(model_context(data)['candidates'][0]['target_id'], '')
        self.assertIn('按主人本轮位置步行查看附近', companion_instructions(data))
        selected = validate_action(data, {**candidate, 'speech': '我过来看看。'})
        self.assertEqual((selected['action'], selected['target_id']), ('SearchNearOwner', ''))
        for invalid in ('visitor', 'ambient', 'stale', 'old_version', 'wrong_evidence', 'travel'):
            bad = copy.deepcopy(data)
            if invalid == 'visitor':
                bad['companion_context']['requester_is_owner'] = False
            elif invalid == 'ambient':
                bad['request_source'] = 'ambient'
            elif invalid == 'stale':
                bad['pending_player_message'] = False
            elif invalid == 'old_version':
                bad['capability_version'] = 9
            elif invalid == 'wrong_evidence':
                bad['evidence'][0]['kind'] = 'Dialogue'
            else:
                bad['candidates'][0]['travel_mode'] = 'OwnVehicle'
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                validate_context(bad)

    def test_v10_backpack_handoff_requires_current_owner_and_same_task_package(self):
        data = travel_request(STORE_PACKAGE_ACTION, modes=(None, 'Walk'))
        data['capability_version'] = 10
        data['companion_context'].update(task_id='Task_001', task_phase='InProgress', has_package=True)
        validate_context(data)
        data['candidates'] = data['candidates'][:1]
        for field, value, error in (
                ('capability_version', 9, 'require capability version 10'),
                ('request_source', 'ambient', 'store package requires current owner request'),
                ('requester_is_owner', False, 'companion command requires owner'),
                ('has_package', False, 'store package requires current owner request'),
                ('task_id', ' ', 'store package requires current owner request')):
            invalid = copy.deepcopy(data)
            target = invalid if field in ('capability_version', 'request_source') else invalid['companion_context']
            target[field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, error):
                validate_context(invalid)

    def test_backpack_handoff_keeps_waiting_and_reports_only_confirmed_slot(self):
        data = travel_request(STORE_PACKAGE_ACTION, modes=(None,))
        data['capability_version'] = 10
        snapshot = data['companion_context']
        snapshot.update(task_id='Task_001', task_phase='InProgress', has_package=True,
                        errand_action=STORE_PACKAGE_ACTION, errand_phase='ReturnToOwner', errand_status='Running',
                        suspension_reason='OwnerBackpackUnavailable')
        add_continuation(data, STORE_PACKAGE_ACTION)
        validate_context(data)
        self.assertIn('OwnerBackpackUnavailable', companion_instructions(data))
        self.assertEqual(model_context(data)['current_activity']['errand_action'], STORE_PACKAGE_ACTION)
        continued = validate_action(data, {**data['candidates'][-1], 'speech': '等你的背包有一格空位，我再放进去。'})
        self.assertEqual(continued['action'], 'ContinueCurrent')

        data['allowed_actions'] = ['Wait']
        data['candidates'] = [dict(candidate_id=new_id(), action='Wait', target_id='', duration=1, evidence_ids=[])]
        del data['current_action'], data['current_activity']
        snapshot.update(has_package=False, owner_has_package=True, owner_package_location='Backpack',
                        errand_status='Succeeded', errand_phase='Idle', suspension_reason='',
                        errand_reason='PackageStoredInOwnerBackpack')
        validate_context(data)
        for payload in (model_context(data), compact_context(data)):
            self.assertEqual(payload['companion_context']['owner_package_location'], 'Backpack')
            self.assertEqual(payload['companion_context']['errand_reason'], 'PackageStoredInOwnerBackpack')
        self.assertIn('此结果只确认主人收取', companion_instructions(data))
        for field, value in (('owner_has_package', False), ('has_package', True), ('requester_is_owner', False)):
            invalid = copy.deepcopy(data)
            invalid['companion_context'][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'owner package location'):
                validate_context(invalid)

    def test_v9_keep_running_drop_and_only_report_confirmed_placement(self):
        data = package_drop_request()
        add_continuation(data, 'DropPackage')
        validate_context(data)
        self.assertEqual(model_context(data)['current_action']['action'], 'DropPackage')
        self.assertEqual(model_context(data)['current_activity']['errand_action'], 'DropPackage')
        selected = validate_action(data, {**data['candidates'][-1], 'speech': '我正在找个平整的地方。'})
        self.assertEqual(selected['action'], 'ContinueCurrent')
        legacy = copy.deepcopy(data)
        legacy.update(capability_version=8, allowed_actions=['ContinueCurrent'], candidates=[data['candidates'][-1]])
        with self.assertRaisesRegex(ValueError, 'invalid current action identity'):
            validate_context(legacy)

        data = travel_request('ReturnPackage', modes=(None,))
        companion = data['companion_context']
        companion.update(task_id='Task_001', task_phase='InProgress', has_package=True)
        validate_context(data)
        self.assertNotIn(RETURN_PACKAGE_INSTRUCTIONS, companion_instructions(data))
        self.assertNotIn(DROP_PACKAGE_INSTRUCTIONS, companion_instructions(data))
        data['capability_version'] = 9
        validate_context(data)
        self.assertIn(RETURN_PACKAGE_INSTRUCTIONS, companion_instructions(data))
        self.assertNotIn(DROP_PACKAGE_INSTRUCTIONS, companion_instructions(data))
        companion.update(has_package=False, errand_action='DropPackage', errand_status='Succeeded',
                         errand_reason='PackagePlacedOnGround')
        data['allowed_actions'] = ['Wait']
        data['candidates'][0].update(action='Wait', duration=1)
        validate_context(data)
        self.assertEqual(model_context(data)['companion_context']['errand_reason'], 'PackagePlacedOnGround')
        self.assertIn(DROP_PACKAGE_INSTRUCTIONS, companion_instructions(data))
        self.assertNotIn('DropPackage', data['allowed_actions'])

    def test_v9_preserves_quiet_ordinary_item_and_home_choices(self):
        data = speech_preference_request()
        data['capability_version'] = 9
        data['companion_context']['quiet_seconds_remaining'] = 35
        validate_context(data)
        self.assertEqual(model_context(data)['candidates'][1]['speech_preference'], 'Quiet')
        self.assertEqual(model_context(data)['companion_context']['quiet_seconds_remaining'], 35)
        validate_context(item_request('ReturnItem', 9))

        data = home_request()
        data['capability_version'] = 9
        data['candidates'][0]['candidate_id'] = new_id()
        data['companion_context'].update(travel_mode='Walk', own_vehicle_available=True, task_description='',
                                         held_item_id='', held_item_name='',
                                         remembered_home_objects=[{'name': '枕头', 'description': '布枕头'}])
        validate_context(data)
        selected = validate_action(data, data['candidates'][1])
        self.assertEqual(selected['candidate_id'], data['candidates'][1]['candidate_id'])
        self.assertEqual(model_context(data)['candidates'][1]['home']['destination_slot_id'], 'pillow_left')
        self.assertEqual(model_context(data)['companion_context']['remembered_home_objects'][0]['name'], '枕头')

    def test_v10_object_options_keep_travel_variants_and_ground_package_identity(self):
        data = travel_request('TakePackage', modes=(None, 'Walk'))
        data['capability_version'] = 10
        data['self_name'] = '牛二狗'
        data['companion_context'].update(task_id='Task_001', task_phase='InProgress', owner_package_location='Ground')
        for action in ('FetchPackage', 'DeliverPackage'):
            data['allowed_actions'].append(action)
            data['candidates'].append({**data['candidates'][1], 'candidate_id': new_id(), 'action': action})
        validate_context(data)
        original = copy.deepcopy(data)
        payload = model_context(data)
        options = payload['object_actions']
        self.assertEqual(options['actor'], '牛二狗')
        self.assertEqual([(row['action'], row['destination'], row['choices']) for row in options['options']],
                         [('TakePackage', '自己手中', [0, 1]), ('FetchPackage', '主人手中', [2]),
                          ('DeliverPackage', '订单指定收件人', [3])])
        self.assertTrue(all(row['source'] == '游戏确认的地上原件' for row in options['options']))
        self.assertEqual(data, original)
        self.assertEqual([c['candidate_id'] for c in payload['candidates']],
                         [c['candidate_id'] for c in data['candidates']])
        for field, value in (('capability_version', 9), ('owner_has_package', True),
                             ('requester_is_owner', False), ('task_phase', 'AwaitingPickup')):
            invalid = copy.deepcopy(data)
            (invalid if field == 'capability_version' else invalid['companion_context'])[field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'owner package location'):
                validate_context(invalid)

    def test_v10_take_item_requires_the_same_owner_inventory_evidence(self):
        data = item_request('FetchItem', 10)
        data['allowed_actions'] = ['TakeItem']
        fact = data['evidence'][-1]
        fact.update(kind='CompanionOwnerItemAvailable', source='ActionResult',
                    description='主人背包里有这把小锤子。')
        candidate = data['candidates'][0]
        candidate.update(action='TakeItem', item_source='OwnerBackpack')
        validate_context(data)
        self.assertEqual(model_context(data)['object_actions']['options'][0]['source'], '主人背包')
        with patch('codex_local.choose', return_value=candidate) as model:
            result = decide(data, 'codex-local')
            self.assertEqual(model.call_args.args[0]['object_actions']['options'][0]['choices'], [0])
            self.assertEqual(result['candidate_id'], candidate['candidate_id'])
            self.assertNotIn('item_source', result)
        for bad_source in ('World', None):
            with self.subTest(bad_source=bad_source), self.assertRaisesRegex(ValueError, 'item source'):
                validate_action(data, {**candidate, 'item_source': bad_source})
        with self.assertRaisesRegex(ValueError, 'item source differs'):
            validate_action(data, {**candidate, 'item_source': 'OwnerHand'})
        fact['source'] = 'ToldBy'
        with self.assertRaisesRegex(ValueError, 'current owner item'):
            validate_context(data)

    def test_v10_caterpillar_uses_exact_item_and_exact_insert_order(self):
        data = item_request('FetchItem', 10)
        data['pending_player_message'] = True
        data['companion_context'].update(task_id='Task_001', task_phase='InProgress')
        data['subjects'][0]['name'] = '毛毛虫'
        data['allowed_actions'] = ['CollectCaterpillar', 'InsertCaterpillar']
        collect = data['candidates'][0]
        collect.update(action='CollectCaterpillar', item_source='World')
        insert = {**collect, 'candidate_id': new_id(), 'action': 'InsertCaterpillar', 'package_task_id': 'Task_001'}
        data['candidates'].append(insert)
        validate_context(data)
        options = model_context(data)['object_actions']['options']
        self.assertEqual([row['object_name'] for row in options], ['小绿', '小绿'])
        self.assertEqual(options[1]['destination'], '原包裹 / Task_001')
        events = []
        parser = SentenceParser(data, validate_action, lambda kind, value: events.append((kind, value)))
        forged = dict(candidate_index=1, package_task_id='Task_002', speech='好。', emotion='Calm')
        with self.assertRaisesRegex(ValueError, 'caterpillar candidate package task'):
            parser.line(json.dumps(forged))
        self.assertEqual(events, [])
        for field, value, error in (('request_source', 'ambient', 'pending owner'), ('pending_player_message', False, 'pending owner'),
                                    ('capability_version', 9, 'version 10')):
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, error):
                validate_context({**data, field: value})

        held = data['companion_context']
        held.update(held_item_id=insert['target_id'], held_item_name='毛毛虫')
        data['allowed_actions'] = ['InsertCaterpillar']
        data['candidates'] = [insert]
        insert['item_source'] = 'CompanionHand'
        data['evidence'][-1].update(kind='CompanionHoldingItem', source='ActionResult')
        validate_context(data)
        self.assertEqual(model_context(data)['object_actions']['options'][0]['source'], '自己手中')

    def test_v10_held_package_prank_requires_ground_worm_and_real_hand_package(self):
        data = item_request('FetchItem', 10)
        data['pending_player_message'] = True
        data['companion_context'].update(task_id='Task_001', task_phase='InProgress',
                                         has_package=True, carried_item_location='Hand',
                                         errand_action='DeliverPackage', errand_status='Running',
                                         errand_phase='GoToDelivery', errand_reason='WaitingAtGrandpaDoor')
        data['evidence'][0]['description'] = '然后把旁边的小绿放进你手里的外卖。'
        data['subjects'][0]['name'] = '小绿（毛毛虫）'
        data['evidence'][-1]['description'] = '我看见了附近的小绿。'
        data['allowed_actions'] = ['InsertCaterpillar']
        candidate = data['candidates'][0]
        candidate.update(action='InsertCaterpillar', item_source='World', package_task_id='Task_001')
        add_continuation(data, 'DeliverPackage')
        data['allowed_actions'].append('DeliverPackage')
        data['candidates'].append(dict(action='DeliverPackage', candidate_id=new_id(), target_id='', duration=0,
                                       evidence_ids=[data['evidence'][0]['event_id']]))
        validate_context(data)
        compact = model_context(data)
        option = compact['object_actions']['options'][0]
        self.assertEqual((option['source'], option['destination']), ('附近已确认的小绿', '自己手里的原包裹 / Task_001'))
        instructions = companion_instructions(data)
        self.assertIn('主人要求先投放就先办这一步', instructions)
        self.assertIn('确认刚才商量的投放', instructions)
        self.assertIn('主人重复刚才的要求或催你去做时，仍按本轮安排理解', instructions)
        events = []
        parser = SentenceParser(data, validate_action, lambda kind, value: events.append((kind, value)))
        parser.line(json.dumps(dict(candidate_index=0, speech='我去拿那只小绿。', emotion='Calm')))
        self.assertEqual(events[0][1]['candidate_id'], candidate['candidate_id'])

        for package_location in ('OwnBikeRack', None):
            blocked = copy.deepcopy(data)
            if package_location:
                blocked['companion_context'].update(carried_item_location=package_location, vehicle_state='Mounted')
            else:
                del blocked['companion_context']['carried_item_location']
            with self.subTest(package_location=package_location), self.assertRaisesRegex(ValueError, 'current hand package'):
                validate_context(blocked)
        blocked = copy.deepcopy(data)
        blocked['evidence'][-1].update(kind='CompanionOwnerItemAvailable', source='ActionResult')
        with self.assertRaisesRegex(ValueError, 'grounded caterpillar'):
            validate_context(blocked)
        unavailable = copy.deepcopy(data)
        unavailable['allowed_actions'].remove('InsertCaterpillar')
        del unavailable['candidates'][0]
        validate_context(unavailable)
        self.assertIn('没有匹配候选就说明当前不能投放，可用 ContinueCurrent 保留已有配送', companion_instructions(unavailable))
        self.assertFalse(any(row['action']=='InsertCaterpillar' for row in model_context(unavailable)['object_actions']['options']))

    def test_v10_stationary_owner_prank_seals_inventory_item_and_current_package(self):
        for source, package in (('OwnerBackpack', 'Hand'), ('OwnerBackpack', 'Ground'), ('OwnerHand', 'Ground')):
            with self.subTest(source=source, package=package):
                data = item_request('FetchItem', 10)
                data['pending_player_message'] = True
                data['evidence'][0]['description'] = '好，把我这里的小绿放进快递。'
                data['companion_context'].update(task_id='Task_001', task_phase='InProgress',
                                                 owner_has_package=package=='Hand', owner_package_location=package)
                data['allowed_actions'] = ['InsertCaterpillar']
                data['evidence'][-1].update(kind='CompanionOwnerItemAvailable', source='ActionResult')
                candidate = data['candidates'][0]
                candidate.update(action='InsertCaterpillar', item_source=source, package_task_id='Task_001')
                validate_context(data)
                model = model_context(data)
                self.assertEqual(model['object_actions']['options'][0]['source'],
                                 {'OwnerHand': '主人手中', 'OwnerBackpack': '主人背包'}[source])
                self.assertEqual(model['object_actions']['options'][0]['destination'],
                                 {'Hand': '主人手里的原包裹', 'Ground': '主人登记在地上的原包裹'}[package] + ' / Task_001')
                self.assertIn('等游戏确认 CaterpillarInserted', companion_instructions(data))
                events = []
                parser = SentenceParser(data, validate_action, lambda kind, value: events.append((kind, value)))
                parser.line(json.dumps(dict(candidate_index=0, speech='我来放。', emotion='Calm')))
                self.assertEqual(events[0][1]['candidate_id'], candidate['candidate_id'])

                blocked = copy.deepcopy(data)
                blocked['companion_context'].update(owner_has_package=True, owner_package_location='RegisteredBikeTrunk')
                with self.assertRaisesRegex(ValueError, 'hand or ground package'):
                    validate_context(blocked)
                blocked = copy.deepcopy(data)
                blocked['evidence'][-1]['kind'] = 'WorldItemSearchLead'
                with self.assertRaisesRegex(ValueError, 'same current item'):
                    validate_context(blocked)
                blocked = copy.deepcopy(data)
                blocked['companion_context']['requester_is_owner'] = False
                del blocked['companion_context']['owner_package_location']
                with self.assertRaisesRegex(ValueError, 'requires owner'):
                    validate_context(blocked)
                if source == 'OwnerHand':
                    blocked = copy.deepcopy(data)
                    blocked['companion_context'].update(owner_has_package=True, owner_package_location='Hand')
                    with self.assertRaisesRegex(ValueError, 'hand or ground package'):
                        validate_context(blocked)

    def test_v10_join_assist_needs_the_current_target_and_real_hit(self):
        data = travel_request('JoinAssist', modes=(None,))
        data.update(capability_version=10, request_source='ambient', evidence=[])
        data['companion_context'].update(requester_is_owner=False, assist_revision=3,
                                         assist_active=False, assist_pending=True)
        target = new_id()
        data['subjects'] = [{'subject_id': target, 'kind': 'npc', 'name': '街坊'}]
        fact = dict(event_id=new_id(), subject_id=target, kind='OwnerConfirmedHit', source='ActionResult',
                    description='这位街坊已经打中主人。')
        data['evidence'].append(fact)
        data['candidates'][0].update(target_id=target, evidence_ids=[fact['event_id']])
        validate_context(data)
        self.assertIn(JOIN_ASSIST_INSTRUCTIONS, companion_instructions(data))
        self.assertNotIn(STOP_ASSIST_INSTRUCTIONS, companion_instructions(data))
        model = model_context(data)
        self.assertEqual(model['candidates'][0]['evidence_ids'], [model['evidence'][0]['event_id']])
        self.assertEqual(model['candidates'][0]['target_id'], model['evidence'][0]['subject_id'])
        events = []
        parser = SentenceParser(data, validate_action, lambda kind, value: events.append((kind, value)))
        parser.line(json.dumps(dict(candidate_index=0, speech='', emotion='Calm')))
        self.assertEqual(events[0][1]['candidate_id'], data['candidates'][0]['candidate_id'])
        self.assertEqual(events[0][1]['evidence_ids'], [fact['event_id']])

        for overrides in ({'kind': 'PlayerSpeech'}, {'source': 'Heard'}, {'kind': 'SelfCombat'},
                          {'subject_id': new_id()}):
            with self.subTest(fact=overrides), self.assertRaisesRegex(ValueError, 'actual hit evidence|unknown target'):
                validate_context({**data, 'evidence': [{**fact, **overrides}]})
        with self.assertRaisesRegex(ValueError, 'requires owner'):
            validate_context({**data, 'request_source': 'player', 'pending_player_message': True})
        data['companion_context']['assist_pending'] = False
        with self.assertRaisesRegex(ValueError, 'pending hit'):
            validate_context(data)

    def test_v10_stop_assist_is_separate_from_chat_and_cannot_cancel_errand(self):
        data = travel_request('StopAssist', modes=(None,))
        data['capability_version'] = 10
        data['companion_context'].update(assist_revision=4, assist_active=True, assist_pending=False,
                                         errand_action='DeliverPackage', errand_status='Running')
        data['candidates'][0]['evidence_ids'] = []
        add_continuation(data, 'DeliverPackage')
        validate_context(data)
        self.assertIn(ACTIVE_ASSIST_INSTRUCTIONS, companion_instructions(data))
        self.assertIn(STOP_ASSIST_INSTRUCTIONS, companion_instructions(data))
        self.assertEqual(model_context(data)['companion_context']['errand_status'], 'Running')
        for index, speech in ((0, '行，收手。'), (1, '我听着呢。')):
            events = []
            parser = SentenceParser(data, validate_action, lambda kind, value: events.append((kind, value)))
            parser.line(json.dumps(dict(candidate_index=index, speech=speech, emotion='Calm')))
            self.assertEqual(events[0][1]['action'], data['candidates'][index]['action'])
        with self.assertRaisesRegex(ValueError, 'current owner request'):
            validate_context({**data, 'pending_player_message': False})
        with self.assertRaisesRegex(ValueError, 'current owner request'):
            validate_context({**data, 'request_source': 'ambient'})
        invalid = copy.deepcopy(data)
        invalid['companion_context']['assist_active'] = False
        with self.assertRaisesRegex(ValueError, 'active assist'):
            validate_context(invalid)
        invalid['companion_context']['assist_revision'] = True
        with self.assertRaisesRegex(ValueError, 'assist snapshot'):
            validate_context(invalid)

        visitor = copy.deepcopy(data)
        visitor['companion_context']['requester_is_owner'] = False
        visitor['allowed_actions'] = ['ContinueCurrent']
        visitor['candidates'] = visitor['candidates'][1:]
        validate_context(visitor)
        self.assertIn(ACTIVE_ASSIST_INSTRUCTIONS, companion_instructions(visitor))
        self.assertNotIn(STOP_ASSIST_INSTRUCTIONS, companion_instructions(visitor))

    def test_quiet_context_is_optional_integer_state_and_preserved_for_model(self):
        data = speech_preference_request()
        # 旧版 v8 的普通 ContinueCurrent 候选仍然可以省略偏好和计时
        data['candidates'] = data['candidates'][:1]
        validate_context(data)
        self.assertNotIn(QUIET_PREFERENCE_INSTRUCTIONS, companion_instructions(data))
        for remaining in (0, 34, 180, -1, 181, True, 30.0, None):
            data['companion_context']['quiet_seconds_remaining'] = remaining
            with self.subTest(remaining=remaining):
                if type(remaining) is int and 0 <= remaining <= 180:
                    validate_context(data)
                    self.assertEqual(model_context(data)['companion_context']['quiet_seconds_remaining'], remaining)
                    self.assertIn(QUIET_PREFERENCE_INSTRUCTIONS, companion_instructions(data))
                else:
                    with self.assertRaisesRegex(ValueError, 'quiet seconds remaining'):
                        validate_context(data)

    def test_speech_preference_requires_pending_owner_continue_and_distinct_choices(self):
        data = speech_preference_request()
        validate_context(data)
        self.assertEqual([item.get('speech_preference') for item in model_context(data)['candidates']],
                         [None, 'Quiet', 'Normal'])
        for field, value in (('request_source', 'ambient'), ('pending_player_message', False),
                             ('requester_is_owner', False), ('capability_version', 7),
                             ('allowed_actions', ['Wait']), ('speech_preference', 'Muted')):
            invalid = copy.deepcopy(data)
            target = {'requester_is_owner': invalid['companion_context'],
                      'speech_preference': invalid['candidates'][1]}.get(field, invalid)
            target[field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'speech preference|unsupported action'):
                validate_context(invalid)
        invalid = travel_request()
        invalid['pending_player_message'] = True
        invalid['candidates'][0]['speech_preference'] = 'Quiet'
        with self.assertRaisesRegex(ValueError, 'speech preference'):
            validate_context(invalid)
        data['candidates'].append({**data['candidates'][1], 'candidate_id': new_id()})
        with self.assertRaisesRegex(ValueError, 'duplicate candidate meaning'):
            validate_context(data)

    @patch('codex_local.shutil.which', return_value='codex.exe')
    def test_speech_preference_is_selected_only_by_candidate_in_both_adapters(self, _):
        data = speech_preference_request()
        for index, echo in ((0, None), (1, None), (2, 'Normal'), (0, 'Quiet'), (1, 'Normal')):
            candidate = data['candidates'][index]
            reply = dict(candidate_index=index, speech='好，我还跟着你。', emotion='Calm')
            if echo is not None:
                reply['speech_preference'] = echo
            encoded = json.dumps(reply, ensure_ascii=False)
            events = []
            parser = SentenceParser(data, validate_action, lambda kind, value: events.append((kind, value)))

            def fake_runner(command, **_options):
                Path(command[command.index('--output-last-message') + 1]).write_text(encoded, encoding='utf-8')
                return SimpleNamespace(returncode=0)

            with self.subTest(index=index, echo=echo):
                if echo is not None and echo != candidate.get('speech_preference'):
                    with self.assertRaisesRegex(ValueError, 'speech preference differs'):
                        parser.feed(encoded)
                    with self.assertRaisesRegex(ValueError, 'speech preference differs'):
                        choose(data, companion_instructions(data), fake_runner)
                    self.assertEqual(events, [])
                else:
                    parser.feed(encoded)
                    parser.finish()
                    local = validate_action(data, choose(data, companion_instructions(data), fake_runner))
                    self.assertEqual(events[0], ('proposal', local))
                    self.assertEqual(local['candidate_id'], candidate['candidate_id'])
                    self.assertNotIn('speech_preference', local)

    def test_fetch_search_lead_requires_matching_server_evidence(self):
        data = item_request('FetchItem', 8)
        fact = data['evidence'][-1]
        fact.update(kind='WorldItemSearchLead', source='ActionResult', description='附近有可查看的小锤子线索。')
        candidate = data['candidates'][0]
        validate_context(data)
        self.assertEqual(validate_action(data, candidate)['candidate_id'], candidate['candidate_id'])
        for unsuitable in ('Seen', 'ToldBy'):
            fact['source'] = unsuitable
            with self.subTest(source=unsuitable), self.assertRaisesRegex(ValueError, 'nearby search lead'):
                validate_action(data, candidate)

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

    def test_follow_and_errand_modes_remain_separate_in_model_snapshot(self):
        data = travel_request('DeliverPackage', modes=('Walk', 'OwnVehicle', 'Automatic'))
        add_continuation(data, 'Follow')
        data['current_activity'].update(mode='Follow', errand_action='', errand_phase='')
        snapshot = data['companion_context']
        snapshot['mode'] = 'Follow'
        self.assertIn(TRAVEL_INSTRUCTIONS, companion_instructions(data))
        self.assertNotIn(SEPARATE_TRAVEL_INSTRUCTIONS, companion_instructions(data))
        for follow, errand in (('Unspecified', 'Walk'), ('Walk', 'Automatic'),
                               ('OwnVehicle', 'Unspecified'), ('Automatic', 'OwnVehicle')):
            with self.subTest(follow=follow, errand=errand):
                snapshot.update(travel_mode=follow, errand_travel_mode=errand)
                validate_context(data)
                for compress in (compact_context, model_context):
                    payload = compress(data)
                    self.assertEqual(payload['companion_context']['travel_mode'], follow)
                    self.assertEqual(payload['companion_context']['errand_travel_mode'], errand)
                instructions = companion_instructions(data)
                self.assertIn(SEPARATE_TRAVEL_INSTRUCTIONS, instructions)
                self.assertNotIn(TRAVEL_INSTRUCTIONS, instructions)
                self.assertIn('仅因主人上车仍沿原安排步行', instructions)
                self.assertIn('主人明确要求骑你自己的车跟随时选 Follow + OwnVehicle', instructions)
                self.assertIn('沿已授权的方式恢复骑车', instructions)
                for candidate in data['candidates'][:3]:
                    result = validate_action(data, candidate)
                    self.assertEqual(result['candidate_id'], candidate['candidate_id'])
                    self.assertNotIn('errand_travel_mode', result)
                continuation = {**data['candidates'][-1], 'speech': '你想让我骑车送，还是走过去？'}
                self.assertEqual(validate_action(data, continuation)['action'], 'ContinueCurrent')

    def test_follow_departure_and_vehicle_changes_do_not_authorize_a_new_delivery(self):
        cases = (
            ('Follow', 'OnFoot', '走。', 'ContinueCurrent', None, '走，我跟着你。'),
            ('Follow', 'Riding', '咱俩骑车，你上你自己的。', 'Follow', 'OwnVehicle', '我上自己的车跟你。'),
            ('DeliverPackage', 'Riding', '我让你跟着我走，骑你自己的车。', 'Follow', 'OwnVehicle', '明白，我骑车跟你。'),
            ('Follow', 'OnFoot', '你把我车里的这份外卖骑车送给爷爷。', 'DeliverPackage', 'OwnVehicle', '我自己骑车去爷爷家送。'),
            ('DeliverPackage', 'Riding', '接着按刚才说好的送。', 'ContinueCurrent', None, '好，我接着送。'),
            ('Follow', 'OnFoot', '骑车去爷爷家那边吧。', 'ContinueCurrent', None, '要我跟着你去，还是替你把这份送过去？'),
        )
        for active, owner_vehicle, line, action, mode, speech in cases:
            with self.subTest(active=active, selected=(action, mode)):
                data = travel_request()
                data['capability_version'] = 10
                snapshot = data['companion_context']
                is_errand = active == 'DeliverPackage'
                snapshot.update(mode='Follow', task_id='Task_001', task_phase='InProgress',
                                owner_has_package=True, owner_package_location='RegisteredBikeTrunk',
                                owner_vehicle_state=owner_vehicle, vehicle_state='NotMounted',
                                errand_status='Running' if is_errand else 'Idle',
                                errand_action=active if is_errand else 'None',
                                errand_phase='ReceiveFromOwner' if is_errand else 'Idle')
                if not is_errand:
                    snapshot['errand_travel_mode'] = 'Unspecified'
                latest = data['evidence'][0]
                latest.update(kind='PlayerSpeech', source='Heard', description=line)
                if is_errand and action == 'Follow':
                    data['evidence'].insert(0, dict(event_id=new_id(), subject_id=data['npc_id'], kind='SelfSpeech',
                                                   source='ActionResult', description='我自己去送这单。'))
                data['allowed_actions'].append('DeliverPackage')
                data['candidates'].extend({**option, 'candidate_id': new_id(), 'action': 'DeliverPackage'}
                                          for option in list(data['candidates']))
                add_continuation(data, active)
                data['current_activity'].update(mode='Follow', errand_action=active if is_errand else '',
                                                errand_phase=snapshot['errand_phase'] if is_errand else '')
                validate_context(data)
                instructions = companion_instructions(data)
                self.assertLess(instructions.index(FOLLOW_ERRAND_INTENT_INSTRUCTIONS), instructions.index(ERRAND_INSTRUCTIONS))
                self.assertIn('仅凭订单存在、包裹或收件地点的位置，不能成立代送委托', instructions)
                self.assertIn('按主人最新的更正执行，恢复同行选 Follow', instructions)
                self.assertIn('已有明确代送时，单纯催促或要求原样继续沿用该委托', instructions)
                self.assertIn('由你自己按服务器登记的地址去找收件人', instructions)
                self.assertIn('只有出行方式尚未确认或实际目的地不清楚时才追问', instructions)
                for compress in (compact_context, model_context):
                    payload = compress(data)
                    self.assertEqual(payload['current_action']['action'], active)
                    self.assertEqual(payload['pending_player_message']['description'], line)
                    self.assertTrue({'Follow', 'DeliverPackage', 'ContinueCurrent'} <=
                                    {option['action'] for option in payload['candidates']})
                index = next(i for i, option in enumerate(data['candidates'])
                             if option['action'] == action and option.get('travel_mode') == mode)
                events = []
                parser = SentenceParser(data, validate_action, lambda kind, value: events.append((kind, value)))
                parser.line(json.dumps(dict(candidate_index=index, speech=speech, emotion='Calm')))
                self.assertEqual(events[0][1]['candidate_id'], data['candidates'][index]['candidate_id'])

    def test_errand_travel_mode_is_optional_and_only_accepts_v8_mode_values(self):
        data = travel_request(modes=(None,))
        validate_context(data)
        for mode in (None, True, '', 'Teleport', [], 1):
            data['companion_context']['errand_travel_mode'] = mode
            with self.subTest(mode=mode), self.assertRaisesRegex(ValueError, 'errand travel mode'):
                validate_context(data)
        data['companion_context']['errand_travel_mode'] = 'Walk'
        data['capability_version'] = 7
        with self.assertRaisesRegex(ValueError, 'errand travel mode'):
            validate_context(data)
        del data['companion_context']['errand_travel_mode']
        validate_context(data)

    def test_completed_errand_stays_bound_to_its_order_after_switch(self):
        data = travel_request('DeliverPackage', modes=('Walk',))
        snapshot = data['companion_context']
        snapshot.update(task_id='Task_002', task_phase='AwaitingPickup', errand_task_id='Task_001',
                        errand_action='DeliverPackage', errand_status='Succeeded', errand_reason='Delivered')
        data['evidence'][0]['description'] = '刚换的这一单已经送了吗？'
        before = copy.deepcopy(data)
        validate_context(data)
        self.assertIn(ERRAND_TASK_INSTRUCTIONS, companion_instructions(data))
        for compress in (compact_context, model_context):
            self.assertEqual(compress(data)['companion_context'], snapshot)
        selected = validate_action(data, data['candidates'][0])
        self.assertEqual(selected['candidate_id'], data['candidates'][0]['candidate_id'])
        self.assertNotIn('errand_task_id', selected)
        self.assertEqual(data, before)

    def test_owner_confirmed_delivery_survives_new_order_follow_and_past_decision(self):
        data = travel_request(modes=(None,))
        add_continuation(data, 'Follow')
        data['current_activity'].update(mode='Follow', errand_action='', errand_phase='')
        data['companion_context'].update(mode='Follow', task_id='Task_003', task_phase='AwaitingPickup',
                                         errand_task_id='Task_002', errand_action='TakePackage',
                                         errand_status='Cancelled', errand_reason='OwnerChangedArrangement')
        player = data['evidence'][0]
        player['description'] = '你现在接着跟我走。最开始那单送到了吗？'
        delivery = dict(event_id=new_id(), kind='CompanionTaskDelivered', subject_id='', source='ActionResult',
                        description='服务器确认订单 Task_001 的真实包裹已送达。')
        # UE 随主人请求重新附上这条事实，原编号可以已经被模型读过
        data['evidence'].append(delivery)
        data['last_decision_event_ids'] = [delivery['event_id']]
        for wire_fields in ({}, {'needs_reply': False}):
            with self.subTest(wire_fields=wire_fields):
                data['evidence'][-1] = {**delivery, **wire_fields}
                before = copy.deepcopy(data)
                validate_context(data)
                self.assertIn(TASK_DELIVERED_INSTRUCTIONS, companion_instructions(data))
                for compress in (compact_context, model_context):
                    payload = compress(data)
                    delivered = next(item for item in payload['evidence'] if item['kind'] == delivery['kind'])
                    self.assertEqual(delivered['description'], delivery['description'])
                    self.assertEqual(delivered['source'], 'ActionResult')
                    self.assertIs(delivered.get('needs_reply', False), False)
                    self.assertNotIn(delivered['event_id'], payload['new_event_ids'])
                    self.assertEqual(payload['pending_player_message']['description'], player['description'])
                    self.assertEqual(payload['pending_player_message']['event_id'], payload['current_player_message']['event_id'])
                    self.assertEqual(payload['companion_context']['task_id'], 'Task_003')
                    self.assertEqual(payload['companion_context']['errand_task_id'], 'Task_002')
                    self.assertEqual(payload['current_activity']['mode'], 'Follow')
                self.assertEqual(data, before)

    def test_delivery_guidance_requires_an_owner_request_and_a_game_fact(self):
        data = travel_request(modes=(None,))
        self.assertNotIn(TASK_DELIVERED_INSTRUCTIONS, companion_instructions(data))
        delivery = dict(event_id=new_id(), kind='CompanionTaskDelivered', subject_id='', source='ActionResult',
                        description='服务器确认订单 Task_001 的真实包裹已送达。', needs_reply=False)
        for change in ({'source': 'Heard'}, {'kind': 'OtherFact'}, {'needs_reply': True}):
            with self.subTest(change=change):
                updated = {**data, 'evidence': [*data['evidence'], {**delivery, **change}]}
                self.assertNotIn(TASK_DELIVERED_INSTRUCTIONS, companion_instructions(updated))
        data['evidence'].append(delivery)
        data['companion_context']['task_id'] = ''
        self.assertIn(TASK_DELIVERED_INSTRUCTIONS, companion_instructions(data))
        for change in ({'request_source': 'ambient'}, {'capability_version': 7},
                       {'companion_context': {**data['companion_context'], 'requester_is_owner': False}}):
            with self.subTest(change=change):
                self.assertNotIn(TASK_DELIVERED_INSTRUCTIONS, companion_instructions({**data, **change}))

    def test_errand_task_id_is_optional_bounded_v8_metadata(self):
        data = travel_request(modes=(None,))
        validate_context(data)
        self.assertNotIn(ERRAND_TASK_INSTRUCTIONS, companion_instructions(data))
        for task_id in ('Task_001', 'T' * 64):
            data['companion_context']['errand_task_id'] = task_id
            with self.subTest(task_id=task_id):
                validate_context(data)
                self.assertEqual(model_context(data)['companion_context']['errand_task_id'], task_id)
        for task_id in (None, True, [], '', 'T' * 65, ' ', 'Task_001\n'):
            data['companion_context']['errand_task_id'] = task_id
            with self.subTest(task_id=task_id), self.assertRaisesRegex(ValueError, 'errand task id'):
                validate_context(data)
        previous = item_request('FetchItem', 7)
        validate_context(previous)
        previous['companion_context']['errand_task_id'] = 'Task_001'
        with self.assertRaisesRegex(ValueError, 'errand task id'):
            validate_context(previous)
        for owner, source in ((False, 'player'), (True, 'ambient')):
            with self.subTest(owner=owner, source=source):
                data['request_source'] = source
                data['companion_context'].update(requester_is_owner=owner, errand_task_id='Task_001')
                with self.assertRaisesRegex(ValueError, 'errand task id'):
                    validate_context(data)

    def test_task_door_state_preserves_the_current_order_and_separate_errand(self):
        data = travel_request('DeliverPackage', modes=('Walk',))
        snapshot = data['companion_context']
        snapshot.update(task_id='Task_002', task_phase='InProgress', errand_task_id='Task_001',
                        errand_action='DeliverPackage', errand_status='Succeeded', errand_reason='Delivered')
        validate_context(data)
        self.assertIn(TASK_DOOR_UNKNOWN_INSTRUCTIONS, companion_instructions(data))
        self.assertNotIn('task_door_state', model_context(data)['companion_context'])
        for state in ('Intact', 'Broken'):
            with self.subTest(state=state):
                snapshot['task_door_state'] = state
                validate_context(data)
                instructions = companion_instructions(data)
                self.assertIn(TASK_DOOR_STATE_INSTRUCTIONS, instructions)
                self.assertNotIn(TASK_DOOR_UNKNOWN_INSTRUCTIONS, instructions)
                for compress in (compact_context, model_context):
                    self.assertEqual(compress(data)['companion_context'], snapshot)
                self.assertNotIn('task_door_state', validate_action(data, data['candidates'][0]))

    def test_task_door_state_is_optional_owner_v8_metadata_with_current_order(self):
        data = travel_request(modes=(None,))
        snapshot = data['companion_context']
        snapshot.update(task_id='Task_001', task_phase='AwaitingPickup', task_door_state='Broken')
        validate_context(data)
        for state in (None, True, '', 'Open', 'Unknown', []):
            with self.subTest(state=state), self.assertRaisesRegex(ValueError, 'task door state'):
                validate_context({**data, 'companion_context': {**snapshot, 'task_door_state': state}})
        for mutation in ({'task_id': ''}, {'task_id': ' '}, {'requester_is_owner': False},
                         {'task_phase': 'Completed'}, {'task_phase': 'Offered'}):
            with self.subTest(mutation=mutation), self.assertRaisesRegex(ValueError, 'task door state'):
                validate_context({**data, 'companion_context': {**snapshot, **mutation}})
        for envelope in ({'request_source': 'ambient'}, {'capability_version': 7}):
            with self.subTest(envelope=envelope), self.assertRaisesRegex(ValueError, 'task door state'):
                validate_context({**data, **envelope})
        del snapshot['task_door_state']
        validate_context(data)
        self.assertNotIn(TASK_DOOR_UNKNOWN_INSTRUCTIONS, companion_instructions({**data, 'capability_version': 7}))

    def test_vehicle_state_reports_current_seat_without_changing_travel_choice(self):
        data = travel_request()
        data['companion_context']['travel_mode'] = 'OwnVehicle'
        validate_context(data)
        self.assertNotIn(VEHICLE_STATE_INSTRUCTIONS, companion_instructions(data))
        candidate = data['candidates'][0]
        for state in ('Mounted', 'ParkedForWalkingFollow', 'NotMounted'):
            with self.subTest(state=state):
                data['companion_context']['vehicle_state'] = state
                validate_context(data)
                for payload in (compact_context(data), model_context(data)):
                    self.assertEqual(payload['companion_context']['vehicle_state'], state)
                    self.assertEqual(payload['companion_context']['travel_mode'], 'OwnVehicle')
                self.assertIn(VEHICLE_STATE_INSTRUCTIONS, companion_instructions(data))
                self.assertNotIn('travel_mode', validate_action(data, candidate))

    def test_vehicle_state_rejects_unknown_values_and_is_optional_on_v8(self):
        data = travel_request()
        for state in (None, True, [], '', 'Moving'):
            data['companion_context']['vehicle_state'] = state
            with self.subTest(state=state), self.assertRaisesRegex(ValueError, 'vehicle state'):
                validate_context(data)
        del data['companion_context']['vehicle_state']
        validate_context(data)
        previous = item_request('FetchItem', 7)
        validate_context(previous)
        previous['companion_context']['vehicle_state'] = 'Mounted'
        with self.assertRaisesRegex(ValueError, 'vehicle state'):
            validate_context(previous)

    def test_owner_vehicle_state_is_private_optional_and_separate_from_companion_seat(self):
        data = travel_request()
        data['capability_version'] = 10
        snapshot = data['companion_context']
        snapshot.update(travel_mode='OwnVehicle', vehicle_state='NotMounted')
        validate_context(data)
        for state in ('OnFoot', 'Riding'):
            snapshot['owner_vehicle_state'] = state
            validate_context(data)
            for payload in (model_context(data), compact_context(data)):
                self.assertEqual(payload['companion_context']['owner_vehicle_state'], state)
                self.assertEqual(payload['companion_context']['vehicle_state'], 'NotMounted')
            self.assertIn('owner_vehicle_state 只指主人', companion_instructions(data))
        for other in ('visitor', 'ambient', 'v8', 'invalid'):
            rejected = copy.deepcopy(data)
            if other == 'visitor':
                rejected['companion_context']['requester_is_owner'] = False
            elif other == 'ambient':
                rejected['request_source'] = 'ambient'
            elif other == 'v8':
                rejected['capability_version'] = 8
            else:
                rejected['companion_context']['owner_vehicle_state'] = 'Mounted'
            with self.subTest(other=other), self.assertRaisesRegex(ValueError, 'owner vehicle state'):
                validate_context(rejected)

    def test_real_item_location_survives_regular_and_compact_prompts(self):
        task_package = travel_request('DeliverPackage', modes=('OwnVehicle',))
        task_package['companion_context'].update(task_id='Task_001', task_phase='InProgress', has_package=True)
        ordinary_item = item_request('ReturnItem', 8)
        ordinary_item['companion_context'].update(task_id='Task_002', task_phase='InProgress',
                                                 owner_has_package=True, owner_package_location='RegisteredBikeTrunk')
        for data, location, seat in ((task_package, 'OwnBikeRack', 'Mounted'),
                                     (ordinary_item, 'Hand', 'NotMounted'),
                                     (ordinary_item, 'OwnBikeRack', 'Mounted')):
            with self.subTest(location=location, package=data['companion_context']['has_package']):
                snapshot = data['companion_context']
                snapshot.update(carried_item_location=location, vehicle_state=seat)
                validate_context(data)
                result = {**data['candidates'][0], 'speech': '我知道这件东西在哪里。', 'emotion': 'Calm'}
                with patch('codex_local.choose', return_value=result) as mocked:
                    action = decide(data, 'codex-local')
                regular, instructions = mocked.call_args.args
                self.assertEqual(regular['companion_context']['carried_item_location'], location)
                self.assertIn(CARRIED_ITEM_LOCATION_INSTRUCTIONS, instructions)
                for compress in (compact_context, model_context):
                    payload = compress(data)['companion_context']
                    self.assertEqual(payload['carried_item_location'], location)
                    self.assertEqual(payload.get('owner_package_location'), snapshot.get('owner_package_location'))
                if data is ordinary_item:
                    self.assertIn(OWNER_PACKAGE_LOCATION_INSTRUCTIONS, instructions)
                self.assertNotIn('carried_item_location', action)
                self.assertNotIn('owner_package_location', action)

    def test_location_metadata_needs_observed_custody_and_owner_request(self):
        data = travel_request(modes=(None,))
        snapshot = data['companion_context']
        snapshot.update(vehicle_state='Mounted', task_id='Task_001', task_phase='InProgress',
                        has_package=True, owner_has_package=True)
        validate_context(data)
        self.assertNotIn(CARRIED_ITEM_LOCATION_INSTRUCTIONS, companion_instructions(data))
        self.assertNotIn(OWNER_PACKAGE_LOCATION_INSTRUCTIONS, companion_instructions(data))
        self.assertNotIn('carried_item_location', model_context(data)['companion_context'])
        snapshot['owner_package_location'] = 'Hand'
        validate_context(data)
        self.assertEqual(model_context(data)['companion_context']['owner_package_location'], 'Hand')
        del snapshot['owner_package_location']

        for field, state, mutations in (
            ('carried_item_location', 'OwnBikeRack', (dict(has_package=False), dict(vehicle_state='NotMounted'))),
            ('owner_package_location', 'RegisteredBikeTrunk', (dict(owner_has_package=False), dict(task_id=' '))),
        ):
            error = field.replace('_', ' ')
            for invalid in (None, True, [], 'Unknown', 'Trunk'):
                with self.subTest(field=field, invalid=invalid), self.assertRaisesRegex(ValueError, error):
                    validate_context({**data, 'companion_context': {**snapshot, field: invalid}})
            for mutation in (*mutations, {'requester_is_owner': False}):
                with self.subTest(field=field, mutation=mutation), self.assertRaisesRegex(ValueError, error):
                    validate_context({**data, 'companion_context': {**snapshot, field: state, **mutation}})
            observed = {**data, 'companion_context': {**snapshot, field: state}}
            validate_context(observed)
            with self.subTest(field=field, source='ambient'), self.assertRaisesRegex(ValueError, error):
                validate_context({**observed, 'request_source': 'ambient'})
            previous = item_request('ReturnItem', 7)
            previous['companion_context'].update({field: 'Hand', 'owner_has_package': True, 'task_id': 'Task_001'})
            with self.subTest(field=field, version=7), self.assertRaisesRegex(ValueError, error):
                validate_context(previous)

    def test_pacing_guidance_only_appears_when_server_offers_pace(self):
        data = travel_request()
        self.assertNotIn(PACE_INSTRUCTIONS, companion_instructions(data))
        data['allowed_actions'].append('Pace')
        data['candidates'].append(dict(candidate_id=new_id(), action='Pace', target_id='', duration=5, evidence_ids=[]))
        validate_context(data)
        self.assertIn(PACE_INSTRUCTIONS, companion_instructions(data))

    def test_visitor_can_reply_while_companion_keeps_pacing(self):
        data = travel_request()
        data['companion_context'].update(requester_is_owner=False, mode='Stay')
        data['pending_player_message'] = True
        data['current_action'] = dict(action='Pace', action_id=new_id(), target_id='', source='Model',
                                      remaining_seconds=3, plan_revision=5)
        data['current_activity'] = dict(kind='companion', mode='Stay', errand_action='', errand_phase='',
                                        has_home_arrangement=False)
        continuation = dict(candidate_id=new_id(), action='ContinueCurrent', target_id='', duration=0, evidence_ids=[])
        data['allowed_actions'] = ['ContinueCurrent']
        data['candidates'] = [continuation]
        validate_context(data)
        self.assertEqual(model_context(data)['current_action']['action'], 'Pace')
        self.assertNotIn(PACE_INSTRUCTIONS, companion_instructions(data))
        self.assertEqual(validate_action(data, {**continuation, 'speech': '我在等人呢。'})['action'], 'ContinueCurrent')
        with self.assertRaisesRegex(ValueError, 'requires a player reply'):
            validate_action(data, continuation)

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
