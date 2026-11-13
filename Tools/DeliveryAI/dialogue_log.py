"""记录每轮交谈的候选、现场事件和模型选择，供试玩后核对台词与动作

只在启动后端时设置 DELIVERY_AI_DIALOGUE_LOG 才写入；内容包含 NPC 台词和玩家原话，只保存在本机 Saved/Logs
"""
import copy
import hashlib
import json
import os
import threading
import time
from pathlib import Path

_lock = threading.Lock()
_path = Path(__file__).resolve().parents[2] / 'Saved' / 'Logs' / 'NpcDialogue.ndjson'


def enabled():
    return bool(os.environ.get('DELIVERY_AI_DIALOGUE_LOG'))


def record(request_id, source, model, original, context, proposal, sentences, error=None, instructions=None):
    """模型调用结束后写一行

    original 是游戏发来的请求，context 是发给模型的短编号版本；两者候选顺序相同，
    所以按下标把选中的动作换成模型看到的短编号，便于和事件对照
    """
    if not enabled():
        return
    chosen = None
    if proposal is not None:
        for index, item in enumerate(original.get('candidates', [])):
            if item.get('action') == proposal.get('action') and (item.get('target_id') or '') == (proposal.get('target_id') or ''):
                alias = context['candidates'][index]
                chosen = f"{alias.get('action')}:{alias.get('target_id') or '-'}"
                break
    row = {
        'time': time.strftime('%H:%M:%S'),
        'request_id': request_id,
        'source': source,
        'model': model,
        'persona': (context.get('persona') or '')[:12],
        'profile_id': original.get('profile_id', ''),
        'full_persona': context.get('persona') or '',
        # 保存模型实际看到的人物、背景和对话，避免复查时拿新配置代替当时的内容
        'model_context': copy.deepcopy(context),
        'pending_player_message': (context.get('pending_player_message') or {}).get('description'),
        'new_event_ids': context.get('new_event_ids', []),
        'evidence': [{'id': e.get('event_id'), 'who': e.get('subject_id'), 'kind': e.get('kind'),
                      'text': e.get('description')} for e in context.get('evidence', [])],
        'candidates': [f"{c.get('action')}:{c.get('target_id') or '-'}" for c in context.get('candidates', [])],
        'chosen': chosen,
        'emotion': None if proposal is None else proposal.get('emotion'),
        'speech': sentences,
        'error': error,
    }
    if instructions is not None:
        row['instructions_sha256'] = hashlib.sha256(instructions.encode('utf-8')).hexdigest()
    line = json.dumps(row, ensure_ascii=False)
    with _lock:
        _path.parent.mkdir(parents=True, exist_ok=True)
        with _path.open('a', encoding='utf-8') as file:
            file.write(line + '\n')
