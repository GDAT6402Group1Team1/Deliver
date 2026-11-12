"""比较同一场景使用原编号和短编号时的模型回复，不修改游戏或后端配置"""
import argparse
import copy
import json
import threading
import time
import uuid
from pathlib import Path

from app_server import AppServer
from review_dialogue import make_cases
from server import validate_action
from streaming import SentenceParser, STREAM_INSTRUCTIONS, compact_context, model_context

ROOT = Path(__file__).resolve().parents[2]


def crowded_case(profile):
    """保留排在第 32 项之后的车辆选项，观察对象增多时的输入与首句耗时"""
    context = copy.deepcopy(make_cases(profile)[0][1])
    context['evidence'] = []
    context['candidates'] = [{'action':'Wait', 'target_id':'', 'duration':1, 'evidence_ids':[]}]
    for index in range(12):
        subject, event = str(uuid.uuid4()), str(uuid.uuid4())
        context['evidence'].append({'event_id':event, 'subject_id':subject, 'kind':'Seen',
                                    'description':f'路旁第{index+1}名行人，没有参与争执'})
        for action in ('LookAt', 'MoveTo', 'StrikeOnce'):
            context['candidates'].append({'action':action, 'target_id':subject, 'duration':2, 'evidence_ids':[event]})
    bike, event = str(uuid.uuid4()), str(uuid.uuid4())
    context['evidence'].append({'event_id':event, 'subject_id':bike, 'kind':'Horn',
                               'description':'身旁这辆停着的电瓶车刚擦碰到你，之后连续鸣笛五次'})
    for action in ('LookAt', 'MoveTo', 'HoldVehicle'):
        context['candidates'].append({'action':action, 'target_id':bike, 'duration':2, 'evidence_ids':[event]})
    context['allowed_actions'] = list(dict.fromkeys(item['action'] for item in context['candidates']))
    return 'crowded_horn', context


def main():
    arguments = argparse.ArgumentParser()
    arguments.add_argument('--model', default='gpt-6.1-sol')
    arguments.add_argument('--rounds', type=int, default=1)
    arguments.add_argument('--cases', nargs='+', default=['provoke', 'interrupted_reply', 'release_vehicle', 'answered_and_quiet'])
    arguments.add_argument('--output', default='context-identifiers-comparison.json')
    args = arguments.parse_args()
    output = ROOT / 'Saved/AIResearch' / Path(args.output).name
    output.parent.mkdir(parents=True, exist_ok=True)
    profiles = json.loads((ROOT / 'Design/AI/NpcProfiles.json').read_text(encoding='utf-8'))['characters']
    results = []
    service = AppServer()
    service._start()
    try:
        for repeat in range(args.rounds):
            for profile_index, profile in enumerate(profiles):
                for case_index, (name, context) in enumerate(make_cases(profile) + [crowded_case(profile)]):
                    if name not in args.cases:
                        continue
                    variants = [('original', compact_context), ('short_ids', model_context)]
                    if (repeat + profile_index + case_index) % 2:
                        variants.reverse()
                    for variant, encode in variants:
                        row = {'model': args.model, 'character': profile['display_name'], 'case': name,
                               'round': repeat, 'variant': variant, 'speech': [], 'events': [],
                               'scene': context['evidence'], 'candidates': context['candidates']}
                        prompt = json.dumps(encode(context), ensure_ascii=False, separators=(',', ':'))
                        row['prompt_bytes'] = len(prompt.encode('utf-8'))
                        row['prompt_chars'] = len(prompt)
                        start = time.monotonic()

                        def receive(kind, value):
                            row['events'].append({'kind': kind, 'ms': round((time.monotonic()-start)*1000)})
                            if kind == 'sentence':
                                row.setdefault('first_sentence_ms', round((time.monotonic()-start)*1000))
                                row['speech'].append(value['text'])

                        reader = SentenceParser(context, validate_action, receive)

                        def delta(text):
                            if text:
                                row.setdefault('first_delta_ms', round((time.monotonic()-start)*1000))
                            reader.feed(text)

                        try:
                            service.stream(prompt, STREAM_INSTRUCTIONS, threading.Event(), delta, model=args.model)
                            reader.finish()
                            row.update(valid=True, action=reader.proposal['action'], target=reader.proposal['target_id'])
                        except Exception as error:
                            row.update(valid=False, error=type(error).__name__)
                        row['complete_ms'] = round((time.monotonic()-start)*1000)
                        results.append(row)
                        output.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
                        print(json.dumps({key:value for key,value in row.items() if key not in ('scene','candidates','events')}, ensure_ascii=False), flush=True)
    finally:
        service.close()


if __name__ == '__main__':
    main()
