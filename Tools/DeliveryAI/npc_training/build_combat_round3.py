"""为第三轮准备独立故事和正式战况格式；只写新目录，不改原数据与验收题"""
from collections import Counter
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from combat_model import prepare_combat_context
from build_combat_data import ROOT, stable_id, case_context, write_json
from combat_round3_stories import stories

FROZEN_SHA = '77706e89104e4ec6d12704cb5b2214ff8efa9e4117cded6e8c400adf3acacced'
PREVIOUS = ROOT / 'Saved/AIResearch/NpcTraining/combat-v5'
OUTPUT = ROOT / 'Saved/AIResearch/NpcTraining/combat-v7'


def digest(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True).encode('utf-8')).hexdigest()


def running_suffix(context):
    current = context['current_action']
    target_name = next(person['name'] for person in context['subjects'] if person['subject_id'] == current['target_id'])
    return (f"；我正在对{target_name}执行{current['action']}，剩余约{current['remaining_seconds']:.1f}秒。"
            '当前动作还未结束；下一步可以继续、逃跑或停手')


def make_case(story, branch_index, profiles):
    identifier = story['id'] + '-branch-' + str(branch_index + 1)
    context = case_context(story['id'], story['current_action'], 0 if story['role'] == '大壮' else 1)
    context['request_id'] = stable_id(identifier + '/request')
    context['persona'] = profiles[story['role']]
    context['profile_id'] = 'chunlai' if story['role'] == '大壮' else 'xiumei'
    own, target = context['self_subject_id'], context['current_action']['target_id']
    bystander = stable_id(story['id'] + '/bystander')
    actors = {'self': own, 'other': target, 'bystander': bystander}
    action, ending = story['branches'][branch_index]
    timeline = story['opening'] + ending
    if any(actor == 'bystander' for actor, _, _ in timeline):
        context['subjects'].append({'subject_id': bystander, 'name': '邻居老周', 'kind': 'npc'})
    events = []
    def add(actor, kind, text):
        events.append({'event_id': stable_id(identifier + '/event/' + str(len(events))),
                       'subject_id': actors[actor], 'kind': kind, 'description': text,
                       'source': 'ActionResult' if actor == 'self' else 'Heard' if 'Speech' in kind else 'Seen',
                       'addressee_id': target if kind == 'SelfSpeech' else own if kind == 'Speech' else '',
                       'reply_to_event_id': ''})
    add('other', 'Perceived', f"看见附近的{context['subjects'][1]['name']}，当时距离约 200 厘米")
    for actor, kind, text in timeline:
        add(actor, kind, text)
    received = story['received'] + sum(kind == 'LightMeleeHit' for _, kind, _ in timeline)
    assert 0 <= received <= 3 and 0 <= story['hits'] <= story['punches']
    body = '刚挨打，正在后仰' if timeline[-1][1] == 'LightMeleeHit' else '站着，可以行动'
    phase = '本轮' if story['current_action'] == 'Fight' else '上一轮'
    add('self', 'SelfCombat', f"这阵子实际挨了 {received} 记轻拳；{phase}交战挥拳 {story['punches']} 次，实际命中 {story['hits']} 次。现在{body}。" + running_suffix(context))
    context['evidence'] = events
    for candidate in context['candidates']:
        candidate['evidence_ids'] = [] if candidate['action'] == 'Wait' else [events[0]['event_id']]
    prompt, _ = prepare_combat_context(context)
    return {'id': identifier, 'group_id': story['id'], 'usage': story['usage'],
            'source': {'kind': 'authored_round3_state_change', 'gameplay_recording': False,
                       'topic': story['topic'], 'branch': branch_index + 1,
                       'basis': 'general state-change failure categories; not a rewritten acceptance case',
                       'branches_are_alternatives_not_executed_trajectories': True},
            'context': context, 'prompt': prompt, 'label': {'acceptable_actions': [action],
                'status': 'author_draft_pending_independent_teacher_review'}}


