"""让本地模型决定正在进行的交战是否继续，游戏仍检查实际动作能否执行"""
from copy import deepcopy
from dataclasses import dataclass
import http.client
import ipaddress
import json
import math
import os
import re
import socket
import threading
import time
from urllib.parse import urlsplit

from dialogue_background import character_background


COMBAT_INSTRUCTIONS = '''你在送快递游戏中控制一位正在交战或逃跑的街坊。根据性格、最近对话、实际受击和现场情况，决定接下来继续交战、逃跑或停手。
Fight 表示继续追近当前对手并出拳；Flee 表示继续或开始远离当前对手；Wait 表示停手。只选择 candidates 中已有的动作，不换对手，不生成台词。
当前行动已经开始，更新时不用再等一次挑衅。轻微擦碰不等于吃亏；明显疼痛、追不上、对方认输或双方已说开都应结合具体情况判断。别按打了几拳、对话几轮固定决定。记录里没有的命中、胜负和意图不能自行补充。
只输出一个 JSON 对象，例如 {"action":"Wait","target_id":""}。Fight/Flee 的 target_id 必须是 opponent，Wait 必须为空字符串。action 必须来自本次 candidates，不输出说明。'''
SUPPORTED_ACTIONS = frozenset(('Fight', 'Flee', 'Wait'))
RESIDENT_CACHE_SLOTS = {'chunlai': 0, 'xiumei': 1}
ACTION_PREFIX = '{"action":"'
ACTION_TOKEN_LIMIT = 6
_slot = threading.BoundedSemaphore(1)


@dataclass(frozen=True)
class CombatDecisionResult:
    proposal: dict | None = None
    fallback_reason: str = ''


def _event_description(event, own, opponent, current):
    """当前动作已有独立字段，战况文字只保留实际挥拳、命中和身体情况"""
    description = event.get('description', '')
    if (event.get('kind') != 'SelfCombat' or event.get('subject_id') != own
            or not isinstance(description, str)):
        return description
    target_name = opponent.get('name', '') if opponent.get('kind') == 'npc' else '对方'
    prefix = f'；我正在对{target_name}执行{current["action"]}，剩余约'
    endings = ('追近、出拳和受击仍在继续；可决定继续、逃跑或停手',
               '当前动作还未结束；下一步可以继续、逃跑或停手')
    # 旧版本在逃跑时也追加“受击仍在继续”，这里只移除服务器生成的完整尾段
    # 人物说的话、真实命中事件和身体状态都保留，动作仍由模型选择
    suffix = (re.escape(prefix) + r'\d+(?:\.\d+)?秒。(?:'
              + '|'.join(re.escape(ending) for ending in endings) + r')$')
    return re.sub(suffix, '', description)


def prepare_combat_context(context):
    """只接收已开始的模型动作；返回模型输入和原候选，失败时说明原因"""
    current = context.get('current_action')
    if context.get('request_source') != 'ambient' or context.get('pending_player_message') is not False:
        raise ValueError('player_or_unknown_request')
    if (not isinstance(current, dict) or current.get('action') not in ('Fight', 'Flee')
            or current.get('source') != 'Model' or not current.get('action_id')):
        raise ValueError('not_running_model_combat')
    remaining = current.get('remaining_seconds')
    if type(remaining) not in (int, float) or not math.isfinite(remaining) or remaining <= 0:
        raise ValueError('action_already_ended')
    target = current.get('target_id')
    subjects = context.get('subjects', [])
    if not target or not any(p.get('subject_id') == target and p.get('kind') in ('npc', 'player') for p in subjects):
        raise ValueError('unknown_opponent')
    opponent = next(p for p in subjects if p.get('subject_id') == target)
    selected = {}
    for candidate in context.get('candidates', []):
        action = candidate.get('action')
        if action not in SUPPORTED_ACTIONS:
            continue
        if candidate.get('target_id') != ('' if action == 'Wait' else target):
            continue
        if action in selected:
            raise ValueError('ambiguous_candidate')
        selected[action] = deepcopy(candidate)
    if 'Wait' not in selected or not any(a in selected for a in ('Fight', 'Flee')):
        raise ValueError('missing_combat_choices')
    # 名字和性格放在稳定前缀，人物编号仅在本次请求里替换，不交给模型拼写 UUID
    own = context.get('self_subject_id', '')
    aliases = {own: 'self', target: 'opponent', '': ''}
    for person in subjects:
        if person.get('subject_id') not in aliases:
            aliases[person['subject_id']] = 'person_' + str(len(aliases) - 2)
    def alias(value):
        return aliases.get(value, 'unknown')
    events = []
    evidence = context.get('evidence', [])
    if len(evidence) > 32:
        raise ValueError('too_many_events')
    for event in evidence:
        events.append({'kind': event.get('kind', ''), 'subject': alias(event.get('subject_id', '')),
                       'addressee': alias(event.get('addressee_id', '')),
                       'description': _event_description(event, own, opponent, current)})
    prompt = {'self_name': context.get('self_name', ''), 'persona': context.get('persona', ''),
              'subjects': [{'id': alias(p['subject_id']), 'name': p.get('name', ''), 'kind': p.get('kind', '')}
                           for p in subjects],
              'current_action': {'action': current['action'], 'target': 'opponent',
                                 'remaining_seconds': round(remaining, 2)},
              'candidates': sorted(selected), 'events': events}
    return prompt, selected


