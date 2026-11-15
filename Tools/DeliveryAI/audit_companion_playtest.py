"""把已有二狗决策日志与 UE 服务器结果对照，不读取或输出玩家原话。"""

import argparse
from collections import Counter
from datetime import datetime, time
import json
from math import ceil, dist, isfinite
from pathlib import Path
import re
from uuid import UUID


DECISION = re.compile(r'Decision npc=(\w+) request=(\w+) accepted=([01]) reason=(\S+)')
FOLLOW = re.compile(r'CompanionFollowMove npc=(\S+) revision=(\d+) result=(\d+) .* travel=(\w+)')
POSITION = r'V\([^()\r\n]*\)'
FOLLOW_POSITIONS = re.compile(rf'from=({POSITION}).*?owner=({POSITION})')
LATENCY = re.compile(r'^\[([\d.:-]+)\].*NpcLatency request=(\w+) stage=(http_start|approved|model_done)\b')
PACKAGE_RESULTS = {'PackageReceived', 'PackageReturned', 'PackageStoredInOwnerBackpack', 'PackagePlacedOnGround', 'Delivered'}
SUSPENSION = re.compile(r'CompanionSuspension npc=(\S+) revision=(\d+) mode=(\d+) reason=(\S+)')
RIDE_TRANSITION = re.compile(r'Companion(Mounted|Dismounted) npc=(\S+) bike=(\S+) revision=(-?\d+)')
SERVER_MESSAGE = re.compile(r'^\[[\d.:-]+\]\[\s*\d+\]LogDelivery: (.*)')
TASK_PROGRESS = re.compile(r'\[Task\] (\S+) (已接听，开始计时|已取件，玩家 )')
TASK_COMPLETE = re.compile(r'\[Task\] (\S+) 完成，用时 ([\d.]+)s，奖励 (\d+)')
GRANDPA_KNOCK = re.compile(r'Grandpa knock=(\d+) doorBroken=([01])\b')
GRANDPA_RECEIPT = re.compile(r'Grandpa received home delivery event=(\S+) task=(\S+) player=\S+')
WALLET_PAYMENT = re.compile(r'^\[[\d.:-]+\]\[\s*\d+\]LogDeliveryWallet: \[Wallet\] \S+ 送达 (\S+)，入账 (\d+)（')
DOOR_AXE_HIT = re.compile(r'DoorAxeHit door=(\S+) attacker=\S+ hit=(\d+)/(\d+)\b')
DOOR_OPENED = re.compile(r'DoorOpenedByAxe door=(\S+) hits=(\d+)\b')
AUTOMATION_RUN = re.compile(
    r'^LogInit: Command Line: .*?-ExecCmds="?Automation RunTests\b'
    r'|^\[[\d.:-]+\]\[\s*\d+\](?:Automation: RunTests=|LogAutomationController: Display: Test Started\.)',
    re.IGNORECASE,
)
DELIVERY_SMOKE = re.compile(r'^LogInit: Command Line: .*?-ExecCmds="?Delivery\.Delivery\.Smoke\b', re.IGNORECASE)
BACKEND_MARK = re.compile(r'\blatency request=(\S+) stage=(\w+) ([^\r\n]+)')
BACKEND_FIELDS = re.compile(r'\b(elapsed_ms|tts_queue_ms|tts_ms|sentence)=(\d+)\b')
TRAVERSAL_MESSAGE = re.compile(r'^\[([\d.:-]+)\]\[\s*\d+\]LogDeliveryNpcTraversal: (.*)')
JUMP_START = re.compile(rf'^NpcObstacleJumpStarted npc=(\S+) move=(\d+) obstacle=(\S+) openDoorwayAhead=([01]) from=({POSITION}) landing=({POSITION})$')
JUMP_LAND = re.compile(rf'^NpcObstacleJumpLanded npc=(\S+) move=(\d+) reached=([01]) feet=({POSITION})$')
PACKAGE_GROUND = re.compile(rf'^CompanionPackagePlacedOnGround npc=(\S+) package=(\S+) revision=(\d+) location=({POSITION})$')
ACTION_RESULT = re.compile(r'^\[([\d.:-]+)\]\[\s*\d+\]LogDeliveryNpcAgent: Display: Action npc=(\w+) action=(\w+) revision=(\d+) result=(Succeeded|Failed|Cancelled) reason=(\S+)$')
JOINED_REPLY = re.compile(r'^\[([\d.:-]+)\]\[\s*\d+\]LogDeliveryNpcAgent: CompanionJoined response (pending|played) npc=(\S+) event=(\w+)(?: reply=(\w+))?$')


