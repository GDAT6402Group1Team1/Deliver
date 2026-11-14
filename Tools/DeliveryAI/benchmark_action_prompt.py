"""固定两个场景、四次模型对照；默认只准备，--run 才执行，不补跑、不调用声音或游戏"""
import argparse
import copy
from dataclasses import asdict
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import threading
import time
import uuid

from action_prompt import select_action_instructions
from app_server import AppServer
from benchmark_app_server_isolation import validation_context
from companion_context import companion_instructions
from server import validate_action, validate_context
from streaming import SentenceParser, STREAM_INSTRUCTIONS

ROOT = Path(__file__).resolve().parents[2]


def sha(text):
    return hashlib.sha256(text.encode('utf-8')).hexdigest()


class ObservedAppServer(AppServer):
    """只记录通知类型与到达时刻，不记录推理文本，不改变模型调用或取消行为"""
    def reset_observation(self):
        self.observed = []
        self.observed_thread = self.observed_turn = None
        self.observed_started = time.monotonic()

    def _read(self, deadline, cancel=None):
        message = super()._read(deadline, cancel)
        method, params = message.get('method'), message.get('params', {})
        if isinstance(method, str) and isinstance(params, dict):
            item = params.get('item', {})
            event = {'method': method, 'ms': round((time.monotonic()-self.observed_started)*1000),
                     'thread': params.get('threadId'), 'turn': params.get('turnId')}
            if method in ('item/started', 'item/completed') and isinstance(item, dict):
                event['item_type'] = item.get('type')
            if method == 'model/safetyBuffering/updated' and type(params.get('showBufferingUi')) is bool:
                event['show_buffering_ui'] = params['showBufferingUi']
            self.observed.append(event)
        return message

    def _rpc(self, method, params, cancel=None):
        result = super()._rpc(method, params, cancel)
        if method == 'thread/start':
            self.observed_thread = result['thread']['id']
        elif method == 'turn/start':
            self.observed_turn = result['turn']['id']
        return result

    def observation_summary(self):
        groups = {}
        for event in self.observed:
            if event['thread'] != self.observed_thread or event['turn'] not in (None, self.observed_turn):
                continue
            key = event['method'] + (':' + str(event['item_type']) if 'item_type' in event else '')
            group = groups.setdefault(key, {'first_ms': event['ms'], 'last_ms': event['ms'], 'count': 0})
            group.update(last_ms=event['ms'], count=group['count']+1)
            if 'show_buffering_ui' in event:
                group['show_buffering_ui'] = event['show_buffering_ui']
        return groups


def prepare():
    source = ROOT/'Saved/AIResearch/action-prompt-offline-20261114.json'
    content = source.read_text(encoding='utf-8')
    audit = json.loads(content)
    by_name = {case['name']: case for case in audit['cases']}
    plans = []
    for index, name in enumerate(('road_correction', 'companion_follow_to_stay')):
        case = by_name[name]
        encoded = copy.deepcopy(case['model_context'])
        context, targets = validation_context(encoded)
        for key in ('world_epoch', 'npc_id', 'request_id'):
            context[key] = str(uuid.uuid5(uuid.NAMESPACE_URL, 'action-prompt-ab:' + name + ':' + key))
        context['allowed_actions'] = case['allowed_actions']
        validate_context(context)
        for candidate in context['candidates']:
            validate_action(context, {**candidate, 'speech': '', 'emotion': 'Calm'})
        original = case['source_instructions']
        if original != STREAM_INSTRUCTIONS + companion_instructions(context):
            raise ValueError('production_instructions_changed_prepare_new_audit')
        selection = select_action_instructions(original, context['allowed_actions'], context['candidates'])
        if selection.instructions_sha256 != case['selection']['instructions_sha256']:
            raise ValueError('selection_changed_prepare_new_audit')
        prompt = json.dumps(encoded, ensure_ascii=False, separators=(',', ':'))
        order = ('original', 'selected') if index == 0 else ('selected', 'original')
        for variant in order:
            instructions = original if variant == 'original' else selection.instructions
            plans.append({'case': name, 'variant': variant, 'model_context': encoded,
                          'validation_context': context, 'speech_targets': targets,
                          'prompt': prompt, 'prompt_sha256': sha(prompt),
                          'instructions': instructions, 'instructions_sha256': sha(instructions),
                          'selection': asdict(selection), 'capability_source': case['capability_source']})
    return {'status': 'prepared', 'model': 'gpt-6.1-sol', 'effort': 'low', 'planned_calls': 4,
            'attempted_calls': 0, 'source_sha256': sha(content), 'plans': plans, 'results': [],
            'source_files_sha256': {name: hashlib.sha256((ROOT/'Tools/DeliveryAI'/name).read_bytes()).hexdigest()
                                    for name in ('app_server.py', 'action_prompt.py', 'benchmark_action_prompt.py')}}


