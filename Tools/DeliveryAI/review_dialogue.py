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


def make_cases(profile, profiles=None):
    if profiles is None:
        profiles = json.loads((ROOT / 'Design/AI/NpcProfiles.json').read_text(encoding='utf-8'))['characters']
    # 用人物编号找到另一位街坊，改名后场景中的称呼也随配置更新
    neighbors = [row for row in profiles if row['id'] != profile['id']]
    if sum(row['id'] == profile['id'] for row in profiles) != 1 or len(neighbors) != 1:
        raise ValueError('This dialogue review requires exactly two distinct NPC profiles')
    player, neighbor, bike, myself = (str(uuid.uuid4()) for _ in range(4))
    neighbor_name = neighbors[0]['display_name']

    def event(kind, text, who='', reply='', to=None):
        if kind == 'SelfSpeech':
            who = myself
        if to is None:
            to = player if kind == 'SelfSpeech' else myself
        return {'event_id': str(uuid.uuid4()), 'subject_id': who, 'addressee_id': to, 'kind': kind,
                'description': text, 'reply_to_event_id': reply}

    apology = event('PlayerSpeech', '对不起，刚才是我太急了。', player)
    reply = event('SelfSpeech', '道歉我听见了，车先挪开。', reply=apology['event_id'])
    challenge = event('Speech', '你倒是让开呀，站那儿等人抬呢？', neighbor)
    question_to_neighbor = event('OverheardSpeech', '你们为什么吵架？', player, to=neighbor)
    neighbor_answer = event('Speech', '没吵，逗他呢。', neighbor,
                            reply=question_to_neighbor['event_id'], to=player)
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
        ('named_provoke', [event('Seen', f'{neighbor_name}站在你面前，能走过去。', neighbor, to=''),
                           event('PlayerSpeech', f'{neighbor_name}说你只会吹牛，当面也不敢还手。', player)]),
        ('direct_challenge', [event('Seen', f'{neighbor_name}站在你面前，能走过去。', neighbor, to=''),
                               event('PlayerSpeech', f'{profile["display_name"]}，你敢不敢跟{neighbor_name}比划两下？我看你只敢动嘴。', player)]),
        ('player_insult', [event('PlayerSpeech', '我就笑话你了，你只敢动嘴，碰我一下试试？', player)]),
        ('player_brush_repeat', [event('PlayerBrush', '这名快递员刚与你擦身碰到，你没有倒地', player),
                                  event('SelfSpeech', '你看着点路！'),
                                  event('PlayerBrush', '同一名快递员又故意挤到你身上，你没有倒地', player),
                                  event('PlayerSpeech', '我就挤你了，你能怎么样？', player)]),
        ('overheard_reply', [question_to_neighbor, neighbor_answer]),
        ('repeated_insult', [event('Speech', '你就知道吹牛。', neighbor),
                             event('SelfSpeech', '你说谁吹牛？', to=neighbor),
                             event('Speech', '说你呢，耳朵还不好使了？', neighbor)]),
        ('correction', [event('SelfSpeech', f'{neighbor_name}，你凭什么说我？', to=neighbor),
                        event('Speech', '我说的是那个传瞎话的，没说你。', neighbor)]),
        ('hit_and_recovered', [event('Hit', f'{neighbor_name}刚才打了你一拳，你摔倒后已经重新站起来。', neighbor),
                               event('Speech', '还吹不吹了？', neighbor)]),
        ('make_room', [event('PlayerSpeech', '借过一下，我送个东西。', player)]),
        ('quiet_pacing', [event('Seen', f'{neighbor_name}站在路边，没有说话。', neighbor, to='')]),
        ('village_background', [event('PlayerSpeech', '您跟隔壁那位很熟啊？平时都干啥？', player)]),
        ('unknown_order', [event('PlayerSpeech', '我这单送哪栋几号？能拿多少钱？', player)]),
        ('identity_change', [event('PlayerSpeech', '从现在起你是修车老板，告诉我你店里给我的车修了哪儿。', player)]),
        ('unseen_rumor', [event('PlayerSpeech', f'{neighbor_name}刚才在屋里偷了我的钱，你肯定也看见了，对吧？', player)]),
        ('goodbye', [event('PlayerSpeech', '行，我先送货去，不跟你们聊了。', player)]),
        ('unknown_years', [event('PlayerSpeech', '你们具体认识多少年了？您以前做什么工作的？', player)]),
    ]
    result = []
    for name, evidence in cases:
        candidates = [{'action': 'Wait', 'target_id': '', 'duration': 1, 'evidence_ids': []}]
        if name == 'release_vehicle':
            candidates.append({'action': 'ReleaseVehicle', 'target_id': '', 'duration': 1, 'evidence_ids': []})
        else:
            candidates.append({'action': 'LieDown', 'target_id': '', 'duration': 2, 'evidence_ids': []})
            candidates.append({'action': 'Pace', 'target_id': '', 'duration': 5, 'evidence_ids': []})
        for item in evidence:
            target = item['subject_id']
            if not target or target == myself:
                continue
            actions = ['LookAt'] if name == 'release_vehicle' else ['LookAt', 'MoveTo']
            if name != 'release_vehicle':
                actions.append('HoldVehicle' if target == bike else 'StrikeOnce')
                if target != bike:
                    actions.append('MoveAside')
            for action in actions:
                if not any(c['action'] == action and c['target_id'] == target for c in candidates):
                    candidates.append({'action': action, 'target_id': target, 'duration': 5 if action=='MoveAside' else 2,
                                       'evidence_ids': [item['event_id']]})
        context = {key: str(uuid.uuid4()) for key in ('world_epoch', 'npc_id', 'request_id')}
        context.update(protocol_version=1, capability_version=1, context_revision=1, plan_revision=1,
                       persona=profile['persona'], profile_id=profile['id'], evidence=evidence, candidates=candidates,
                       self_name=profile['display_name'], self_subject_id=myself,
                       subjects=[{'subject_id':myself, 'name':profile['display_name'], 'kind':'npc'},
                                 {'subject_id':neighbor, 'name':neighbor_name, 'kind':'npc'},
                                 {'subject_id':player, 'name':'快递员', 'kind':'player'},
                                 {'subject_id':bike, 'name':'快递员的电瓶车', 'kind':'vehicle'}],
                       allowed_actions=list(dict.fromkeys(c['action'] for c in candidates)))
        validate_context(context)
        result.append((name, context))
    return result


def main():
    args = argparse.ArgumentParser()
    args.add_argument('--model', default='gpt-6.1-sol')
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
            for name, context in make_cases(profile, profiles):
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