def prepare_runtime_combat_context(context):
    """试玩和判定日志使用同一份人物配置，历史固定评测保留原来的输入"""
    prompt, candidates = prepare_combat_context(context)
    profile = character_background(context)
    for key in ('combat_preferences', 'relationships'):
        if key in profile:
            prompt[key] = profile[key]
    return prompt, candidates


def combat_schema(actions):
    return {'oneOf': [{'type': 'object', 'additionalProperties': False,
                        'properties': {'action': {'const': action},
                                       'target_id': {'const': '' if action == 'Wait' else 'opponent'}},
                        'required': ['action', 'target_id']} for action in sorted(actions)]}


def parse_combat_choice(text, candidates):
    value = json.loads(text)
    if (not isinstance(value, dict) or set(value) != {'action', 'target_id'} or value['action'] not in candidates
            or value['target_id'] != ('' if value['action'] == 'Wait' else 'opponent')):
        raise ValueError('invalid_combat_choice')
    return value['action']


def _endpoint(value):
    parsed = urlsplit(value)
    if parsed.scheme != 'http' or parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise ValueError('invalid_local_endpoint')
    # 小模型只连接本机，避免错误配置把游戏上下文发到未知地址
    if parsed.hostname != 'localhost' and not ipaddress.ip_address(parsed.hostname or '').is_loopback:
        raise ValueError('endpoint_not_loopback')
    if parsed.path not in ('', '/', '/v1/chat/completions'):
        raise ValueError('invalid_endpoint_path')
    return parsed.hostname, parsed.port or 8771


