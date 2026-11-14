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
NUMBER = r'([-+\d.eE]+)'
POSITION = rf'V\(X={NUMBER}, Y={NUMBER}, Z={NUMBER}\)'
FOLLOW_POSITIONS = re.compile(rf'from={POSITION}.*?owner={POSITION}')
LATENCY = re.compile(r'^\[([\d.:-]+)\].*NpcLatency request=(\w+) stage=(http_start|approved|model_done)\b')
PACKAGE_RESULTS = {'PackageReceived', 'PackageReturned', 'Delivered'}
SUSPENSION = re.compile(r'CompanionSuspension npc=(\S+) revision=(\d+) mode=(\d+) reason=(\S+)')
TASK_COMPLETE = re.compile(r'\[Task\] (\S+) 完成，用时 ([\d.]+)s，奖励 (\d+)')
BACKEND_MARK = re.compile(r'\blatency request=(\S+) stage=(\w+) ([^\r\n]+)')
BACKEND_FIELDS = re.compile(r'\b(elapsed_ms|tts_queue_ms|tts_ms|sentence)=(\d+)\b')


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
    for row in editor_rows:
        line = row.decode('utf-8', errors='replace')
        if 'Bringing World' in line and 'UEDPIE' in line:
            world_start = line.partition(']')[0].lstrip('[')
        if match := SUSPENSION.search(line):
            gameplay.append({'world_start': world_start, 'kind': 'companion_suspension',
                             'actor': match[1], 'revision': int(match[2]), 'reason': match[4]})
        if match := TASK_COMPLETE.search(line):
            gameplay.append({'world_start': world_start, 'kind': 'task_completed',
                             'task': match[1], 'elapsed_seconds': float(match[2]), 'reward': int(match[3])})
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
                try:
                    coords = tuple(map(float, positions.groups()))
                except ValueError:
                    continue
                if all(isfinite(value) for value in coords):
                    key = (world_start, match[1], int(match[2]), match[4])
                    samples = follow_positions.setdefault(key, [])
                    samples.append((coords[:3], coords[3:]))
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
        'limits': [
            'Decision acceptance is proposal acceptance. A MoveTo result is a path request result.',
            'Observed fields describe the snapshot before that turn. Physical mount and delivery are not inferred.',
            'Position samples are grouped by editor world startup, actor and command revision. They measure displacement between navigation submissions, not route arrival.',
            'Editor log intervals exclude audio output and can include queue or transport time.',
            'Backend intervals share a job origin. First audio chunk is emitted by the server, before device playback. Each percentile uses only turns with that marker.',
            'Editor follow request counts cover the entire supplied editor log. Local start time assumes the same day.',
            'Task completion is a server event for the task. Without a companion delivery result it is not attributed to the companion.',
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