def main():
    frozen = ROOT / 'Saved/AIResearch/NpcTraining/acceptance-reviewed.json'
    assert hashlib.sha256(frozen.read_bytes()).hexdigest() == FROZEN_SHA
    old = json.loads((PREVIOUS / 'situations.json').read_text(encoding='utf-8'))
    profiles = {row['context']['self_name']: row['context']['persona'] for row in old
                if row['context']['self_name'] in ('大壮', '翠花')}
    rows, conversions = deepcopy(old), []
    # 只更新回放训练的副本；旧内部检查保持原输入，单独报告结果
    for row in rows:
        if row['usage'] != 'train':
            continue
        before = digest(row['prompt'])
        bodies = [event for event in row['context']['evidence'] if event['kind'] == 'SelfCombat']
        assert len(bodies) == 1 and '我正在对' not in bodies[0]['description']
        old_description = bodies[0]['description']
        bodies[0]['description'] += running_suffix(row['context'])
        row['prompt'], _ = prepare_combat_context(row['context'])
        assert digest(row['prompt']) == before
        conversions.append({'id': row['id'], 'before_prompt_sha256': before,
                            'after_prompt_sha256': digest(row['prompt']),
                            'before_self_combat': old_description, 'after_self_combat': bodies[0]['description']})
    authored = stories()
    assert len(authored) == 40 and len({s['id'] for s in authored}) == 40
    new = [make_case(story, index, profiles) for story in authored for index in range(2)]
    rows += new
    train = [r for r in rows if r['usage'] == 'train']
    validation_old = [r for r in old if r['usage'] == 'validation']
    validation_new = [r for r in new if r['usage'] == 'validation']
    assert len(train) == 224 and len(validation_old) == 15 and len(validation_new) == 16
    assert not {r['group_id'] for r in train} & {r['group_id'] for r in validation_old + validation_new}
    coverage = Counter((r['context']['self_name'], r['context']['current_action']['action']) for r in validation_new)
    assert set(coverage.values()) == {4} and len(coverage) == 4
    OUTPUT.mkdir(parents=True, exist_ok=False)
    write_json(OUTPUT / 'situations.json', rows)
    write_json(OUTPUT / 'validation-legacy.json', validation_old)
    write_json(OUTPUT / 'validation-new.json', validation_new)
    write_json(OUTPUT / 'format-conversions.json', conversions)
    teacher = json.loads((PREVIOUS / 'teacher.json').read_text(encoding='utf-8'))
    # 旧标签沿用，但保留输入格式已变化的事实；新故事仍逐例由教师审核
    teacher['reused_teacher_source'] = str(PREVIOUS / 'teacher.json')
    teacher['reused_teacher_sha256'] = hashlib.sha256((PREVIOUS / 'teacher.json').read_bytes()).hexdigest()
    teacher['situations_sha256'] = hashlib.sha256((OUTPUT / 'situations.json').read_bytes()).hexdigest()
    teacher['previous_round_calls'] = teacher.pop('calls')
    teacher['calls'] = []
    teacher['round3_replay_format_changes'] = '160 raw SelfCombat suffixes use neutral current-action wording; shared preprocessing removes redundant status; model inputs and labels unchanged from v5; see format-conversions.json'
    write_json(OUTPUT / 'teacher.json', teacher)
    plan = {'round': 3, 'authorization': 'new user instruction after two-round failure; earlier results retained',
            'status': 'data_prepared_teacher_pending_gpu_not_started', 'frozen_acceptance_sha256': FROZEN_SHA,
            'acceptance_content_read_by_generator': False, 'acceptance_labels_changed': False,
            'new_stories': 40, 'new_training_groups': 32, 'new_validation_groups': 8,
            'new_training_rows': 64, 'new_validation_rows': 16, 'replay_training_rows': 160,
            'total_training_rows': 224, 'total_training_groups': len({r['group_id'] for r in train}),
            'legacy_validation_rows': 15, 'validation_scored_separately': True,
            'new_validation_coverage': {str(k): v for k,v in coverage.items()},
            'new_draft_actions': dict(Counter(r['label']['acceptable_actions'][0] for r in new)),
            'role_change_coverage': dict(Counter(('continue' if r['label']['acceptable_actions'][0] == r['context']['current_action']['action'] else 'change') for r in validation_new)),
            'replay_input_change': 'raw context uses neutral UE status suffix; shared prepare_combat_context removes only known redundant SelfCombat suffix, keeping real hit counts and independent current_action; original files and labels retained',
            'input_preprocessing_version': 'self_combat_actual_facts_v2',
            'input_preprocessing_change': 'all production/training/evaluation inputs use the same suffix removal; raw frozen40 and labels unchanged; not comparable as identical prompts to pre-fix reports',
            'rejected_previous_preparation': {'folder': 'Saved/AIResearch/NpcTraining/combat-v6', 'reason': 'runtime suffix asserted ongoing punching and hits even while fleeing or after agreement; teacher errors confirmed reliance on this false fact', 'training_started': False},
            'source_cpp': ['Source/Delivery/AI/Actions/DeliveryNpcCombatComponent.cpp:217', 'Source/Delivery/AI/Decision/DeliveryNpcAgentComponent.cpp:217'],
            'fixed_training': {'initial_adapter': 'Saved/AIResearch/NpcTraining/combat-v5/round2/adapter',
                'learning_rate': .00002, 'optimizer_steps': 56, 'gradient_accumulation': 4,
                'micro_batches': 224, 'epochs': 1, 'max_seq_length': 4096, 'max_output_tokens': 64,
                'seed': 20261114, 'selection': 'one predeclared candidate; no acceptance-based parameter search'},
            'why_not_use_loss_as_success': ['13-token answer contains mostly fixed JSON text; low average token loss is not action accuracy',
                'old 15 validation cases all used one character, while all five final errors continued current action',
                'new branch pairs keep body counters fixed where possible while later observable facts change'],
            'validation_rule': 'report legacy15 and new16 separately, with role/current-action/continue-change breakdown; independent review before final40',
            'final_rule_unchanged': {'reasonable_action_minimum': .9, 'cases': 40, 'protocol_target_valid_required': True, 'critical_technical_errors': 0},
            'previous_data_sha256': hashlib.sha256((PREVIOUS / 'situations.json').read_bytes()).hexdigest(),
            'new_data_sha256': hashlib.sha256((OUTPUT / 'situations.json').read_bytes()).hexdigest()}
    write_json(OUTPUT / 'experiment-plan.json', plan)
    assert hashlib.sha256(frozen.read_bytes()).hexdigest() == FROZEN_SHA
    print(json.dumps({'output': str(OUTPUT), 'training_rows': len(train), 'new_validation_rows': len(validation_new),
                      'draft_actions': plan['new_draft_actions'], 'validation_coverage': plan['new_validation_coverage']}, ensure_ascii=False))


if __name__ == '__main__':
    main()
