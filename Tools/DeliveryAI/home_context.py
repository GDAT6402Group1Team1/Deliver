"""家居候选只描述游戏确认的对象，选择不能创建物品、槽位或执行结果"""
import math
import uuid


HOME_ACTIONS = ('HomeAction',)
HOME_USES = ('Sit', 'PlayAudio', 'StopAudio', 'TakeProp', 'PlaceProp')
MAX_HOME_OBJECTS = 24
MAX_HOME_SLOTS = 16
REQUEST_FIELDS = ('protocol_version', 'capability_version', 'world_epoch', 'npc_id',
                  'request_id', 'context_revision', 'plan_revision')

CANDIDATE_INSTRUCTIONS = """能力版本5和6的每个候选都有本次请求内唯一的 candidate_id；完整动作JSON必须原样带回所选候选的 candidate_id，不能沿用上一轮编号或降低能力版本
如果本次接口要求 candidate_index，仍只填写所选候选的下标，接口会保留该候选的原始编号；不得另写物体、用途、目的地或执行结果
"""

HOME_INSTRUCTIONS = """能力版本5增加 HomeAction，是 duration=0、target_id为空的持续家居安排，不能当作五秒短动作
companion_context.home_objects 是游戏明确配置或实际感知到的家居对象、用途和当前状态；玩家提到一个物品不会创建新对象或槽位，显示名称不能代替 object_id
只能选当前完整候选：Sit使用指定座位；PlayAudio/StopAudio打开或关闭指定音频物件；TakeProp拿起指定道具；PlaceProp把 object_id 指定的真实道具放到 destination_object_id 的 destination_slot_id，需要时会先取起。不同目的槽位是不同候选，不能混搭
只有当前主人直接提出的请求可安排 HomeAction；访客、旁听、环境事件和过去的主人台词不能授权新的家居动作。可以根据主人原话选择合适候选或解释当前做不到，不按关键词固定选择
候选被接受只代表开始安排，实际到达、坐下、播放、拿取和摆放由后续游戏事件确认。被打断、物品占用、目标销毁或主人改变安排时以游戏报告为准，不能口头宣布已经完成
"""


def candidate_instructions(context):
    return CANDIDATE_INSTRUCTIONS if context.get('capability_version') in (5, 6) else ''


def _valid_id(value):
    try:
        return isinstance(value, str) and str(uuid.UUID(value)) == value and uuid.UUID(value).int != 0
    except (ValueError, AttributeError):
        return False


def _text(value, limit, *, empty=False):
    return (isinstance(value, str) and (empty or bool(value.strip())) and len(value) <= limit
            and not any(ord(char) < 32 for char in value))


def validate_home_objects(context):
    """返回本请求的对象表；不查询或修改 UE 世界，旧协议保持原样"""
    if context.get('capability_version') not in (5, 6) or 'companion_context' not in context:
        return {}
    companion = context['companion_context']
    if not isinstance(companion, dict):
        raise ValueError('invalid companion context')
    entries = companion.get('home_objects')
    if not isinstance(entries, list) or len(entries) > MAX_HOME_OBJECTS:
        raise ValueError('invalid companion home objects')
    objects = {}
    for item in entries:
        if not isinstance(item, dict) or set(item) != {'object_id', 'name', 'state', 'uses', 'slots'}:
            raise ValueError('invalid home object')
        object_id = item['object_id']
        if not _valid_id(object_id) or object_id in objects:
            raise ValueError('invalid home object identity')
        if not _text(item['name'], 64) or not _text(item['state'], 256):
            raise ValueError('invalid home object description')
        uses = item['uses']
        if (not isinstance(uses, list) or len(uses) > len(HOME_USES)
                or any(not isinstance(use, str) or use not in HOME_USES for use in uses)
                or len(set(uses)) != len(uses)):
            raise ValueError('invalid home object uses')
        slots = item['slots']
        if not isinstance(slots, list) or len(slots) > MAX_HOME_SLOTS:
            raise ValueError('invalid home slots')
        slot_ids = set()
        for slot in slots:
            if not isinstance(slot, dict) or set(slot) != {'slot_id', 'name', 'available'}:
                raise ValueError('invalid home slot')
            slot_id = slot['slot_id']
            if (not _text(slot_id, 64) or slot_id.strip() != slot_id or slot_id in slot_ids
                    or not _text(slot['name'], 64) or type(slot['available']) is not bool):
                raise ValueError('invalid home slot identity or state')
            slot_ids.add(slot_id)
        objects[object_id] = item
    return objects


