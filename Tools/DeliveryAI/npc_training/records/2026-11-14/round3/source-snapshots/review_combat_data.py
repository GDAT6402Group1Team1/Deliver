"""用教师核对经过和动作，保留每次请求与分歧，不把验收标签交给教师训练"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from app_server import AppServer
from combat_model import COMBAT_INSTRUCTIONS
from build_combat_data import DEFAULT, write_json

TEACHER = '''审核送快递游戏的持续交战小模型训练输入。每个 case 是独立场景，不能互相串联。
模型只根据 persona、已有事件和 current_action 选择 Fight（继续追当前对手并出拳）、Flee（远离当前对手）或 Wait（停手）。不靠对话次数决定，不能因为性格暴躁永远攻击，也不能忽略交战尚未结束的事实。自己的疼痛、实际受击、对方是否继续挥拳、道歉、认输和安全距离都要考虑。无需台词。
先检查输入有没有把尚未作出的决定作为事实：例如“自己决定停手/准备逃跑/应该继续打”。如果有，input_ok=false并指出那句话。已实际放下拳头、已听见道歉、身体疼痛属于事实。不要猜测事件未写出的下一步意图。
FightThreat 的“这个人朝我冲过来，准备动手”是游戏原生的感知事件，描述对手已经开始的追打，可正常使用；SelfSpeech 是自己此前已经说出口的话，也不是本轮的新决定。
返回JSON数组，顺序和id保持输入一致。每项仅有 id、input_ok（bool）、action（三种之一）、target_id（Fight/Flee为opponent，Wait为空字符串）、reason（不超过35字的具体依据）。不要markdown，不要复述场景，不修改输入，不输出分析过程。'''


def parse_output(text, batch):
    values = json.loads(text.strip())
    if not isinstance(values, list) or [v.get('id') for v in values] != [v['id'] for v in batch]:
        raise ValueError('teacher_case_ids_changed')
    for value in values:
        if (set(value) != {'id', 'input_ok', 'action', 'target_id', 'reason'} or type(value['input_ok']) is not bool
                or value['action'] not in ('Fight', 'Flee', 'Wait') or not isinstance(value['reason'], str)
                or value['target_id'] != ('' if value['action'] == 'Wait' else 'opponent')):
            raise ValueError('teacher_protocol_invalid')
    return values


def prepare_training(folder, rows, results):
    by_id = {r['id']: r for r in results}
    groups = {'train': [], 'validation': []}
    review = []
    for row in rows:
        result = by_id.get(row['id'])
        if not result or not result['input_ok']:
            review.append({'id': row['id'], 'reason': 'missing_or_invalid_input', 'teacher': result})
            continue
        if row.get('label') and result['action'] not in row['label']['acceptable_actions']:
            review.append({'id': row['id'], 'reason': 'teacher_disagrees', 'teacher': result,
                           'draft_label': row['label']})
            continue
        record = {'id': row['id'], 'group_id': row['group_id'], 'source': row['source'], 'task': 'combat_update',
                  'teacher': {'action': result['action'], 'reason': result['reason'], 'model': 'gpt-6.1-sol'},
                  'messages': [{'role': 'system', 'content': COMBAT_INSTRUCTIONS},
                               {'role': 'user', 'content': json.dumps(row['prompt'], ensure_ascii=False, separators=(',', ':'))},
                               {'role': 'assistant', 'content': json.dumps({'action': result['action'], 'target_id': result['target_id']}, separators=(',', ':'))}]}
        groups[row['usage']].append(record)
    for name, records in groups.items():
        (folder / (name + '.jsonl')).write_text(''.join(json.dumps(r, ensure_ascii=False)+'\n' for r in records), encoding='utf-8')
    write_json(folder / 'needs_review.json', review)
    return {'prepared_counts': {k: len(v) for k, v in groups.items()}, 'needs_review': len(review)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--folder', type=Path, default=DEFAULT)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--batch-size', type=int, default=6)
    parser.add_argument('--prepare-only', action='store_true')
    args = parser.parse_args()
    rows = json.loads((args.folder / 'situations.json').read_text(encoding='utf-8'))
    report_file = args.folder / 'teacher.json'
    report = json.loads(report_file.read_text(encoding='utf-8')) if report_file.exists() else {
        'model': 'gpt-6.1-sol', 'service_tier': 'default', 'results': [], 'calls': [],
        'situations_sha256': hashlib.sha256((args.folder / 'situations.json').read_bytes()).hexdigest(),
        'acceptance_used': False}
    if report['situations_sha256'] != hashlib.sha256((args.folder / 'situations.json').read_bytes()).hexdigest():
        raise ValueError('教师开始后输入发生变化，请另建版本，不能覆盖旧记录')
    if args.prepare_only:
        print(json.dumps(prepare_training(args.folder, rows, report['results']), ensure_ascii=False))
        return
    print(json.dumps({'pending': len(rows)-len(report['results']), 'gpu_used': False}), flush=True)
    if not args.run:
        return
    done = {r['id'] for r in report['results']}
    pending = [r for r in rows if r['id'] not in done]
    service = AppServer()
    try:
        for start in range(0, len(pending), args.batch_size):
            batch = pending[start:start+args.batch_size]
            prompt = json.dumps([{'id': r['id'], 'context': r['prompt']} for r in batch], ensure_ascii=False)
            chunks, timing = [], {}
            origin = time.monotonic()
            call = {'ids': [r['id'] for r in batch], 'prompt_sha256': hashlib.sha256(prompt.encode()).hexdigest()}
            try:
                service.stream(prompt, TEACHER, threading.Event(), chunks.append, model=report['model'],
                               measure=lambda name, elapsed: timing.update({name: elapsed}))
                text = ''.join(chunks)
                results = parse_output(text, batch)
                report['results'].extend(results)
                call.update(output=text, timing=timing, usage=service.last_token_usage, status='completed')
            except Exception as error:
                call.update(output=''.join(chunks), timing=timing, status='failed', error=type(error).__name__+':'+str(error))
                report['calls'].append(call)
                write_json(report_file, report)
                raise
            call['elapsed_seconds'] = time.monotonic()-origin
            report['calls'].append(call)
            write_json(report_file, report)
            print(json.dumps({'reviewed': len(report['results']), 'total': len(rows), 'last_ids': call['ids'],
                              'seconds': call['elapsed_seconds']}, ensure_ascii=False), flush=True)
    finally:
        service.close()
    print(json.dumps(prepare_training(args.folder, rows, report['results']), ensure_ascii=False))


if __name__ == '__main__':
    main()
