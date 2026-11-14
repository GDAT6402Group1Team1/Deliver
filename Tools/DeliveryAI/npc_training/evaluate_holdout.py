"""准备独立验收输入并汇总模型结果；验收标签不进入模型提示或训练数据"""
import argparse
import hashlib
import json
from pathlib import Path
import statistics

from build_seed_data import TRAINING_SYSTEM, compact_context


ROOT = Path(__file__).resolve().parents[3]
EXPERIMENT = ROOT / 'Saved/AIResearch/NpcTraining'


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def prepare(source):
    original = read_json(source)
    if original.get('usage') != 'eval_only':
        raise ValueError('只接受明确标记 eval_only 的独立验收集')
    destination = EXPERIMENT / 'data/eval.jsonl'
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        raise FileExistsError(destination)
    rows = []
    for case in original['cases']:
        if case['usage'] != 'eval_only':
            raise ValueError('验收样本缺少 eval_only 标记')
        rows.append({'id': case['case_id'], 'group_id': case['scenario_group_id'],
                     'source': case['source'], 'usage': 'eval_only', 'messages': [
                         {'role': 'system', 'content': TRAINING_SYSTEM},
                         {'role': 'user', 'content': json.dumps(compact_context(case['context']),
                                                               ensure_ascii=False, separators=(',', ':'))}]})
    destination.write_text(''.join(json.dumps(row, ensure_ascii=False) + '\n' for row in rows), encoding='utf-8')
    return {'path': str(destination), 'samples': len(rows), 'labels_in_prompt': False,
            'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
            'input_sha256': hashlib.sha256(destination.read_bytes()).hexdigest()}


def summarize(source, results):
    cases = {case['case_id']: case for case in read_json(source)['cases']}
    outputs = [json.loads(line) for line in results.read_text(encoding='utf-8-sig').splitlines() if line.strip()]
    if len({row['id'] for row in outputs}) != len(outputs) or set(cases) != {row['id'] for row in outputs}:
        raise ValueError('结果缺少验收场景、包含额外场景或有重复编号')
    details = []
    for row in outputs:
        case = cases[row['id']]
        label = case['label']
        try:
            reply = json.loads(row['output'])
        except (ValueError, TypeError):
            reply = {}
        selected = row['checks'].get('selected_candidate', {})
        action_target = {key: selected.get(key) for key in ('action', 'target_id')}
        details.append({'id': row['id'], 'profile_id': case['profile_id'], 'output': row['output'],
                        'selected_action': action_target, 'format_valid': row['checks']['valid'],
                        'allowed_action_target': row['checks']['valid'] and action_target in label['acceptable_action_targets'],
                        'allowed_speech_target': row['checks']['valid'] and reply.get('speech_target_id') in label['acceptable_speech_targets'],
                        'empty_speech_allowed': label['allow_empty_speech'] or bool(reply.get('speech', '').strip()),
                        'first_token_ms': row['first_token_ms'], 'complete_ms': row['complete_ms'],
                        'manual_review_required': {'must_not': label['must_not'], 'must_not_claim': label['must_not_claim'],
                                                   'naturalness': label.get('naturalness', '')}})
    summary = {'samples': len(details), 'semantic_quality_scored': False,
               'note': '动作和收话人检查仅用于定位；自然口语、事实、人物一致性仍需逐条阅读，不能当作游戏验收',
               'format_valid': sum(row['format_valid'] for row in details),
               'allowed_action_target': sum(row['allowed_action_target'] for row in details),
               'allowed_speech_target': sum(row['allowed_speech_target'] for row in details),
               'complete_ms_median': statistics.median(row['complete_ms'] for row in details),
               'first_token_ms_median': statistics.median(row['first_token_ms'] for row in details if row['first_token_ms'] is not None),
               'details': details}
    destination = results.with_name('holdout_review.json')
    destination.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding='utf-8')
    return {'path': str(destination), **{key: value for key, value in summary.items() if key != 'details'}}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=EXPERIMENT / 'heldout_cases.json')
    parser.add_argument('--results', type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.source, args.results) if args.results else prepare(args.source), ensure_ascii=False))
