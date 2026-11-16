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
    """设置了环境变量 DELIVERY_AI_DIALOGUE_LOG 时记录对话日志"""
    return bool(os.environ.get('DELIVERY_AI_DIALOGUE_LOG'))


def record(request_id, source, model, original, context, proposal, sentences, error=None, instructions=None):
    """模型调用结束后写一行

    完整对话使用短编号候选，战斗小模型只接收动作名和 opponent
    日志按各自格式记录，不修改游戏请求或模型输入
    """
    if not enabled():
        return
    candidates = context.get('candidates', [])
    combat = bool(candidates) and all(isinstance(item, str) for item in candidates)
    chosen = None
    chosen_candidate_id = None
    chosen_travel_mode = None
    chosen_resolution = 'no_proposal' if proposal is None else 'unresolved_no_match'
    if combat:
        target = context.get('current_action', {}).get('target') or ''
        labels = {action: f"{action}:{'-' if action == 'Wait' else target or '-'}" for action in candidates}
        if proposal is not None:
            expected = '' if proposal.get('action') == 'Wait' else original.get('current_action', {}).get('target_id')
            if (proposal.get('target_id') or '') == expected:
                chosen = labels.get(proposal.get('action'))
                if chosen is not None:
                    chosen_resolution = 'resolved_combat'
        candidate_labels = list(labels.values())
    else:
        candidate_labels = [f"{item.get('action')}:{item.get('target_id') or '-'}" for item in candidates]
    if proposal is not None and not combat:
        # 出行变体共用动作和目标，日志按实际候选身份匹配，不能拿第一个变体代替
        candidate_id = proposal.get('candidate_id')
        matches = [(index, item) for index, item in enumerate(original.get('candidates', []))
                   if (not candidate_id or item.get('candidate_id') == candidate_id)
                   and item.get('action') == proposal.get('action')
                   and (item.get('target_id') or '') == (proposal.get('target_id') or '')
                   and ('travel_mode' not in proposal or item.get('travel_mode') == proposal['travel_mode'])]
        if len(matches) > 1:
            chosen_resolution = 'unresolved_ambiguous_candidates'
        elif len(matches) == 1:
            index, selected = matches[0]
            selected_id = selected.get('candidate_id')
            # 保留模型短目标名；有编号时也按编号找到模型看到的同一候选
            visible = [(i, item) for i, item in enumerate(candidates)
                       if selected_id and item.get('candidate_id') == selected_id]
            if selected_id and any(item.get('candidate_id') for item in candidates):
                index = visible[0][0] if len(visible) == 1 else -1
            if (0 <= index < len(candidate_labels)
                    and candidates[index].get('action') == selected.get('action')
                    and candidates[index].get('travel_mode') == selected.get('travel_mode')):
                chosen = candidate_labels[index]
                chosen_candidate_id = selected_id
                chosen_travel_mode = selected.get('travel_mode')
                chosen_resolution = 'resolved_candidate_id' if candidate_id else 'resolved_unique_fields'
            else:
                chosen_resolution = 'unresolved_context_candidate'
    pending = context.get('pending_player_message', original.get('pending_player_message'))
    evidence = context.get('events', []) if combat else context.get('evidence', [])
    original_events = original.get('evidence', [])
    recorded_evidence = []
    for index, event in enumerate(evidence):
        # 战斗输入省略事件编号，但事件顺序不变，编号仍从游戏原请求保留
        event_id = (original_events[index].get('event_id') if index < len(original_events) else None) if combat else event.get('event_id')
        recorded_evidence.append({'id': event_id, 'who': event.get('subject' if combat else 'subject_id'),
                                  'kind': event.get('kind'), 'text': event.get('description')})
    row = {
        'time': time.strftime('%H:%M:%S'),
        'request_id': request_id,
        'source': source,
        'model': model,
        'npc_id': original.get('npc_id'),
        'world_epoch': original.get('world_epoch'),
        'current_action': copy.deepcopy(original.get('current_action')),
        'persona': (context.get('persona') or '')[:12],
        'profile_id': original.get('profile_id', ''),
        'full_persona': context.get('persona') or '',
        # 保存模型实际看到的人物、背景和对话，避免复查时拿新配置代替当时的内容
        'model_context': copy.deepcopy(context),
        'pending_player_message': pending.get('description') if isinstance(pending, dict) else pending,
        'new_event_ids': context.get('new_event_ids', []),
        'evidence': recorded_evidence,
        'candidates': candidate_labels,
        'chosen': chosen,
        'chosen_candidate_id': chosen_candidate_id,
        'chosen_travel_mode': chosen_travel_mode,
        'chosen_resolution': chosen_resolution,
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
