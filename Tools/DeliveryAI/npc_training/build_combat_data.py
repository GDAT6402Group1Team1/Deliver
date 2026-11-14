"""准备交战训练与独立验收；保留原始情境、日志出处和教师修改记录"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import random
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from combat_model import prepare_combat_context
from combat_training_stories import training_stories

ROOT = Path(__file__).resolve().parents[3]
DEFAULT = ROOT / 'Saved/AIResearch/NpcTraining/combat-v4'
PERSONAS = (
    ('大壮', '翠花', '你是退休街坊大壮，爱面子、嘴硬、脾气急，被当面嘲笑会想还手。实际吃疼会害怕，也愿意顺着道歉停手；不因旁人起哄就无限追打。'),
    ('翠花', '大壮', '你是退休街坊翠花，性子急，不愿被人压一头，敢当面争辩也会还手。怕疼，明显吃亏会跑；对方认真认错时可以收手，不必每次争到底。'))


def stable_id(value):
    return str(uuid.uuid5(uuid.NAMESPACE_URL, 'delivery-combat-v1/' + value))


def case_context(identifier, current_action, index):
    own, target = stable_id(identifier + '/self'), stable_id(identifier + '/opponent')
    event_id = stable_id(identifier + '/event')
    own_name, other_name, persona = PERSONAS[index % 2]
    candidates = [{'action': a, 'target_id': '' if a == 'Wait' else target,
                   'duration': {'Wait': 2., 'Fight': 8., 'Flee': 5.}[a],
                   'evidence_ids': [] if a == 'Wait' else [event_id]} for a in ('Wait', 'Fight', 'Flee')]
    random.Random(identifier).shuffle(candidates)
    return {'protocol_version': 1, 'capability_version': 1,
            'world_epoch': stable_id('world'), 'npc_id': own, 'request_id': stable_id(identifier + '/request'),
            'context_revision': 1, 'plan_revision': 1, 'request_source': 'ambient',
            'pending_player_message': False, 'persona': persona, 'self_name': own_name,
            'self_subject_id': own, 'profile_id': '', 'allowed_actions': ['Wait', 'Fight', 'Flee'],
            'subjects': [{'subject_id': own, 'name': own_name, 'kind': 'npc'},
                         {'subject_id': target, 'name': other_name, 'kind': 'npc'}],
            'evidence': [],
            'candidates': candidates, 'current_action': {'action': current_action, 'target_id': target,
                'source': 'Model', 'remaining_seconds': 3., 'action_id': stable_id(identifier + '/action')}}


def structured_training_cases(log_file):
    profiles = {}
    for line in log_file.read_text(encoding='utf-8').splitlines():
        entry = json.loads(line)
        name = entry.get('model_context', {}).get('self_name')
        if name in ('大壮', '翠花') and entry.get('full_persona'):
            profiles[name] = entry['full_persona']
    if set(profiles) != {'大壮', '翠花'}:
        raise ValueError('缺少正式人物完整性格')
    result = []
    for index, story in enumerate(training_stories()):
        identifier = story['id']
        context = case_context(identifier, story['current_action'], index)
        own = context['self_subject_id']
        target = context['current_action']['target_id']
        context['persona'] = profiles[context['self_name']]
        other_name = context['subjects'][1]['name']
        events = []
        def add(kind, subject, text, source, addressee=''):
            events.append({'event_id': stable_id(identifier + '/observed/' + str(len(events))),
                           'kind': kind, 'subject_id': subject, 'addressee_id': addressee,
                           'description': text, 'source': source, 'reply_to_event_id': ''})
        add('Perceived', target, f"看见附近的{other_name}，当时距离约 {story['distance']} 厘米", 'Seen')
        add('SelfSpeech', own, story['self_speech'], 'ActionResult', target)
        add('Speech', target, story['speech'], 'Heard', own)
        if story['last_event'] == 'FightThreat':
            add('FightThreat', target, '这个人朝我冲过来，准备动手', 'Seen')
        elif story['last_event'] == 'LightMeleeHit':
            add('LightMeleeHit', target, '这个人打了我一拳，我后仰了一下，仍然站着', 'Seen')
        elif story['last_event'] == 'LostSight':
            add('LostSight', target, f'暂时看不见{other_name}', 'Seen')
        body = '刚挨打，正在后仰' if story['last_event'] == 'LightMeleeHit' else '站着，可以行动'
        current = '本轮' if story['current_action'] == 'Fight' else '上一轮'
        add('SelfCombat', own, f"这阵子实际挨了 {story['received']} 记轻拳；{current}交战挥拳 {story['punches']} 次，实际命中 {story['hits']} 次。现在{body}。", 'ActionResult')
        context['evidence'] = events
        for candidate in context['candidates']:
            candidate['evidence_ids'] = [] if candidate['action'] == 'Wait' else [events[0]['event_id']]
        prompt, _ = prepare_combat_context(context)
        usage = 'validation' if index < 120 and (index + 1) % 8 == 0 else 'train'
        result.append({'id': identifier, 'group_id': identifier, 'usage': usage,
                       'source': {'kind': 'authored_ue_event_scenario', 'gameplay_recording': False,
                                  'persona_source': str(log_file.relative_to(ROOT)),
                                  'event_formats': 'DescribeCombatState / ObserveFact / Speech'},
                       'context': context, 'prompt': prompt, 'label': None})
    return result


def log_examples(path):
    """日志保留真实对话；旧日志没有当前行动字段，补入部分明确标为人工假设"""
    rows = [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines() if line.strip()]
    selected = []
    for index, row in enumerate(rows):
        old = row.get('model_context', {})
        combat = [e for e in old.get('evidence', []) if e.get('kind') == 'SelfCombat']
        if not combat or not str(row.get('chosen', '')).startswith(('Fight:', 'Flee:')):
            continue
        if not any(char in combat[-1].get('description', '') for char in '123456789'):
            continue
        selected.append((index, row))
    result = []
    # 同次试玩中的不同轮次留在同一组，不把连续对话拆到训练与验证两边
    for index, row in selected[-8:]:
        original = row['model_context']
        action, target = row['chosen'].split(':', 1)
        identifier = 'log-' + row['request_id']
        aliases = {p['subject_id']: stable_id(identifier + '/' + p['subject_id']) for p in original['subjects']}
        old_events = original.get('evidence', [])
        evidence = []
        for n, event in enumerate(old_events):
            evidence.append({**event, 'event_id': stable_id(identifier + '/event/' + str(n)),
                             'subject_id': aliases.get(event.get('subject_id'), ''),
                             'addressee_id': aliases.get(event.get('addressee_id'), ''), 'reply_to_event_id': ''})
        target_events = [e['event_id'] for e in evidence if e['subject_id'] == aliases.get(target)]
        if not target_events:
            continue
        context = case_context(identifier, action, index)
        context.update(persona=row.get('full_persona', original.get('persona', '')),
                       self_name=original.get('self_name', ''), self_subject_id=aliases[original['self_subject_id']],
                       subjects=[{**p, 'subject_id': aliases[p['subject_id']]} for p in original['subjects']], evidence=evidence)
        context['current_action']['target_id'] = aliases[target]
        context['candidates'] = [{'action': a, 'target_id': '' if a == 'Wait' else aliases[target],
                                 'duration': {'Fight': 8., 'Flee': 5., 'Wait': 2.}[a],
                                 'evidence_ids': [] if a == 'Wait' else target_events[-1:]} for a in ('Wait', 'Fight', 'Flee')]
        prompt, _ = prepare_combat_context(context)
        result.append({'id': identifier, 'group_id': 'log-session-' + row['time'].split(':')[0], 'usage': 'train',
                       'source': {'kind': 'game_log_with_reconstructed_running_action',
                                  'file': str(path.relative_to(ROOT)), 'line': index+1, 'request_id': row['request_id'],
                                  'original_sha256': hashlib.sha256(json.dumps(row, ensure_ascii=False, sort_keys=True).encode()).hexdigest(),
                                  'assumption': '把该轮已选择的 Fight/Flee 设为正在执行，日志没有实际持续状态，不能据此声称是完整真实轨迹'},
                       'context': context, 'prompt': prompt, 'label': None, 'original_log': row})
    return result


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=DEFAULT)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    training = structured_training_cases(ROOT / 'Saved/Logs/NpcDialogue.ndjson')
    assert len(training) == 135
    assert sum(r['usage'] == 'train' for r in training) == 120
    assert len({json.dumps(r['prompt']['events'], ensure_ascii=False) for r in training}) == 135
    logs = log_examples(ROOT / 'Saved/Logs/NpcDialogue.ndjson')
    write_json(args.output / 'situations.json', training + logs)
    acceptance_file = ROOT / 'Saved/AIResearch/NpcTraining/acceptance-reviewed.json'
    manifest = {'authored_training_situations': 120, 'internal_validation_situations': 15, 'acceptance_situations': 40,
                'log_derived_samples': len(logs), 'log_episode_groups': len({r['group_id'] for r in logs}),
                'usage_counts': dict(Counter(r['usage'] for r in training + logs)),
                'sources': dict(Counter(r['source']['kind'] for r in training + logs)),
                'teacher_reviewed': False, 'student_trajectory_corrections': 0,
                'note': '人工情境用于覆盖独立决策变化；日志补充不算作纯真实持续动作轨迹，也不声称完成 DAgger',
                'acceptance_file': str(acceptance_file),
                'acceptance_sha256': hashlib.sha256(acceptance_file.read_bytes()).hexdigest()}
    write_json(args.output / 'manifest.json', manifest)
    print(json.dumps(manifest, ensure_ascii=False))


if __name__ == '__main__':
    main()
