"""逐轮比较街坊对话，只调用模型，不连接 UE、不合成声音，也不模拟命中"""
import argparse
import copy
import hashlib
import json
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

import dialogue_background
from app_server import AppServer
from server import validate_action, validate_context
from streaming import SentenceParser, STREAM_INSTRUCTIONS, model_context


ROOT = Path(__file__).resolve().parents[2]
MAX_CALLS = 12
MAX_NEIGHBOR_REPLIES = 3
ATTACKS = {'Fight', 'StrikeOnce'}


class ConversationScene:
    """保存这次虚构场景中双方各自听到的话，模型返回后才加入下一轮"""

    def __init__(self, profiles, scenario):
        self.scenario = scenario
        self.profiles = {row['id']: row for row in profiles['characters']}
        if set(self.profiles) != {'chunlai', 'xiumei'}:
            raise ValueError('This review requires the chunlai and xiumei profiles')
        self.ids = {key: self.identity(key) for key in (*self.profiles, 'player')}
        self.history = {key: [] for key in self.profiles}
        self.seen = {key: [] for key in self.profiles}
        self.serial = 0
        for key in self.profiles:
            other = self.neighbor(key)
            self.history[key] = [
                self.event('Perceived', other, '',
                           f'感知到附近的{self.name(other)}，距离约 180 厘米', source='Seen'),
                self.event('Perceived', 'player', '',
                           '感知到附近的快递员，距离约 170 厘米', source='Seen'),
                self.event('SelfCombat', key, '',
                           '这阵子实际挨了 0 记轻拳；上一轮交战挥拳 0 次，实际命中 0 次。现在站着，可以行动。',
                           source='ActionResult'),
            ]
        self.fixture = []
        if scenario == 'actual':
            # 两版使用同一段实机开场，后续接话全部来自本次模型输出
            self.fixture = [
                {'speaker': 'chunlai', 'target': 'xiumei', 'text': '今儿不挑我毛病啦？',
                 'source': 'NpcDialogue.ndjson 2026-11-14 02:59:58'},
                {'speaker': 'xiumei', 'target': 'chunlai', 'text': '你还盼着挨数落啊？',
                 'source': 'NpcDialogue.ndjson 2026-11-14 03:00:17'},
                {'speaker': 'chunlai', 'target': 'xiumei', 'text': '我盼你夸一句，难哪！',
                 'source': 'NpcDialogue.ndjson 2026-11-14 03:00:30'},
            ]
            for item in self.fixture:
                self.npc_says(item['speaker'], self.ids[item['target']], item['text'])
        if scenario == 'apology':
            # 这个对照从双方已经把话说开的状态开始，预置内容单独标明来源
            apology = '刚才是我瞎起哄，对不起，我不该挑你们打架。'
            event_id = self.player_says(apology)
            self.fixture = [
                {'speaker': 'player', 'text': apology, 'source': 'authored_control'},
                {'speaker': 'xiumei', 'text': '行，刚才那事过去了。', 'source': 'authored_control'},
            ]
            self.npc_says('xiumei', self.ids['player'], self.fixture[1]['text'], event_id)

    def identity(self, label):
        return str(uuid.uuid5(uuid.NAMESPACE_URL, f'delivery-conflict-review/{self.scenario}/{label}'))

    def name(self, key):
        return '快递员' if key == 'player' else self.profiles[key]['display_name']

    def neighbor(self, key):
        return next(other for other in self.profiles if other != key)

    def event(self, kind, speaker, addressee, text, reply='', source='Heard', event_id=None):
        self.serial += 1
        return {'event_id': event_id or self.identity(f'event/{self.serial}'),
                'subject_id': self.ids.get(speaker, speaker),
                'addressee_id': self.ids.get(addressee, addressee),
                'kind': kind, 'source': source, 'description': text,
                'reply_to_event_id': reply}

    def player_says(self, text):
        event = self.event('PlayerSpeech', 'player', 'xiumei', text)
        self.history['xiumei'].append(event)
        self.history['chunlai'].append({**event, 'kind': 'OverheardSpeech'})
        return event['event_id']

    def npc_says(self, speaker, target_id, text, reply_to=''):
        if not text:
            return
        event = self.event('SelfSpeech', speaker, target_id, text,
                           reply=reply_to, source='ActionResult')
        self.history[speaker].append(event)
        other = self.neighbor(speaker)
        kind = 'Speech' if target_id == self.ids[other] else 'OverheardSpeech'
        self.history[other].append({**event, 'kind': kind, 'source': 'Heard'})

    def player_lines(self):
        female, male = self.name('xiumei'), self.name('chunlai')
        return {
            'actual': [
                f'{female}，{male}就在旁边。你俩到底谁厉害？敢不敢比划两下，输的当面认怂？',
                '我没起哄，他就是想和你打架',
                '他真的想和你打架',
            ],
            'paraphrase': [
                f'{female}，{male}说你就是嘴上厉害，真较量你不敢。',
                '他刚才还说要让你两下，你能忍这口气？',
                '人就在这儿，你俩谁服谁，当面说清楚啊。',
            ],
            'colloquial': [
                f'{female}，他说你怂。',
                '他说你也就嘴上厉害。',
                '你问他啊，他人不就在这儿。',
            ],
            'greeting': [f'{female}，早啊，今天出来散步？'],
            'apology': ['那我送货去了，你们慢慢聊。'],
        }[self.scenario]

    def context(self, key, call_number, player_turn):
        evidence = copy.deepcopy(self.history[key][-32:])
        candidates = [
            {'action': action, 'target_id': '', 'duration': 5 if action == 'Pace' else 4,
             'evidence_ids': []}
            for action in ('Wait', 'LieDown', 'Pace')
        ]
        # 与日志中的十三项候选保持相同构成，不额外提供玩家期望的动作
        for target in (self.neighbor(key), 'player'):
            fact = next(item for item in reversed(evidence) if item['subject_id'] == self.ids[target])
            for action in ('LookAt', 'StrikeOnce', 'MoveAside', 'Fight', 'Flee'):
                candidates.append({'action': action, 'target_id': self.ids[target],
                                   'duration': 4 if action == 'LookAt' else 5,
                                   'evidence_ids': [fact['event_id']]})
        row = self.profiles[key]
        context = {
            'world_epoch': self.identity('world'), 'npc_id': self.ids[key],
            'request_id': self.identity(f'request/{call_number}/{key}'),
            'protocol_version': 1, 'capability_version': 1,
            'context_revision': self.serial, 'plan_revision': call_number,
            'request_source': 'player' if player_turn else 'ambient',
            'voice': row.get('tts_voice', 'male'), 'profile_id': key,
            'persona': row['persona'], 'self_name': row['display_name'],
            'self_subject_id': self.ids[key],
            'subjects': [{'subject_id': self.ids[person], 'name': self.name(person),
                          'kind': 'player' if person == 'player' else 'npc'}
                         for person in (key, self.neighbor(key), 'player')],
            'evidence': evidence, 'last_decision_event_ids': list(self.seen[key]),
            'candidates': candidates,
            'allowed_actions': list(dict.fromkeys(item['action'] for item in candidates)),
        }
        return validate_context(context)