def identity(value):
    try:
        return UUID(value).hex
    except (TypeError, ValueError, AttributeError):
        return None


def capture(path):
    with path.open('rb') as stream:
        # 固定读取范围，保留下一次快照的新增内容，也跳过可能正在写入的半行
        length = stream.seek(0, 2)
        stream.seek(0)
        data = stream.read(length)
    return data.split(b'\n')[:-1]


def compact_position(value):
    # UE 的 ToCompactString 省略零分量，全零为 V(0)
    fields = value[2:-1]
    if fields == '0':
        return (0., 0., 0.)
    axes = {}
    for field in fields.split(', '):
        axis, separator, number = field.partition('=')
        if not separator or axis not in 'XYZ' or len(axis) != 1 or axis in axes:
            return None
        try:
            coordinate = float(number)
        except ValueError:
            return None
        if not isfinite(coordinate):
            return None
        axes[axis] = coordinate
    return tuple(axes.get(axis, 0.) for axis in 'XYZ')


def jump_attempts(events):
    attempts = []
    pending = {}
    for event in events:
        if event['kind'] not in ('npc_obstacle_jump_started', 'npc_obstacle_jump_landed'):
            continue
        key = (event['world_start'], event['actor'], event['move_request'])
        if event['kind'] == 'npc_obstacle_jump_started':
            attempt = {'editor_world_start': event['world_start'], 'actor': event['actor'],
                       'move_request': event['move_request'], 'start_at': event['time_utc'],
                       'land_at': None, 'reached_landing': None, 'elapsed_ms': None,
                       'outcome': 'landing_not_observed'}
            attempts.append(attempt)
            # 同一路线可再次跳跃，新起跳不能替前次补一个落地结果
            pending[key] = attempt
        else:
            attempt = pending.pop(key, None)
            if attempt is None:
                attempts.append({'editor_world_start': event['world_start'], 'actor': event['actor'],
                                 'move_request': event['move_request'], 'start_at': None,
                                 'land_at': event['time_utc'], 'reached_landing': event['reached_landing'],
                                 'elapsed_ms': None, 'outcome': 'landing_without_observed_start'})
                continue
            attempt.update(land_at=event['time_utc'], reached_landing=event['reached_landing'],
                           outcome='landed_at_checked_point' if event['reached_landing'] else 'landed_elsewhere')
            try:
                start, end = (datetime.strptime(value, '%Y.%m.%d-%H.%M.%S:%f')
                              for value in (attempt['start_at'], attempt['land_at']))
                if end >= start:
                    attempt['elapsed_ms'] = round((end-start).total_seconds()*1000)
            except ValueError:
                pass
    return attempts


def backend_timings(rows):
    marks = {}
    for row in rows:
        match = BACKEND_MARK.search(row.decode('utf-8', errors='replace'))
        if not match or not (request := identity(match[1])):
            continue
        values = {key: int(value) for key, value in BACKEND_FIELDS.findall(match[3])}
        if values.get('sentence', 0) != 0 or 'elapsed_ms' not in values:
            continue
        marks.setdefault(request, {}).setdefault(match[2], values)
    return marks


def latency_summary(turns):
    fields = {}
    for turn in turns:
        if not turn['owner_request']:
            continue
        for field, value in turn['backend_ms'].items():
            fields.setdefault(field, []).append(value)
    summary = {}
    for field, values in fields.items():
        values.sort()
        count = len(values)
        summary[field] = {'n': count, 'p50': values[ceil(count * .50) - 1],
                          'p90': values[ceil(count * .90) - 1], 'max': values[-1]}
    return summary


