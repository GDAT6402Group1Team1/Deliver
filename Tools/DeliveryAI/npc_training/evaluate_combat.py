"""通过正式适配器评测独立战斗模型，标签只用于汇总，不进入模型输入"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import statistics
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from combat_model import ACTION_TOKEN_LIMIT, LocalCombatClient, prepare_combat_context
from build_combat_data import write_json


def percentile(values, fraction):
    return sorted(values)[max(0, math.ceil(len(values)*fraction)-1)] if values else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--teacher', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--split', choices=['train', 'validation', 'acceptance_only', 'all'], default='all')
    parser.add_argument('--endpoint', default='http://127.0.0.1:8771')
    parser.add_argument('--timeout', type=float, default=10.)
    parser.add_argument('--limit', type=int)
    parser.add_argument('--repeat', type=int, default=1)
    parser.add_argument('--seed', type=int, default=20261114)
    parser.add_argument('--temperature', type=float, default=0.)
    parser.add_argument('--compact-output', action='store_true', help='只生成动作名，用原候选表查目标')
    parser.add_argument('--run', action='store_true')
    args = parser.parse_args()
    rows = json.loads(args.source.read_text(encoding='utf-8'))
    if isinstance(rows, dict):
        rows = rows['cases']
    if args.split != 'all':
        rows = [r for r in rows if r['usage'] == args.split]
    # 同一份固定顺序用于量化和层数对照；不按模型结果重排情境
    random.Random(20261114).shuffle(rows)
    if args.limit:
        rows = rows[:args.limit]
    teacher = {r['id']: r for r in json.loads(args.teacher.read_text(encoding='utf-8'))['results']} if args.teacher else {}
    if args.output.exists():
        raise FileExistsError(args.output)
    report = {'source': str(args.source), 'source_sha256': hashlib.sha256(args.source.read_bytes()).hexdigest(),
              'input_preparation_sha256': hashlib.sha256(
                  (Path(__file__).resolve().parents[1] / 'combat_model.py').read_bytes()).hexdigest(),
              'split': args.split, 'samples': len(rows), 'timeout_seconds': args.timeout, 'results': [],
              'status': 'prepared', 'context_tokens': 4096,
              'max_output_tokens': ACTION_TOKEN_LIMIT if args.compact_output else 64,
              'output_mode': 'action_name' if args.compact_output else 'json',
              'sampling': {'temperature': args.temperature, 'top_p': .8, 'top_k': 20, 'min_p': 0, 'seed': args.seed},
              'repeated_identical_prompt_is_separate_metric': True,
              'scoring_definition': 'v2: reasonable action >=90%, 40+ cases, all outputs and targets legal; technical cancellation checks are separate',
              'scoring_changed_before_round2_inference': True}
    running_path = Path(__file__).resolve().parents[3] / 'Saved/AIResearch/LocalNpcModel/qwen3-1.7b/running.json'
    if running_path.is_file():
        running = json.loads(running_path.read_text(encoding='utf-8'))
        if running.get('health') == args.endpoint.rstrip('/') + '/health':
            report['model_runtime'] = running
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_json(args.output, report)
    if not args.run:
        print(json.dumps({key: report[key] for key in ('status', 'samples', 'split')}))
        return
    client = LocalCombatClient(args.endpoint, args.timeout, seed=args.seed, temperature=args.temperature,
                               compact_output=args.compact_output)
    for row in rows:
        prompt, candidates = prepare_combat_context(row['context'])
        expected = row.get('label') or {}
        acceptable = expected.get('acceptable_actions', [])
        if not acceptable and row['id'] in teacher:
            acceptable = [teacher[row['id']]['action']]
        for repeat in range(args.repeat):
            timing = {}
            result = {'id': row['id'], 'group_id': row['group_id'], 'source': row.get('source'),
                      'prepared_prompt_sha256': hashlib.sha256(json.dumps(
                          prompt, ensure_ascii=False, separators=(',', ':')).encode('utf-8')).hexdigest(),
                      'repeat': repeat, 'acceptable_actions': acceptable, 'correct': False,
                      'protocol_valid': False, 'target_valid': False}
            start = time.monotonic()
            try:
                action = client.choose(prompt, candidates, threading.Event(),
                                       lambda stage, ms: timing.update({stage: ms}))
                result.update(action=action, correct=action in acceptable, protocol_valid=True, target_valid=True,
                              output=client.last_output, model_usage=client.last_usage)
            except Exception as error:
                result.update(error=type(error).__name__ + ':' + str(error), output=client.last_output)
                result['decision_ms_before_cleanup'] = (time.monotonic()-start)*1000
                if client.requires_drain:
                    # 评测也确认前一请求结束，不能把积压时间算到下一情境
                    wait_until = time.monotonic()+5
                    while time.monotonic() < wait_until and not client.is_idle():
                        time.sleep(.1)
                    if not client.is_idle():
                        raise RuntimeError('local_model_did_not_become_idle')
                    client.requires_drain = False
            result.update(complete_ms=(time.monotonic()-start)*1000, timing=timing)
            report['results'].append(result)
            write_json(args.output, report)
            print(json.dumps({key: result.get(key) for key in ('id', 'repeat', 'action', 'correct', 'complete_ms', 'error')}, ensure_ascii=False), flush=True)
    first = [r for r in report['results'] if r['repeat'] == 0]
    repeats = [r for r in report['results'] if r['repeat'] > 0]
    report.update(status='completed', summary={
        'cases': len(first), 'correct': sum(r['correct'] for r in first),
        'protocol_valid': sum(r['protocol_valid'] for r in first),
        'target_valid': sum(r['target_valid'] for r in first),
        'accuracy': sum(r['correct'] for r in first)/len(first),
        'first_pass_ms_median': statistics.median(r['complete_ms'] for r in first),
        'first_pass_ms_p95': percentile([r['complete_ms'] for r in first], .95),
        'exact_repeat_ms_p95': percentile([r['complete_ms'] for r in repeats], .95),
        'unwanted_fight': sum(r.get('action') == 'Fight' and acceptable and 'Fight' not in acceptable
                             for r in first if (acceptable := r['acceptable_actions'])),
        'acceptance_qualified': args.split == 'acceptance_only' and len(first) >= 40
                               and sum(r['correct'] for r in first)/len(first) >= .9
                               and all(r['protocol_valid'] and r['target_valid'] for r in first),
        'integration_checks_required': True})
    write_json(args.output, report)
    print(json.dumps(report['summary']))


if __name__ == '__main__':
    main()