def evaluate_reply(service, scene, speaker, player_round, followup, number, model, instructions):
    context = scene.context(speaker, number, player_turn=followup == 0)
    encoded = model_context(context)
    row = {'call': number, 'player_round': player_round, 'followup': followup,
           'speaker': speaker, 'name': scene.name(speaker), 'context': context,
           'model_context': encoded, 'speech': [], 'raw_output': '', 'valid': False}
    start = time.monotonic()

    def receive(kind, value):
        if kind == 'sentence':
            row.setdefault('first_sentence_ms', round((time.monotonic() - start) * 1000))
            row['speech'].append(value['text'])

    parser = SentenceParser(context, validate_action, receive)

    def delta(text):
        row['raw_output'] += text
        parser.feed(text)

    try:
        # 沿用正式模型适配器的超时与协议检查，没有语音或生产后端请求
        service.stream(json.dumps(encoded, ensure_ascii=False, separators=(',', ':')),
                       instructions, threading.Event(), delta, model=model)
        parser.finish()
        row.update(valid=True, proposal=parser.proposal)
        scene.seen[speaker] = [item['event_id'] for item in context['evidence']]
        proposal = parser.proposal
        # 没有明确收话人时，按当前提问者处理，与旧请求的兼容约定相同
        target = proposal.get('speech_target_id', scene.ids['player'] if followup == 0 else '')
        row['speech_target_id'] = target
        row['speech_target_name'] = next((scene.name(k) for k, v in scene.ids.items() if v == target), '')
        pending = encoded.get('pending_player_message')
        player_event = next((item for item in reversed(context['evidence'])
                             if item['kind'] == 'PlayerSpeech'), None)
        reply_to = player_event['event_id'] if pending and player_event else ''
        scene.npc_says(speaker, target, ''.join(row['speech']), reply_to)
    except Exception as error:
        row['error'] = {'type': type(error).__name__, 'message': str(error)}
    row['complete_ms'] = round((time.monotonic() - start) * 1000)
    row['token_usage'] = copy.deepcopy(service.last_token_usage)
    return row