def audit(editor_rows, dialogue_rows, epoch, after, backend_rows=()):
    decisions = {}
    follow_requests = Counter()
    follow_positions = {}
    timing = {}
    world_start = None
    gameplay = []
    test_markers = set()
    for row in editor_rows:
        line = row.decode('utf-8', errors='replace').rstrip('\r\n')
        if AUTOMATION_RUN.search(line):
            test_markers.add('ue_automation')
        if DELIVERY_SMOKE.match(line):
            test_markers.add('delivery_smoke')
        if 'Bringing World' in line and 'UEDPIE' in line:
            world_start = line.partition(']')[0].lstrip('[')
        if match := ACTION_RESULT.fullmatch(line):
            npc, action = identity(match[2]), identity(match[3])
            if npc and action:
                gameplay.append({'world_start': world_start, 'time_utc': match[1],
                                 'kind': 'npc_action_result', 'npc': npc, 'action_id': action,
                                 'plan_revision': int(match[4]), 'status': match[5], 'reason': match[6]})
        if match := JOINED_REPLY.fullmatch(line):
            event, reply = identity(match[4]), identity(match[5])
            if event and (match[2] == 'pending' or reply):
                gameplay.append({'world_start': world_start, 'time_utc': match[1],
                                 'kind': f'companion_joined_reply_{match[2]}', 'actor': match[3],
                                 'event_id': event, 'reply_id': reply})
        if traversal := TRAVERSAL_MESSAGE.match(line):
            if match := JUMP_START.fullmatch(traversal[2]):
                origin, landing = compact_position(match[5]), compact_position(match[6])
                if origin is not None and landing is not None:
                    gameplay.append({'world_start': world_start, 'time_utc': traversal[1],
                                     'kind': 'npc_obstacle_jump_started', 'actor': match[1],
                                     'move_request': int(match[2]), 'obstacle': match[3],
                                     'open_doorway_ahead': match[4] == '1',
                                     'from_cm': origin, 'planned_landing_cm': landing})
            elif match := JUMP_LAND.fullmatch(traversal[2]):
                if (feet := compact_position(match[4])) is not None:
                    gameplay.append({'world_start': world_start, 'time_utc': traversal[1],
                                     'kind': 'npc_obstacle_jump_landed', 'actor': match[1],
                                     'move_request': int(match[2]), 'reached_landing': match[3] == '1',
                                     'feet_cm': feet})
        if match := SUSPENSION.search(line):
            gameplay.append({'world_start': world_start, 'kind': 'companion_suspension',
                             'actor': match[1], 'revision': int(match[2]), 'reason': match[4]})
        if match := RIDE_TRANSITION.search(line):
            transition, actor, bike, revision = match.groups()
            gameplay.append({'world_start': world_start, 'kind': f'companion_{transition.lower()}',
                             'actor': actor, 'bike': bike, 'revision': int(revision)})
        if match := WALLET_PAYMENT.match(line):
            gameplay.append({'world_start': world_start, 'kind': 'wallet_task_paid',
                             'task': match[1], 'reward': int(match[2])})
        if server_message := SERVER_MESSAGE.match(line):
            message = server_message[1]
            if match := TASK_PROGRESS.match(message):
                kind = 'task_picked_up' if match[2].startswith('已取件') else 'task_accepted'
                gameplay.append({'world_start': world_start, 'kind': kind, 'task': match[1]})
            elif match := GRANDPA_KNOCK.match(message):
                gameplay.append({'world_start': world_start, 'kind': 'grandpa_knock',
                                 'knock_count': int(match[1]), 'door_broken': match[2] == '1'})
            elif (match := GRANDPA_RECEIPT.match(message)) and (event := identity(match[1])):
                gameplay.append({'world_start': world_start, 'kind': 'grandpa_received_delivery',
                                 'event_id': event, 'task': match[2]})
            elif match := DOOR_AXE_HIT.match(message):
                gameplay.append({'world_start': world_start, 'kind': 'door_axe_hit',
                                 'door': match[1], 'hit_count': int(match[2]), 'required_hits': int(match[3])})
            elif match := DOOR_OPENED.match(message):
                gameplay.append({'world_start': world_start, 'kind': 'door_opened_by_axe',
                                 'door': match[1], 'hit_count': int(match[2])})
            elif match := TASK_COMPLETE.match(message):
                gameplay.append({'world_start': world_start, 'kind': 'task_completed',
                                 'task': match[1], 'elapsed_seconds': float(match[2]), 'reward': int(match[3])})
            elif match := PACKAGE_GROUND.fullmatch(message):
                if (position := compact_position(match[4])) is not None:
                    gameplay.append({'world_start': world_start, 'kind': 'companion_package_placed_on_ground',
                                     'actor': match[1], 'package': match[2], 'revision': int(match[3]),
                                     'location_cm': position})
        match = DECISION.search(line)
        if match:
            npc, request, accepted, reason = match.groups()
            key = (identity(npc), identity(request))
            if all(key):
                decisions.setdefault(key, set()).add((accepted == '1', reason))
        match = FOLLOW.search(line)
        if match and 'Ergou' in match[1]:
            follow_requests[(match[4], match[3])] += 1
            positions = FOLLOW_POSITIONS.search(line)
            if positions:
                origin, owner = (compact_position(value) for value in positions.groups())
                if origin is not None and owner is not None:
                    key = (world_start, match[1], int(match[2]), match[4])
                    samples = follow_positions.setdefault(key, [])
                    samples.append((origin, owner))
        match = LATENCY.search(line)
        if match and (request := identity(match[2])):
            stamp = datetime.strptime(match[1], '%Y.%m.%d-%H.%M.%S:%f')
            timing.setdefault(request, {}).setdefault(match[3], stamp)

    backend = backend_timings(backend_rows)
    entries = []
    skipped_json = 0
    for raw in dialogue_rows:
        try:
            row = json.loads(raw)
        except (ValueError, UnicodeError):
            skipped_json += 1
            continue
        if not isinstance(row, dict) or row.get('profile_id') != 'ergou':
            continue
        npc, request = identity(row.get('npc_id')), identity(row.get('request_id'))
        if epoch and identity(row.get('world_epoch')) != epoch:
            continue
        clock = row.get('time')
        if after and (not isinstance(clock, str) or clock < after):
            continue
        if not npc or not request:
            continue
        outcomes = decisions.get((npc, request), set())
        context = row.get('model_context') or {}
        companion = context.get('companion_context') or {}
        reason = companion.get('errand_reason')
        markers = timing.get(request, {})
        start = markers.get('http_start')
        elapsed = {
            stage: round((stamp - start).total_seconds() * 1000)
            for stage, stamp in markers.items() if stage != 'http_start' and start and stamp >= start
        }
        backend_mark = backend.get(request, {})
        stages = {'model_turn_start_from_job': 'model_connection_turn_start',
                  'first_model_delta_from_job': 'model_connection_first_delta',
                  'first_sentence_from_job': 'first_sentence',
                  'first_audio_chunk_from_job': 'first_audio_chunk'}
        backend_ms = {name: backend_mark[stage]['elapsed_ms'] for name, stage in stages.items()
                      if stage in backend_mark}
        for name, begin, end in (
            ('model_turn_start_to_first_delta', 'model_turn_start_from_job', 'first_model_delta_from_job'),
            ('first_delta_to_first_sentence', 'first_model_delta_from_job', 'first_sentence_from_job'),
        ):
            if begin in backend_ms and end in backend_ms and backend_ms[end] >= backend_ms[begin]:
                backend_ms[name] = backend_ms[end] - backend_ms[begin]
        first_chunk = backend_mark.get('first_audio_chunk', {})
        for field in ('tts_queue_ms', 'tts_ms'):
            if field in first_chunk:
                backend_ms[field] = first_chunk[field]
        entries.append({
            'time': clock, 'request': request, 'npc': npc, 'world_epoch': row.get('world_epoch'),
            'owner_request': companion.get('requester_is_owner'),
            'chosen_action': (row.get('chosen') or '').partition(':')[0] or None,
            'chosen_candidate_id': row.get('chosen_candidate_id'),
            'chosen_travel_mode': row.get('chosen_travel_mode'),
            'chosen_resolution': row.get('chosen_resolution'),
            'server_decision': [{'accepted': accepted, 'reason': why} for accepted, why in sorted(outcomes)],
            'editor_stream_approved': 'approved' in markers,
            'editor_ms_after_http_start': elapsed,
            'backend_ms': backend_ms,
            'observed_before_decision': {
                'travel_mode': companion.get('travel_mode'),
                'command_revision': companion.get('command_revision'),
                'has_package': companion.get('has_package'),
                'package_outcome': reason if reason in PACKAGE_RESULTS else None,
                'errand_action': companion.get('errand_action'),
                'errand_phase': companion.get('errand_phase'),
                'errand_status': companion.get('errand_status'),
                'errand_reason': reason,
                'suspension_reason': companion.get('suspension_reason'),
                'vehicle_state': companion.get('vehicle_state'),
                'owner_vehicle_state': companion.get('owner_vehicle_state'),
                'task_door_state': companion.get('task_door_state'),
                'assist_revision': companion.get('assist_revision'),
                'assist_active': companion.get('assist_active'),
                'assist_pending': companion.get('assist_pending'),
            },
        })

    motion_samples = []
    for (world_start, actor, revision, mode), samples in follow_positions.items():
        actor_first, owner_first = samples[0]
        actor_last, owner_last = samples[-1]
        motion_samples.append({
            'editor_world_start': world_start, 'actor': actor, 'revision': revision,
            'travel_mode': mode, 'sample_count': len(samples),
            'npc_net_displacement_cm': round(dist(actor_first[:2], actor_last[:2]), 1) if len(samples) > 1 else None,
            'owner_net_displacement_cm': round(dist(owner_first[:2], owner_last[:2]), 1) if len(samples) > 1 else None,
            'last_horizontal_separation_cm': round(dist(actor_last[:2], owner_last[:2]), 1),
        })
    return {
        'companion_turns': entries, 'invalid_dialogue_rows': skipped_json,
        'owner_backend_latency_ms': latency_summary(entries),
        'editor_follow_requests_all_worlds': [
            {'travel_mode': mode, 'move_request_result': result, 'count': count}
            for (mode, result), count in sorted(follow_requests.items())
        ],
        'editor_follow_position_samples_all_worlds': motion_samples,
        'editor_gameplay_events_all_worlds': gameplay,
        'editor_obstacle_jump_attempts_all_worlds': jump_attempts(gameplay),
        'editor_test_markers': sorted(test_markers),
        'limits': [
            'Decision acceptance is proposal acceptance. A MoveTo result is a path request result.',
            'Observed fields describe the snapshot before that turn. Mount and dismount entries require explicit server events.',
            'Position samples are grouped by editor world startup, actor and command revision. They measure displacement between navigation submissions, not route arrival.',
            'Editor log intervals exclude audio output and can include queue or transport time.',
            'Backend intervals share a job origin. First audio chunk is emitted by the server, before device playback. Each percentile uses only turns with that marker.',
            'Editor follow request counts cover the entire supplied editor log. Local start time assumes the same day.',
            'Task completion is a server event for the task. Without a companion delivery result it is not attributed to the companion.',
            'Grandpa receipt confirms the real package reached the recipient. Wallet payout confirms server credit. These events do not establish HUD display or identify the package carrier.',
            'Door hits and opening require explicit server logs. Older builds without those messages cannot establish whether the player swung at the door.',
            'Jump start records an accepted launch, not actual airtime. A matched landing uses the same editor world startup, actor and move request. Landing is not task completion or proof that the resumed path reached its goal.',
            'A jump without an observed landing may be interrupted, still running or outside the captured interval; it is not classified as a failed jump.',
            'Ground placement records release of the real package at a checked point. It does not prove stable physical rest, player pickup, delivery or reward.',
            'Action results belong to their logged action IDs. They are not joined to model requests by timing or plan revision. CompanionErrandAccepted confirms command registration, not delivery.',
            'Joined reply played requires the explicit playback receipt. Waiting for the reply and accepting a model action do not count as playback.',
            'Test markers describe the supplied editor log. Their absence does not establish human input. world_start is the most recent PIE startup line, not an authority world ID.',
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--editor', required=True, type=Path)
    parser.add_argument('--dialogue', required=True, type=Path)
    parser.add_argument('--backend', action='append', type=Path, default=[])
    scope = parser.add_mutually_exclusive_group(required=True)
    scope.add_argument('--epoch', type=UUID)
    scope.add_argument('--after-local', type=time.fromisoformat)
    args = parser.parse_args()
    report = audit(capture(args.editor), capture(args.dialogue),
                   args.epoch.hex if args.epoch else None,
                   args.after_local.isoformat(timespec='seconds') if args.after_local else None,
                   (row for path in args.backend for row in capture(path)))
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