def main():
    arguments = argparse.ArgumentParser()
    arguments.add_argument('--run', action='store_true')
    arguments.add_argument('--output', default='action-prompt-ab-20261114.json')
    args = arguments.parse_args()
    output = ROOT/'Saved/AIResearch'/Path(args.output).name
    if output.exists() and json.loads(output.read_text(encoding='utf-8')).get('attempted_calls', 0):
        arguments.error('已有真实尝试记录，不覆盖或补跑，请保留这批结果')
    report = prepare()
    def save():
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    save()
    print(json.dumps({'status': 'prepared', 'planned_calls': 4, 'running': args.run}), flush=True)
    if not args.run:
        return
    service = ObservedAppServer()
    previous_effort = os.environ.get('DELIVERY_CODEX_REASONING_EFFORT')
    os.environ['DELIVERY_CODEX_REASONING_EFFORT'] = 'low'
    try:
        for plan in report['plans']:
            row = {'case': plan['case'], 'variant': plan['variant'], 'speech': [], 'stages_ms': {},
                   'started_utc': datetime.now(timezone.utc).isoformat(),
                   'prompt_sha256': plan['prompt_sha256'], 'instructions_sha256': plan['instructions_sha256']}
            service.reset_observation()
            started = time.monotonic()
            def received(kind, value):
                if kind == 'proposal':
                    row['decision_ms'] = round((time.monotonic()-started)*1000)
                elif kind == 'sentence':
                    row.setdefault('first_sentence_ms', round((time.monotonic()-started)*1000))
                    row['speech'].append(value['text'])
            reader = SentenceParser(plan['validation_context'], validate_action, received)
            reader.speech_targets = plan['speech_targets']
            report['attempted_calls'] += 1
            save()
            try:
                service.stream(plan['prompt'], plan['instructions'], threading.Event(), reader.feed,
                               model='gpt-6.1-sol',
                               measure=lambda stage, elapsed: row['stages_ms'].update({stage: round(elapsed)}))
                reader.finish()
                chosen = next(index for index, candidate in enumerate(plan['validation_context']['candidates'])
                              if candidate['action'] == reader.proposal['action'] and candidate['target_id'] == reader.proposal['target_id'])
                row.update(valid=True, action=reader.proposal['action'], duration=reader.proposal['duration'],
                           target=plan['model_context']['candidates'][chosen]['target_id'], emotion=reader.proposal['emotion'],
                           speech_target=next((key for key, value in plan['speech_targets'].items()
                                               if value == reader.proposal.get('speech_target_id')), None))
            except Exception as error:
                row.update(valid=False, error=type(error).__name__)
            row.update(complete_ms=round((time.monotonic()-started)*1000), usage=service.last_token_usage,
                       settings=service.last_model_settings, notifications=service.observation_summary())
            report['results'].append(row)
            save()
            print(json.dumps(row, ensure_ascii=True), flush=True)
        report['status'] = 'completed_no_retry'
        save()
    finally:
        service.close()
        if previous_effort is None:
            os.environ.pop('DELIVERY_CODEX_REASONING_EFFORT', None)
        else:
            os.environ['DELIVERY_CODEX_REASONING_EFFORT'] = previous_effort


if __name__ == '__main__':
    main()
