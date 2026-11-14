"""用六个固定场景比较 NPC 模型，保留提示与人物设置，不启动游戏或合成声音"""
import argparse
import copy
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import threading
import time

from app_server import AppServer
from benchmark_app_server_isolation import validation_context
from benchmark_prompt_deduplication import companion_case
from companion_context import ERRAND_ACTIONS, companion_instructions
from server import validate_action
from streaming import SentenceParser, STREAM_INSTRUCTIONS


ROOT = Path(__file__).resolve().parents[2]


def sha(text):
    return hashlib.sha256(text.encode('utf-8')).hexdigest()


def prepare_cases():
    """复用已保存的对照情境，两种模型收到完全相同的当前提示"""
    source = ROOT / 'Saved/AIResearch/prompt-deduplication-20261114.json'
    previous = json.loads(source.read_text(encoding='utf-8'))
    names = ('resident_challenge', 'resident_escape')
    cases = [{'name': name, 'context': copy.deepcopy(previous['cases'][index]['context']),
              'source': source.name, 'review': '按人物和现场选择行动，不因固定轮数必定攻击或逃跑'}
             for index, name in enumerate(names)]
    fixtures = ROOT / 'Saved/AIResearch/dialogue-prompt-experiment.json'
    examples = json.loads(fixtures.read_text(encoding='utf-8'))['cases']
    for name in ('road_correction', 'ordinary_request', 'player_changes_rules'):
        item = next(row for row in examples if row['case_id'] == name)
        cases.append({'name': name, 'context': json.loads(item['model_input']),
                      'source': fixtures.name, 'review': item['review']})
    companion = companion_case()
    cases.append({'name': companion['name'], 'context': companion['context'],
                  'source': 'benchmark_prompt_deduplication.companion_case',
                  'review': '主人要求留在原地，应选择 Stay；不能假称已完成取件或送件'})
    for case in cases:
        case['instructions'] = STREAM_INSTRUCTIONS + companion_instructions(case['context'])
        case['prompt'] = json.dumps(case['context'], ensure_ascii=False, separators=(',', ':'))
        case['prompt_sha256'] = sha(case['prompt'])
        case['instructions_sha256'] = sha(case['instructions'])
        context, _ = validation_context(case['context'])
        for candidate in context['candidates']:
            validate_action(context, {**candidate, 'speech': '', 'emotion': 'Calm'})
    return cases


def prepare_recorded_cases(request_ids):
    """保留真实请求的人物名单、候选和原话，核对提示与当时完全一致"""
    source = ROOT / 'Saved/Logs/NpcDialogue.ndjson'
    records = {}
    for line in source.read_text(encoding='utf-8').splitlines():
        item = json.loads(line)
        if item.get('request_id') in request_ids and 'model_context' in item:
            records[item['request_id']] = item
    if set(records) != set(request_ids):
        raise ValueError('requested_context_missing_from_log')
    backend_log = (ROOT / 'Saved/Logs/AIResidentBackend.log').read_text(encoding='utf-8')
    cases = []
    for request_id in request_ids:
        record = records[request_id]
        encoded = record['model_context']
        if not encoded.get('subjects'):
            raise ValueError('record_has_no_subjects')
        version = 3 if any(c['action'] in ERRAND_ACTIONS for c in encoded['candidates']) else (2 if 'companion_context' in encoded else 1)
        instructions = STREAM_INSTRUCTIONS + companion_instructions({**encoded, 'capability_version': version})
        if sha(instructions) != record.get('instructions_sha256'):
            raise ValueError('recorded_instructions_differ_from_current')
        baseline = {key: record.get(key) for key in ('time', 'model', 'chosen', 'speech', 'emotion', 'error')}
        match = re.search(r'request=' + re.escape(request_id) + r' stage=first_sentence elapsed_ms=(\d+)', backend_log)
        if match:
            baseline['first_sentence_ms'] = int(match[1])
        prompt = json.dumps(encoded, ensure_ascii=False, separators=(',', ':'))
        case = {'name': request_id, 'context': encoded, 'source': source.name, 'capability_version': version,
                'historical_baseline': baseline, 'instructions': instructions, 'prompt': prompt,
                'instructions_sha256': sha(instructions), 'prompt_sha256': sha(prompt),
                'review': '使用当时真实名单和候选；检查收话人、实际动作、自然口语与尚未完成的事'}
        context, _ = validation_context(encoded)
        context['capability_version'] = version
        for candidate in context['candidates']:
            validate_action(context, {**candidate, 'speech': '', 'emotion': 'Calm'})
        cases.append(case)
    return cases