def run(args):
    profiles_path = Path(args.profiles).resolve()
    profile_text = profiles_path.read_text(encoding='utf-8-sig')
    profiles = json.loads(profile_text)
    instructions = (Path(args.instructions).resolve().read_text(encoding='utf-8-sig')
                    if args.instructions else STREAM_INSTRUCTIONS)
    output = Path(args.output).resolve()
    if output.exists():
        raise FileExistsError(f'Review output already exists: {output}')
    output.parent.mkdir(parents=True, exist_ok=True)
    old_profile_path = dialogue_background.PROFILE_PATH
    dialogue_background.PROFILE_PATH = profiles_path
    dialogue_background._read_profiles.cache_clear()
    service = AppServer()
    report = {'kind': 'sequential_model_review', 'variant': args.variant, 'scenario': args.scenario,
              'model': args.model, 'started_utc': datetime.now(timezone.utc).isoformat(),
              'profiles_path': str(profiles_path), 'profiles': profiles,
              'profiles_sha256': hashlib.sha256(profile_text.encode('utf-8')).hexdigest(),
              'instructions_path': str(Path(args.instructions).resolve()) if args.instructions else 'STREAM_INSTRUCTIONS',
              'instructions': instructions,
              'instructions_sha256': hashlib.sha256(instructions.encode('utf-8')).hexdigest(),
              'limits': {'model_calls': args.max_calls, 'neighbor_replies_per_round': MAX_NEIGHBOR_REPLIES},
              'notice': '这是模型顺序对话模拟，不是 UE 动作测试；选择攻击不代表追到、命中、受击或多人同步。静态场景不模拟移动、播放等待、打断和事件过期。',
              'calls': [], 'attack_selected': None, 'status': 'running'}

    def save():
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    try:
        # 人物背景和示例必须与指定快照一致，不能混入正式配置中的新文字
        dialogue_background._read_profiles()
        scene = ConversationScene(profiles, args.scenario)
        report.update(player_lines=scene.player_lines(), initial_dialogue=scene.fixture,
                      initial_dialogue_note='actual 使用同一段实机开场；apology 预置已接受道歉的对照；其余场景没有预置对话')
        save()
        for player_round, text in enumerate(scene.player_lines(), 1):
            scene.player_says(text)
            speaker, stop_after_reply = 'xiumei', False
            for followup in range(MAX_NEIGHBOR_REPLIES + 1):
                if len(report['calls']) >= args.max_calls:
                    report['status'] = 'call_limit'
                    return report
                row = evaluate_reply(service, scene, speaker, player_round, followup,
                                     len(report['calls']) + 1, args.model, instructions)
                report['calls'].append(row)
                print(json.dumps({key: row.get(key) for key in
                                  ('call', 'player_round', 'followup', 'name', 'speech',
                                   'speech_target_name', 'proposal', 'valid', 'complete_ms', 'error')},
                                 ensure_ascii=False), flush=True)
                save()
                if not row['valid']:
                    report['status'] = 'model_error'
                    return report
                proposal = row['proposal']
                if proposal['action'] in ATTACKS:
                    report['attack_selected'] = {
                        'player_round': player_round, 'call': row['call'], 'speaker': speaker,
                        'action': proposal['action'], 'target_id': proposal['target_id'],
                        'target_name': next(scene.name(k) for k, v in scene.ids.items()
                                            if v == proposal['target_id']),
                    }
                    report['status'] = 'attack_selected'
                    return report
                if not row['speech']:
                    break
                other = scene.neighbor(speaker)
                addressing_neighbor = row['speech_target_id'] == scene.ids[other]
                # 连续两人只对玩家说话时结束；一旦转向彼此，仍按三轮上限接话
                if stop_after_reply and not addressing_neighbor:
                    break
                stop_after_reply = not addressing_neighbor
                speaker = other
        report['status'] = 'no_attack_selected'
        return report
    finally:
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
        try:
            service.close()
        finally:
            dialogue_background.PROFILE_PATH = old_profile_path
            dialogue_background._read_profiles.cache_clear()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profiles', default=str(ROOT / 'Design/AI/NpcProfiles.json'))
    parser.add_argument('--instructions')
    parser.add_argument('--output', required=True)
    parser.add_argument('--variant', required=True)
    parser.add_argument('--scenario', choices=('actual', 'paraphrase', 'colloquial', 'greeting', 'apology'), default='actual')
    parser.add_argument('--model', default='gpt-6.1-sol')
    parser.add_argument('--max-calls', type=int, default=MAX_CALLS, choices=range(1, MAX_CALLS + 1))
    args = parser.parse_args()
    report = run(args)
    print(json.dumps({'output': str(Path(args.output).resolve()), 'status': report['status'],
                      'calls': len(report['calls']), 'attack_selected': report['attack_selected']},
                     ensure_ascii=False), flush=True)
    if report['status'] in ('model_error', 'call_limit'):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
