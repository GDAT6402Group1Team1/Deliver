"""用已有真实上下文交替比较两版 App Server，只调用模型，不启动游戏或合成声音"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import threading
import time
import types
import uuid

from app_server import AppServer
from server import ENVELOPE, validate_action
from streaming import SentenceParser, STREAM_INSTRUCTIONS

ROOT = Path(__file__).resolve().parents[2]


def validation_context(encoded):
    """日志短编号只用于模型输入，服务器校验侧恢复稳定且合法的 UUID"""
    context = copy.deepcopy(encoded)
    context.setdefault('subjects', [])
    context.setdefault('self_subject_id', '')
    identity = lambda value: str(uuid.uuid5(uuid.NAMESPACE_URL, 'delivery-provider-benchmark:' + value)) if value else ''
    for item in context['evidence']:
        for key in ('event_id', 'subject_id', 'addressee_id', 'reply_to_event_id'):
            if key in item:
                item[key] = identity(item[key])
    for item in context['candidates']:
        item['target_id'] = identity(item['target_id'])
        item['evidence_ids'] = [identity(value) for value in item['evidence_ids']]
    for person in context['subjects']:
        person['subject_id'] = identity(person['subject_id'])
    context['self_subject_id'] = identity(context['self_subject_id'])
    context.update({key: 'benchmark' for key in ENVELOPE})
    context.update(protocol_version=1, capability_version=2 if 'companion_context' in context else 1,
                   context_revision=1, plan_revision=1)
    context['allowed_actions'] = list(dict.fromkeys(c['action'] for c in context['candidates']))
    return context, {person['subject_id']: identity(person['subject_id']) for person in encoded.get('subjects', [])}


def main():
    arguments = argparse.ArgumentParser()
    arguments.add_argument('--request-id', action='append', required=True)
    arguments.add_argument('--rounds', type=int, choices=(1, 2), default=1)
    arguments.add_argument('--baseline-ref', default='79f4491')
    arguments.add_argument('--output', default='app-server-isolation-comparison.json')
    args = arguments.parse_args()
    if len(args.request_id) > 3:
        arguments.error('每次最多选择三个真实上下文')
    source = ROOT / 'Saved/Logs/NpcDialogue.ndjson'
    data = source.read_bytes()
    records = {row['request_id']: row for line in data.decode('utf-8').splitlines()
               if (row := json.loads(line)).get('request_id') in args.request_id and 'model_context' in row}
    if set(records) != set(args.request_id):
        arguments.error('指定请求没有完整 model_context')
    instructions_hash = hashlib.sha256(STREAM_INSTRUCTIONS.encode('utf-8')).hexdigest()
    if any(row.get('instructions_sha256') != instructions_hash for row in records.values()):
        arguments.error('当前提示词与记录不同，不能声称是相同上下文对照')
    baseline = subprocess.check_output(['git', 'show', args.baseline_ref + ':Tools/DeliveryAI/app_server.py'], cwd=ROOT)
    previous = types.ModuleType('delivery_previous_app_server')
    exec(compile(baseline, 'previous_app_server.py', 'exec'), previous.__dict__)
    services = {'before': previous.AppServer(), 'isolated': AppServer()}
    output = ROOT / 'Saved/AIResearch' / Path(args.output).name
    output.parent.mkdir(parents=True, exist_ok=True)
    report = {'source_sha256': hashlib.sha256(data).hexdigest(), 'baseline_ref': args.baseline_ref,
              'baseline_source_sha256': hashlib.sha256(baseline).hexdigest(),
              'instructions_sha256': instructions_hash, 'rows': []}
    try:
        for repeat in range(args.rounds):
            for index, request_id in enumerate(args.request_id):
                record = records[request_id]
                prompt = json.dumps(record['model_context'], ensure_ascii=False, separators=(',', ':'))
                variants = ['before', 'isolated'] if (repeat + index) % 2 == 0 else ['isolated', 'before']
                for variant in variants:
                    service = services[variant]
                    row = {'request_id': request_id, 'source_time': record['time'], 'variant': variant,
                           'round': repeat, 'model': record['model'], 'speech': [], 'stages_ms': {},
                           'prompt_sha256': hashlib.sha256(prompt.encode('utf-8')).hexdigest()}
                    # 日志保存的是模型短编号，验证仍使用相同候选与实际解析器
                    context, speech_targets = validation_context(record['model_context'])
                    started = time.monotonic()

                    def receive(kind, value):
                        if kind == 'sentence':
                            row.setdefault('first_sentence_ms', round((time.monotonic()-started)*1000))
                            row['speech'].append(value['text'])

                    reader = SentenceParser(context, validate_action, receive)
                    reader.speech_targets = speech_targets
                    try:
                        service.stream(prompt, STREAM_INSTRUCTIONS, threading.Event(), reader.feed,
                                       model=record['model'],
                                       measure=lambda stage, elapsed: row['stages_ms'].update({stage: round(elapsed)}))
                        reader.finish()
                        targets = {value: key for key, value in speech_targets.items()}
                        row.update(valid=True, action=reader.proposal['action'], target=targets.get(reader.proposal['target_id'], ''),
                                   emotion=reader.proposal['emotion'])
                    except Exception as error:
                        row.update(valid=False, error=type(error).__name__)
                    row['complete_ms'] = round((time.monotonic()-started)*1000)
                    row['usage'] = service.last_token_usage
                    row['settings'] = getattr(service, 'last_model_settings', {'model': record['model'], 'effort': 'low'})
                    report['rows'].append(row)
                    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
                    print(json.dumps(row, ensure_ascii=True), flush=True)
    finally:
        for service in services.values():
            service.close()


if __name__ == '__main__':
    main()
