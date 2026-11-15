import json
import unittest

from audit_companion_playtest import audit, compact_position


STAMP = '2026.11.14-21.26.00:824'
STAMPED_LINE = f'[{STAMP}][490]'


class CompanionPlaytestAuditTests(unittest.TestCase):
    def test_joined_reply_receipt_keeps_pending_separate_from_played(self):
        event = '11111111111111111111111111111111'
        reply = '22222222222222222222222222222222'
        lines = [
            f'{STAMPED_LINE}LogDeliveryNpcAgent: CompanionJoined response pending npc=Ergou event={event}',
            f'{STAMPED_LINE}LogDeliveryNpcAgent: CompanionJoined response played npc=Ergou event={event}',
            f'{STAMPED_LINE}LogTemp: CompanionJoined response played npc=Ergou event={event} reply={reply}',
            f'{STAMPED_LINE}LogDeliveryNpcAgent: CompanionJoined response played npc=Ergou event={event} reply={reply}',
        ]
        report = audit([line.encode() for line in lines], (), None, None)
        pending, played = report['editor_gameplay_events_all_worlds']
        self.assertEqual(pending['kind'], 'companion_joined_reply_pending')
        self.assertIsNone(pending['reply_id'])
        self.assertEqual((played['kind'], played['event_id'], played['reply_id']),
                         ('companion_joined_reply_played', event, reply))

    def test_action_timeout_is_reported_without_inventing_request_correlation(self):
        npc = '22222222222222222222222222222222'
        action = '33333333333333333333333333333333'
        rows = [
            f'{STAMPED_LINE}LogDeliveryNpcAgent: Display: Action npc={npc} action={action} revision=14 result=Failed reason=ExecutorTimeout',
            f'{STAMPED_LINE}LogDeliveryNpcAgent: Display: Action npc={npc} action=bad revision=15 result=Succeeded reason=CompanionErrandAccepted',
            f'{STAMPED_LINE}LogTemp: Display: Action npc={npc} action={action} revision=14 result=Succeeded reason=Delivered',
        ]
        report = audit([(row+'\r').encode() for row in rows], (), None, None)
        events = report['editor_gameplay_events_all_worlds']
        self.assertEqual(len(events), 1)
        self.assertEqual((events[0]['status'], events[0]['reason']), ('Failed', 'ExecutorTimeout'))
        self.assertEqual((events[0]['action_id'], events[0]['plan_revision']), (action, 14))
        self.assertNotIn('request', events[0])

    def test_jump_results_match_world_actor_and_path_not_action_selection(self):
        second = '2026.11.14-21.27.00:824'
        rows = [
            f'{STAMPED_LINE}LogWorld: Bringing World /Game/UEDPIE_0_Test up for play',
            f'{STAMPED_LINE}LogDeliveryNpcTraversal: NpcObstacleJumpStarted npc=Ergou move=9 obstacle=Threshold openDoorwayAhead=1 from=V(0) landing=V(X=120.00, Z=50.00)',
            f'{STAMPED_LINE}LogDeliveryNpcTraversal: NpcObstacleJumpStarted npc=Resident move=9 obstacle=Threshold openDoorwayAhead=0 from=V(Y=20.00) landing=V(X=120.00, Y=20.00)',
            '[2026.11.14-21.26.01:424][505]LogDeliveryNpcTraversal: NpcObstacleJumpLanded npc=Ergou move=9 reached=1 feet=V(X=120.00, Z=50.00)',
            f'{STAMPED_LINE}LogTemp: NpcObstacleJumpLanded npc=Resident move=9 reached=1 feet=V(0)',
            f'[{second}][700]LogWorld: Bringing World /Game/UEDPIE_0_Test up for play',
            f'[{second}][701]LogDeliveryNpcTraversal: NpcObstacleJumpLanded npc=Resident move=9 reached=0 feet=V(X=15.00)',
        ]
        report = audit([(row+'\r').encode() for row in rows], (), None, None)
        attempts = report['editor_obstacle_jump_attempts_all_worlds']
        self.assertEqual(len(attempts), 3)
        self.assertEqual((attempts[0]['outcome'], attempts[0]['elapsed_ms']), ('landed_at_checked_point', 600))
        self.assertEqual(attempts[1]['outcome'], 'landing_not_observed')
        self.assertIsNone(attempts[1]['reached_landing'])
        self.assertEqual(attempts[2]['outcome'], 'landing_without_observed_start')
        self.assertFalse(attempts[2]['reached_landing'])
        self.assertEqual(attempts[2]['editor_world_start'], second)
        self.assertEqual(report['editor_gameplay_events_all_worlds'][0]['from_cm'], (0., 0., 0.))

    def test_repeated_jumps_on_same_path_do_not_fill_an_earlier_missing_landing(self):
        rows = [
            f'{STAMPED_LINE}LogDeliveryNpcTraversal: NpcObstacleJumpStarted npc=Ergou move=12 obstacle=Threshold openDoorwayAhead=0 from=V(0) landing=V(X=120.00)',
            f'{STAMPED_LINE}LogDeliveryNpcTraversal: NpcObstacleJumpStarted npc=Ergou move=12 obstacle=Threshold openDoorwayAhead=0 from=V(X=30.00) landing=V(X=150.00)',
            f'{STAMPED_LINE}LogDeliveryNpcTraversal: NpcObstacleJumpLanded npc=Ergou move=12 reached=0 feet=V(X=125.00)',
        ]
        report = audit([row.encode() for row in rows], (), None, None)
        attempts = report['editor_obstacle_jump_attempts_all_worlds']
        self.assertEqual([attempt['outcome'] for attempt in attempts], ['landing_not_observed', 'landed_elsewhere'])

    def test_ground_release_requires_server_event_not_snapshot_or_llm_choice(self):
        npc = '22222222-2222-2222-2222-222222222222'
        request = '33333333-3333-3333-3333-333333333333'
        dialogue = json.dumps({'npc_id': npc, 'request_id': request, 'profile_id': 'ergou',
                               'chosen': 'DropPackage:owner', 'model_context': {
                                   'companion_context': {'requester_is_owner': True,
                                                         'errand_reason': 'PackagePlacedOnGround'}}}).encode()
        report = audit((), [dialogue], None, None)
        self.assertEqual(report['editor_gameplay_events_all_worlds'], [])
        self.assertEqual(report['companion_turns'][0]['observed_before_decision']['package_outcome'], 'PackagePlacedOnGround')
        rows = [
            f'{STAMPED_LINE}LogTemp: CompanionPackagePlacedOnGround npc=Ergou package=WrongCategory revision=4 location=V(0)',
            f'{STAMPED_LINE}LogDelivery: CompanionPackagePlacedOnGround npc=Ergou package=OriginalPackage revision=4 location=V(X=150.00, Z=6.00)',
            f'{STAMPED_LINE}LogDelivery: CompanionPackagePlacedOnGround npc=Ergou package=BadPosition revision=4 location=V(X=nan)',
        ]
        report = audit([row.encode() for row in rows], [dialogue], None, None)
        events = report['editor_gameplay_events_all_worlds']
        self.assertEqual(len(events), 1)
        self.assertEqual((events[0]['kind'], events[0]['package'], events[0]['revision']),
                         ('companion_package_placed_on_ground', 'OriginalPackage', 4))
        self.assertEqual(events[0]['location_cm'], (150., 0., 6.))

    def test_compact_vectors_keep_zero_coordinates_and_reject_invalid_values(self):
        self.assertEqual(compact_position('V(0)'), (0., 0., 0.))
        self.assertEqual(compact_position('V(Y=-1.25, Z=50.00)'), (0., -1.25, 50.))
        for vector in ('V()', 'V(X=nan)', 'V(Z=inf)', 'V(X=5, X=6)', 'V(A=2)', 'V(XYZ=1)'):
            with self.subTest(vector=vector):
                self.assertIsNone(compact_position(vector))
        rows = [
            f'{STAMPED_LINE}LogDeliveryCompanion: CompanionFollowMove npc=Ergou revision=2 result=2 from=V(0) owner=V(X=300.00) travel=OnFoot',
            f'{STAMPED_LINE}LogDeliveryCompanion: CompanionFollowMove npc=Ergou revision=2 result=2 from=V(X=100.00) owner=V(X=350.00) travel=OnFoot',
        ]
        report = audit([row.encode() for row in rows], (), None, None)
        samples = report['editor_follow_position_samples_all_worlds']
        self.assertEqual(samples[0]['npc_net_displacement_cm'], 100.)
        self.assertEqual(samples[0]['last_horizontal_separation_cm'], 250.)

    def test_first_order_events_require_explicit_server_results(self):
        lines = [
            f'{STAMPED_LINE}LogWorld: Bringing World /Game/UEDPIE_0_TestForCharacter up for play',
            f'{STAMPED_LINE}LogDelivery: [Task] Task_001 已接听，开始计时',
            f'{STAMPED_LINE}LogDelivery: [Task] Task_001 已取件，玩家 PrivatePlayer（计时未重置）',
            f'{STAMPED_LINE}LogDeliveryCompanionBike: CompanionMounted npc=Ergou bike=ErgouBike revision=2',
            f'{STAMPED_LINE}LogDeliveryCompanionBike: CompanionDismounted npc=Ergou bike=ErgouBike revision=2',
            f'{STAMPED_LINE}LogDelivery: Grandpa knock=2 doorBroken=0',
            f'{STAMPED_LINE}LogTemp: DoorOpenedByAxe door=UnrelatedDoor hits=3',
            f'{STAMPED_LINE}LogDelivery: DoorAxeHit door=GrandpaDoor attacker=PrivatePlayer hit=3/3',
            f'{STAMPED_LINE}LogDelivery: DoorOpenedByAxe door=GrandpaDoor hits=3',
            f'{STAMPED_LINE}LogDeliveryWallet: [Wallet] PrivatePlayer 送达 Task_001，入账 120（底薪 100 × 时间 1.20 × 特殊 1.00），余额 400',
            f'{STAMPED_LINE}LogDelivery: [Task] Task_001 完成，用时 26.2s，奖励 120，交付玩家 PrivatePlayer',
            f'{STAMPED_LINE}LogDelivery: Grandpa received home delivery event=7AA59A12-A391-47E7-9C18-F157BF3A43EC task=Task_001 player=PrivatePlayer',
            f'{STAMPED_LINE}LogDelivery: Grandpa received home delivery event=incomplete task=Task_002 player=PrivatePlayer',
            f'{STAMPED_LINE}LogTemp: [Wallet] PrivatePlayer 送达 Task_002，入账 120（底薪 100），余额 400',
        ]
        report = audit([line.encode() for line in lines], (), None, None)
        events = report['editor_gameplay_events_all_worlds']
        self.assertEqual([event['kind'] for event in events], [
            'task_accepted', 'task_picked_up', 'companion_mounted', 'companion_dismounted',
            'grandpa_knock', 'door_axe_hit', 'door_opened_by_axe', 'wallet_task_paid', 'task_completed',
            'grandpa_received_delivery',
        ])
        self.assertTrue(all(event['world_start'] == STAMP for event in events))
        self.assertEqual(events[2]['bike'], 'ErgouBike')
        self.assertFalse(events[4]['door_broken'])
        self.assertEqual((events[5]['hit_count'], events[5]['required_hits']), (3, 3))
        self.assertEqual(events[-3], {'world_start': STAMP, 'kind': 'wallet_task_paid', 'task': 'Task_001', 'reward': 120})
        self.assertEqual((events[-2]['task'], events[-2]['reward']), ('Task_001', 120))
        self.assertEqual(events[-1], {'world_start': STAMP, 'kind': 'grandpa_received_delivery', 'task': 'Task_001',
                                     'event_id': '7aa59a12a39147e79c18f157bf3a43ec'})
        self.assertEqual(report['editor_test_markers'], [])
        self.assertNotIn('PrivatePlayer', json.dumps(report))

    def test_marks_known_test_launches_and_runtime_automation(self):
        for line, marker in (
            ('LogInit: Command Line: -ExecCmds="Automation RunTests Delivery.NPC.Grandpa"', 'ue_automation'),
            ('LogInit: Command Line: -game -ExecCmds=Delivery.Delivery.Smoke', 'delivery_smoke'),
            (f'{STAMPED_LINE}LogAutomationController: Display: Test Started. Name={{Grandpa}}', 'ue_automation'),
        ):
            with self.subTest(line=line):
                report = audit([line.encode()], (), None, None)
                self.assertEqual(report['editor_test_markers'], [marker])

        manual_launch = b'LogInit: Command Line: /Game/Level/TestForCharacter -ExecCmds="t.MaxFPS 60"'
        report = audit([manual_launch], (), None, None)
        self.assertEqual(report['editor_test_markers'], [])