def run_case(service, case, model):
    context, targets = validation_context(case['context'])
    context['capability_version'] = case.get('capability_version', context['capability_version'])
    row = {'case': case['name'], 'model': model, 'speech': [], 'stages_ms': {},
           'prompt_sha256': case['prompt_sha256'], 'instructions_sha256': case['instructions_sha256'],
           'started_utc': datetime.now(timezone.utc).isoformat()}
    started = time.monotonic()

    def receive(kind, value):
        if kind == 'proposal':
            row['decision_ms'] = round((time.monotonic() - started) * 1000)
        elif kind == 'sentence':
            row.setdefault('first_sentence_ms', round((time.monotonic() - started) * 1000))
            row['speech'].append(value['text'])

    reader = SentenceParser(context, validate_action, receive)
    reader.speech_targets = targets
    visible_output = []

    def feed(delta):
        # 只保存模型公开回复，失败时可区分格式错误和游戏校验拒绝
        visible_output.append(delta)
        reader.feed(delta)

    try:
        service.stream(case['prompt'], case['instructions'], threading.Event(), feed, model=model,
                       measure=lambda stage, elapsed: row['stages_ms'].update({stage: round(elapsed)}))
        reader.finish()
        proposal = reader.proposal
        chosen = next(index for index, candidate in enumerate(context['candidates'])
                      if candidate['action'] == proposal['action'] and candidate['target_id'] == proposal['target_id'])
        row.update(protocol_valid=True, action=proposal['action'], duration=proposal['duration'],
                   target=case['context']['candidates'][chosen]['target_id'], emotion=proposal['emotion'],
                   speech_target=next((key for key, value in targets.items()
                                       if value == proposal.get('speech_target_id')), None))
    except Exception as error:
        row.update(protocol_valid=False, error=type(error).__name__)
        if isinstance(error, ValueError):
            row['validation_error'] = str(error)[:200]
    # 协议通过只说明动作与格式有效，台词是否自然仍需检查原话
    row.update(complete_ms=round((time.monotonic() - started) * 1000),
               usage=copy.deepcopy(service.last_token_usage), settings=copy.deepcopy(service.last_model_settings),
               quality_review='pending', visible_output=''.join(visible_output)[:8192])
    return row


def summarize(rows, models):
    summary = {}
    for model in models:
        selected = [row for row in rows if row['model'] == model]
        elapsed = [row['first_sentence_ms'] for row in selected if row.get('protocol_valid') and 'first_sentence_ms' in row]
        summary[model] = {'attempts': len(selected), 'protocol_valid': sum(bool(row.get('protocol_valid')) for row in selected)}
        if elapsed:
            summary[model].update(first_sentence_median_ms=statistics.median(elapsed),
                                  first_sentence_min_ms=min(elapsed), first_sentence_max_ms=max(elapsed))
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--output', default='reply-models-20261114.json')
    parser.add_argument('--case', action='append')
    parser.add_argument('--model', action='append', choices=('gpt-6.1-sol', 'gpt-6-luna'))
    parser.add_argument('--effort', choices=('low', 'medium'), default='low')
    parser.add_argument('--request-id', action='append')
    args = parser.parse_args()
    models = tuple(dict.fromkeys(args.model or ('gpt-6.1-sol', 'gpt-6-luna')))
    if args.request_id and (args.case or len(args.request_id) > 6):
        parser.error('真实日志请求与固定场景分开选择，每次最多六条')
    cases = prepare_recorded_cases(args.request_id) if args.request_id else prepare_cases()
    if args.case:
        if set(args.case) - {case['name'] for case in cases}:
            parser.error('指定的场景不存在')
        cases = [case for case in cases if case['name'] in args.case]
    output = ROOT / 'Saved/AIResearch' / Path(args.output).name
    if output.exists() and json.loads(output.read_text(encoding='utf-8')).get('attempted_calls', 0):
        parser.error('保留已有真实结果，请选择新的输出文件')
    report = {'status': 'prepared', 'models': models, 'reasoning_effort': args.effort, 'service_tier': 'default',
              'planned_calls': len(cases) * len(models), 'attempted_calls': 0, 'cases': cases, 'results': [],
              'production_settings_changed': False,
              'source_sha256': {name: hashlib.sha256((ROOT / 'Tools/DeliveryAI' / name).read_bytes()).hexdigest()
                                for name in ('app_server.py', 'streaming.py', 'companion_context.py')}}

    def save():
        output.parent.mkdir(parents=True, exist_ok=True)
        report['summary'] = summarize(report['results'], models)
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    save()
    print(json.dumps({'status': 'prepared', 'calls': report['planned_calls'], 'run': args.run}), flush=True)
    if not args.run:
        return
    environment = {'DELIVERY_CODEX_REASONING_EFFORT': args.effort, 'DELIVERY_CODEX_SERVICE_TIER': 'default'}
    previous = {key: os.environ.get(key) for key in environment}
    os.environ.update(environment)
    service = AppServer()
    try:
        for index, case in enumerate(cases):
            # 交替调用顺序，避免某个模型总在冷启动或总排在另一个模型后面
            for model in models if index % 2 == 0 else reversed(models):
                report['attempted_calls'] += 1
                save()
                row = run_case(service, case, model)
                report['results'].append(row)
                if service.model_catalog:
                    report['observed_catalog'] = {name: service.model_catalog.get(name) for name in models}
                save()
                print(json.dumps(row, ensure_ascii=False), flush=True)
        report['status'] = 'completed_no_retry'
        save()
        print(json.dumps(report['summary'], ensure_ascii=False), flush=True)
    finally:
        service.close()
        for key, value in previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


if __name__ == '__main__':
    main()