class LocalCombatClient:
    """一次调用共用一个截止时间；取消时关掉连接，让本地服务停止当前生成"""
    def __init__(self, endpoint, timeout_seconds=1.5, model='delivery-npc-qwen3', seed=None, temperature=0.,
                 *, compact_output=False):
        self.host, self.port = _endpoint(endpoint)
        if not .1 <= timeout_seconds <= 30:
            raise ValueError('invalid_timeout')
        self.timeout_seconds, self.model = timeout_seconds, model
        self.requires_drain = False
        self.last_output = ''
        self.last_usage = None
        self.seed = seed
        if type(temperature) not in (int, float) or not 0 <= temperature <= 1:
            raise ValueError('invalid_temperature')
        self.temperature = temperature
        self.compact_output = compact_output

    def _choose_compact(self, post, check, mark, messages, candidates, cache_slot):
        actions = sorted(candidates)
        if not actions or any(action not in SUPPORTED_ACTIONS for action in actions):
            raise ValueError('invalid_combat_choices')
        template = post('/apply-template', {'messages': messages, 'chat_template_kwargs': {'enable_thinking': False}})
        # 先补上训练时的 JSON 开头，模型只写动作名，目标交给现有候选表
        tokens = post('/tokenize', {'content': template['prompt'], 'add_special': True, 'parse_special': True})['tokens']
        prefix = post('/tokenize', {'content': ACTION_PREFIX, 'add_special': False})['tokens']
        tokens += prefix
        if len(tokens) + 64 > 4096:
            raise ValueError('context_exceeds_4096')
        grammar = 'root ::= ' + ' | '.join(json.dumps(action) for action in actions)
        payload = {'prompt': tokens, 'grammar': grammar, 'n_predict': ACTION_TOKEN_LIMIT, 'stream': False,
                   'temperature': self.temperature, 'top_p': .8, 'top_k': 20, 'min_p': 0, 'cache_prompt': True}
        if cache_slot is not None:
            payload['id_slot'] = cache_slot
        if self.seed is not None:
            payload['seed'] = self.seed
        mark('combat_prompt_ready')
        self.requires_drain = True
        result = post('/completion', payload)
        self.requires_drain = False
        check()
        action = result.get('content')
        self.last_usage = result.get('timings')
        if result.get('stop_type') != 'eos' or result.get('truncated') is not False:
            raise ValueError('incomplete_combat_choice')
        if not isinstance(action, str) or action not in actions:
            raise ValueError('invalid_combat_choice')
        self.last_output = json.dumps({'action': action, 'target_id': '' if action == 'Wait' else 'opponent'},
                                      separators=(',', ':'))
        mark('combat_model_done')
        return action

    def is_idle(self):
        connection = http.client.HTTPConnection(self.host, self.port, timeout=.5)
        try:
            connection.request('GET', '/slots')
            response = connection.getresponse()
            value = json.loads(response.read(65537))
            return (response.status == 200 and isinstance(value, list) and bool(value)
                    and all(isinstance(slot, dict) and slot.get('is_processing') is False for slot in value))
        except (OSError, ValueError, http.client.HTTPException):
            return False
        finally:
            connection.close()

    def choose(self, prompt, candidates, cancel, measure=None, *, cache_slot=None):
        self.last_output = ''
        self.last_usage = None
        if cache_slot is not None and (type(cache_slot) is not int or cache_slot not in RESIDENT_CACHE_SLOTS.values()):
            raise ValueError('invalid_combat_cache_slot')
        started = time.monotonic()
        deadline = started + self.timeout_seconds
        finished = threading.Event()
        current = [None]
        active_socket = [None]
        def check():
            if cancel.is_set():
                raise RuntimeError('cancelled')
            if time.monotonic() >= deadline:
                raise TimeoutError('combat_model_timeout')
        def mark(stage):
            if measure:
                try:
                    measure(stage, (time.monotonic() - started) * 1000)
                except Exception:
                    pass
        def close_connection():
            connection = current[0]
            if connection is not None:
                try:
                    if active_socket[0]:
                        active_socket[0].shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                connection.close()
        def watch_cancel():
            while not finished.wait(.02):
                if cancel.is_set() or time.monotonic() >= deadline:
                    close_connection()
                    return
        def post(path, body):
            check()
            connection = http.client.HTTPConnection(self.host, self.port, timeout=max(.01, deadline-time.monotonic()))
            current[0] = connection
            response = None
            try:
                connection.connect()
                # Connection: close 的响应会清空 connection.sock，保存原 socket 才能中断慢速响应体
                active_socket[0] = connection.sock
                check()
                encoded = json.dumps(body, ensure_ascii=False).encode('utf-8')
                connection.request('POST', path, encoded, {'Content-Type': 'application/json'})
                response = connection.getresponse()
                raw = response.read(131073)
                check()
                if response.status != 200:
                    raise ValueError('local_http_' + str(response.status))
                if len(raw) > 131072:
                    raise ValueError('local_response_too_large')
                return json.loads(raw)
            finally:
                if response is not None:
                    response.close()
                connection.close()
                current[0] = None
                active_socket[0] = None
        watcher = threading.Thread(target=watch_cancel, name='CombatModelCancel', daemon=True)
        watcher.start()
        try:
            messages = [{'role': 'system', 'content': COMBAT_INSTRUCTIONS},
                        {'role': 'user', 'content': json.dumps(prompt, ensure_ascii=False, separators=(',', ':'))}]
            if self.compact_output:
                return self._choose_compact(post, check, mark, messages, candidates, cache_slot)
            template = post('/apply-template', {'messages': messages, 'chat_template_kwargs': {'enable_thinking': False}})
            tokens = post('/tokenize', {'content': template['prompt'], 'add_special': True})['tokens']
            if len(tokens) + 64 > 4096:
                raise ValueError('context_exceeds_4096')
            mark('combat_prompt_ready')
            self.requires_drain = True
            payload = {
                'model': self.model, 'messages': messages, 'stream': False, 'temperature': self.temperature,
                'top_p': .8, 'top_k': 20, 'min_p': 0, 'max_tokens': 64, 'cache_prompt': True,
                'chat_template_kwargs': {'enable_thinking': False},
                'response_format': {'type': 'json_object', 'schema': combat_schema(candidates)}}
            if cache_slot is not None:
                payload['id_slot'] = cache_slot
            if self.seed is not None:
                payload['seed'] = self.seed
            result = post('/v1/chat/completions', payload)
            self.requires_drain = False
            check()
            choice = result['choices'][0]
            self.last_output = choice.get('message', {}).get('content', '')
            self.last_usage = result.get('timings') or result.get('usage')
            if choice.get('finish_reason') != 'stop':
                raise ValueError('incomplete_combat_choice')
            action = parse_combat_choice(self.last_output, candidates)
            mark('combat_model_done')
            return action
        except (OSError, http.client.HTTPException):
            check()
            raise
        finally:
            finished.set()
            close_connection()
            watcher.join(.1)