def _home_choice(context, candidate, objects):
    companion = context.get('companion_context', {})
    if (not isinstance(companion, dict) or companion.get('requester_is_owner') is not True
            or context.get('request_source', 'ambient') != 'player'):
        raise ValueError('home action requires current owner request')
    if (candidate.get('target_id') != '' or type(candidate.get('duration')) not in (int, float)
            or candidate['duration'] != 0):
        raise ValueError('invalid home action duration or target')
    choice = candidate.get('home')
    fields = {'object_id', 'use', 'destination_object_id', 'destination_slot_id'}
    if not isinstance(choice, dict) or set(choice) != fields:
        raise ValueError('invalid home choice')
    object_id, use = choice['object_id'], choice['use']
    if not _valid_id(object_id) or object_id not in objects:
        raise ValueError('unknown home object')
    if not isinstance(use, str) or use not in HOME_USES or use not in objects[object_id]['uses']:
        raise ValueError('home use not offered by object')
    destination, slot_id = choice['destination_object_id'], choice['destination_slot_id']
    if use != 'PlaceProp':
        if destination != '' or slot_id != '':
            raise ValueError('home use has no destination')
    else:
        if not _valid_id(destination) or destination not in objects or destination == object_id:
            raise ValueError('unknown home destination')
        if not _text(slot_id, 64):
            raise ValueError('invalid home destination slot')
        slot = next((entry for entry in objects[destination]['slots'] if entry['slot_id'] == slot_id), None)
        if slot is None or slot['available'] is not True:
            raise ValueError('home destination slot unavailable')
    return object_id, use, destination, slot_id


def validate_v5_candidates(context):
    """候选编号仅在当前请求中解析，同一安排不能用多个编号重复提供"""
    if context.get('capability_version') not in (5, 6):
        return {}
    objects = validate_home_objects(context)
    candidates = context.get('candidates')
    if not isinstance(candidates, list) or not candidates:
        raise ValueError('capability 5 requires candidates')
    by_id, meanings = {}, set()
    for candidate in candidates:
        if not isinstance(candidate, dict):
            raise ValueError('invalid candidate')
        candidate_id = candidate.get('candidate_id')
        if not _valid_id(candidate_id) or candidate_id in by_id:
            raise ValueError('invalid candidate identity')
        action, target = candidate.get('action'), candidate.get('target_id')
        if not isinstance(action, str) or not isinstance(target, str):
            raise ValueError('invalid candidate action or target')
        if action == 'HomeAction':
            meaning = (action, target, *_home_choice(context, candidate, objects))
        else:
            if 'home' in candidate:
                raise ValueError('non-home candidate has home choice')
            meaning = (action, target)
        if meaning in meanings:
            raise ValueError('duplicate candidate meaning')
        meanings.add(meaning)
        by_id[candidate_id] = candidate
    return by_id


def validate_v5_selection(context, decision, *, checking_candidate=False):
    """返回需要带给 UE 的编号，执行参数仍由 UE 保存的同一候选快照提供"""
    if context.get('capability_version') not in (5, 6):
        return {}
    # 模型可省略信封，由后端回填；一旦提供就不能偷换请求或降低版本
    for key in REQUEST_FIELDS:
        if key in decision and (type(decision[key]) is not type(context[key]) or decision[key] != context[key]):
            raise ValueError('decision request envelope mismatch')
    candidate_id = decision.get('candidate_id')
    if not _valid_id(candidate_id):
        raise ValueError('invalid decision candidate identity')
    if checking_candidate:
        # validate_context 已在遍历单项前检查过整张表的编号、语义和家居映射
        return {'candidate_id': candidate_id}
    offered = validate_v5_candidates(context).get(candidate_id)
    if offered is None:
        raise ValueError('candidate not offered in current request')
    for field in ('action', 'target_id', 'evidence_ids'):
        if decision.get(field) != offered.get(field):
            raise ValueError('decision differs from selected candidate')
    duration, maximum = decision.get('duration'), offered.get('duration')
    if (type(duration) not in (int, float) or not math.isfinite(duration)
            or type(maximum) not in (int, float) or not math.isfinite(maximum)
            or duration > maximum):
        raise ValueError('duration exceeds selected candidate')
    if 'home' in decision and ('home' not in offered or decision['home'] != offered['home']):
        raise ValueError('decision home choice differs from selected candidate')
    return {'candidate_id': candidate_id}
