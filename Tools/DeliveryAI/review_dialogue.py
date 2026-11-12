"""用固定的虚构现场查看模型如何接话，不连接游戏或合成声音"""
import argparse
import json
import os
import threading
import time
import uuid
from pathlib import Path
from app_server import AppServer
from server import validate_action, validate_context
from streaming import SentenceParser, STREAM_INSTRUCTIONS, compact_context, model_context

ROOT = Path(__file__).resolve().parents[2]


def make_cases(profile):
    player, neighbor, bike = (str(uuid.uuid4()) for _ in range(3))

    def event(kind, text, who='', reply=''):
        return {'event_id': str(uuid.uuid4()), 'subject_id': who, 'kind': kind,
                'description': text, 'reply_to_event_id': reply}

    apology = event('PlayerSpeech', '对不起，刚才是我太急了。', player)
    reply = event('SelfSpeech', '道歉我听见了，车先挪开。', reply=apology['event_id'])
    challenge = event('Speech', '你倒是让开呀，站那儿等人抬呢？', neighbor)
    cases = [
        ('provoke', [event('Speech', '这车把路堵了，我说车呢。', neighbor),
                     event('PlayerSpeech', '他刚才是不是在说你？', player)]),
        ('apology', [event('Impact', '这名快递员刚才骑车碰到了你，现在已经停下。', player), apology]),
        ('answered_then_neighbor', [apology, reply, challenge]),
        ('answered_and_quiet', [apology, reply]),
        ('same_question_again', [apology, reply, event('PlayerSpeech', apology['description'], player)]),
        ('interrupted_reply', [event('PlayerSpeech', '你们到底为啥吵？', player),
                               event('SelfSpeech', '他偏说这路归他管。'),
                               event('Speech', '我说归我扫，你耳朵落家了？', neighbor)]),
        ('horn', [event('Horn', '这辆车在身旁低速停着，已经连续鸣笛五次。', bike),
                  event('Speech', '他这是给你伴奏呢，再大点声。', neighbor)]),
        ('release_vehicle', [event('HoldingVehicle', '你已拽住这辆停着的车，快递员正在道歉。', bike), apology]),
    ]
    result = []
    for name, evidence in cases:
        candidates = [{'action': 'Wait', 'target_id': '', 'duration': 1, 'evidence_ids': []}]
        if name == 'release_vehicle':
            candidates.append({'action': 'ReleaseVehicle', 'target_id': '', 'duration': 1, 'evidence_ids': []})
        else:
            candidates.append({'action': 'LieDown', 'target_id': '', 'duration': 2, 'evidence_ids': []})
        for item in evidence:
            target = item['subject_id']
            if not target:
                continue
            actions = ['LookAt'] if name == 'release_vehicle' else ['LookAt', 'MoveTo']
            if name != 'release_vehicle':
                actions.append('HoldVehicle' if target == bike else 'StrikeOnce')
            for action in actions:
                if not any(c['action'] == action and c['target_id'] == target for c in candidates):
                    candidates.append({'action': action, 'target_id': target, 'duration': 2,
                                       'evidence_ids': [item['event_id']]})
        context = {key: str(uuid.uuid4()) for key in ('world_epoch', 'npc_id', 'request_id')}
        context.update(protocol_version=1, capability_version=1, context_revision=1, plan_revision=1,
                       persona=profile['persona'], evidence=evidence, candidates=candidates,
                       allowed_actions=list(dict.fromkeys(c['action'] for c in candidates)))
        validate_context(context)
        result.append((name, context))
    return result


def main():
    args = argparse.ArgumentParser()
    args.add_argument('--model', default='gpt-6-luna')
    args.add_argument('--output', default='dialogue-style-review.json')
    args.add_argument('--cases', nargs='+')
    args.add_argument('--context-format', choices=('original', 'short'), default='short')
    args = args.parse_args()
    os.environ['DELIVERY_CODEX_MODEL'] = args.model
    profiles = json.loads((ROOT / 'Design/AI/NpcProfiles.json').read_text(encoding='utf-8'))['characters']
    folder = ROOT / 'Saved/AIResearch'
    folder.mkdir(parents=True, exist_ok=True)
    output = folder / Path(args.output).name
    results = []
    service = AppServer()
    try:
        for profile in profiles:
            for name, context in make_cases(profile):
                if args.cases and name not in args.cases:
                    continue
                row = {'character': profile['display_name'], 'case': name, 'model': args.model,
                       'context_format': args.context_format,
                       'scene': context['evidence'], 'candidates': context['candidates'], 'speech': []}
                start = time.monotonic()

                def receive(kind, value):
                    if kind == 'sentence':
                        row.setdefault('first_sentence_ms', round((time.monotonic() - start) * 1000))
                        row['speech'].append(value['text'])

                reader = SentenceParser(context, validate_action, receive)
                encode = model_context if args.context_format == 'short' else compact_context
                prompt = json.dumps(encode(context), ensure_ascii=False, separators=(',', ':'))
                try:
                    service.stream(prompt, STREAM_INSTRUCTIONS, threading.Event(), reader.feed)
                    reader.finish()
                    row.update(action=reader.proposal['action'], target=reader.proposal['target_id'],
                               emotion=reader.proposal['emotion'], valid=True)
                except Exception as error:
                    row.update(valid=False, error=str(error))
                row['complete_ms'] = round((time.monotonic() - start) * 1000)
                results.append(row)
                output.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
                print(json.dumps({k: v for k, v in row.items() if k not in ('scene', 'candidates')},
                                 ensure_ascii=False), flush=True)
    finally:
        service.close()


if __name__ == '__main__':
    main()