def _release_when_idle(client):
    """HTTP 已取消后仍可能有短暂推理；确认服务空闲才接受下一次本地决定"""
    def wait_for_slot():
        while not client.is_idle():
            time.sleep(.2)
        _slot.release()
    threading.Thread(target=wait_for_slot, name='CombatModelDrain', daemon=True).start()


def choose_combat_action(context, cancel, measure=None, *, client=None):
    """成功返回现有游戏协议；失败返回原因，取消后调用方应结束这次请求"""
    if cancel.is_set():
        return CombatDecisionResult(fallback_reason='cancelled')
    # 这份训练只覆盖两位街坊，同行角色和护主安排继续使用原来的模型
    if 'companion_context' in context:
        return CombatDecisionResult(fallback_reason='companion_not_in_training_scope')
    allowed_profiles = {value.strip() for value in os.environ.get(
        'DELIVERY_COMBAT_MODEL_PROFILE_IDS', 'chunlai,xiumei').split(',') if value.strip()}
    profile = context.get('profile_id')
    if profile not in allowed_profiles:
        return CombatDecisionResult(fallback_reason='profile_not_in_training_scope')
    try:
        prompt, candidates = prepare_runtime_combat_context(context)
    except (ValueError, TypeError, KeyError) as error:
        return CombatDecisionResult(fallback_reason=str(error))
    # 对方的话还没接时由对话模型回答，已经回应的消息仍可用于判断战况
    evidence = context.get('evidence', [])
    answered = {event.get('reply_to_event_id') for event in evidence
                if event.get('kind') in ('SelfSpeech', 'ActionReply', 'NpcMessageSent')}
    if any(event.get('kind') == 'NpcMessage' and event.get('needs_reply') is True
           and event.get('event_id') not in answered for event in evidence):
        return CombatDecisionResult(fallback_reason='pending_npc_reply')
    endpoint = os.environ.get('DELIVERY_COMBAT_MODEL_URL', '')
    if client is None and not endpoint:
        return CombatDecisionResult(fallback_reason='disabled')
    if not _slot.acquire(blocking=False):
        return CombatDecisionResult(fallback_reason='busy')
    try:
        options = {}
        if client is None:
            resident_cache = os.environ.get('DELIVERY_COMBAT_MODEL_RESIDENT_CACHE') == '1'
            if resident_cache:
                if profile not in RESIDENT_CACHE_SLOTS:
                    return CombatDecisionResult(fallback_reason='profile_not_in_training_scope')
                # 缓存按服务器登记的人物编号选择，改显示名也不会用到别人的缓存
                options['cache_slot'] = RESIDENT_CACHE_SLOTS[profile]
            client = LocalCombatClient(endpoint, float(os.environ.get('DELIVERY_COMBAT_MODEL_TIMEOUT_SECONDS', '1.5')),
                                       seed=20261114 if resident_cache else None,
                                       temperature=float(os.environ.get('DELIVERY_COMBAT_MODEL_TEMPERATURE', '0')),
                                       compact_output=resident_cache)
        action = client.choose(prompt, candidates, cancel, measure, **options)
        if cancel.is_set():
            return CombatDecisionResult(fallback_reason='cancelled')
        proposal = {**candidates[action], 'speech': '', 'emotion': 'Calm', 'speech_target_id': ''}
        # 延迟导入避免 server / streaming 初始化时循环导入，最终沿用同一套游戏协议检查
        from server import validate_action
        return CombatDecisionResult(proposal=validate_action(context, proposal))
    except Exception as error:
        if cancel.is_set():
            reason = 'cancelled'
        elif isinstance(error, TimeoutError):
            reason = 'timeout'
        else:
            reason = str(error) if isinstance(error, ValueError) else type(error).__name__
        return CombatDecisionResult(fallback_reason=reason)
    finally:
        if isinstance(client, LocalCombatClient) and client.requires_drain:
            _release_when_idle(client)
        else:
            _slot.release()
