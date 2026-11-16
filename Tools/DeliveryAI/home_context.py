"""家居候选只描述游戏确认的对象，选择不能创建物品、槽位或执行结果"""
import math
import uuid

from continued_activity import validate_speech_preference, validate_speech_preference_echo


HOME_ACTIONS = ('HomeAction',)
HOME_USES = ('Sit', 'PlayAudio', 'StopAudio', 'TakeProp', 'PlaceProp')
HOME_USES_V10 = HOME_USES + ('LeaveSeat', 'PlaceNearOwner')
ITEM_SOURCES = {'FetchItem': ('World',), 'ReturnItem': ('CompanionHand',),
                'TakeItem': ('OwnerHand', 'OwnerBackpack'), 'CollectCaterpillar': ('World',),
                'InsertCaterpillar': ('World', 'CompanionHand', 'OwnerHand', 'OwnerBackpack')}
TRAVEL_MODES = ('Walk', 'OwnVehicle', 'Automatic')
TRAVEL_ACTIONS = ('Follow', 'FetchPackage', 'TakePackage', 'DeliverPackage', 'ReturnPackage',
                  'DropPackage', 'StorePackageInBackpack', 'GoHome', 'FetchItem', 'ReturnItem', 'TakeItem')
MAX_HOME_OBJECTS = 24
MAX_HOME_SLOTS = 16
REQUEST_FIELDS = ('protocol_version', 'capability_version', 'world_epoch', 'npc_id',
                  'request_id', 'context_revision', 'plan_revision')

CANDIDATE_INSTRUCTIONS = """能力版本5及以后的每个候选都有本次请求内唯一的 candidate_id；完整动作JSON必须原样带回所选候选的 candidate_id，不能沿用上一轮编号或降低能力版本
如果本次接口要求 candidate_index，仍只填写所选候选的下标，接口会保留该候选的原始编号；不得另写物体、用途、目的地或执行结果
"""

HOME_INSTRUCTIONS = """能力版本5增加 HomeAction，是 duration=0、target_id为空的持续家居安排，不能当作五秒短动作
companion_context.home_objects 是游戏本轮看见或确认拿在手里的家居物件及其当前用途。玩家说出物件名称不会创建对象或槽位，执行仍以本轮 object_id 为准
只能选当前完整候选：Sit使用指定座位；PlayAudio/StopAudio打开或关闭指定音频物件；TakeProp拿起指定道具；PlaceProp把 object_id 指定的真实道具放到 destination_object_id 的 destination_slot_id，需要时会先取起。不同目的槽位是不同候选，不能混搭
只有当前主人直接提出的请求可安排 HomeAction；访客、旁听、环境事件和过去的主人台词不能授权新的家居动作。可以根据主人原话选择合适候选或解释当前做不到，不按关键词固定选择
主人点名的物件或目的地不在本轮完整候选里时，不拿别的物件或别的位置代替，也不说正在办；选一个不改变当前安排的候选，直接告诉主人现在没看到这件东西或这里放不了
物件状态写着爷爷还没搬走、房子还没到手时，屋里的东西都不能搬、不能放、不能用；主人提出家居安排就直接说明要先把爷爷催走、房子解锁后才能动，不说成没地方放或挪不动
每个家居候选带一句 home_summary，写明这个候选实际会拿哪件东西、放到哪里。主人要求搬放时，选 home_summary 里的物件和地点都与主人原话一致的那一个；台词里说的物件必须就是所选候选的物件。主人明确要求搬、放、拿而本轮有对应候选时，要选那个家居候选，不能只口头答应却选 Wait 或 LookAt
候选被接受只代表开始安排，实际到达、坐下、播放、拿取和摆放由后续游戏事件确认。被打断、物品占用、目标销毁或主人改变安排时以游戏报告为准，不能口头宣布已经完成
"""

HOME_MEMORY_INSTRUCTIONS = ('remembered_home_objects 只记得曾在共用房屋见到的物件名称和用途。'
                            '可以谈起印象，当前具体位置、开关、占用和可用动作以本轮游戏事实为准。\n')


def candidate_instructions(context):
    """能力版本 5～10 时返回候选说明，否则返回空字符串"""
    return CANDIDATE_INSTRUCTIONS if context.get('capability_version') in (5, 6, 7, 8, 9, 10) else ''


def _home_uses(context):
    """能力版本 10 用新的家居用途表，其他版本用旧表"""
    return HOME_USES_V10 if context.get('capability_version') == 10 else HOME_USES


def _valid_id(value):
    """value 是规范的非零 UUID 字符串"""
    try:
        return isinstance(value, str) and str(uuid.UUID(value)) == value and uuid.UUID(value).int != 0
    except (ValueError, AttributeError):
        return False


def _text(value, limit, *, empty=False):
    """value 是不超过 limit 字、没有控制字符的字符串；empty 为 False 时不能全是空白"""
    return (isinstance(value, str) and (empty or bool(value.strip())) and len(value) <= limit
            and not any(ord(char) < 32 for char in value))


def validate_home_objects(context):
    """返回本请求的对象表；不查询或修改 UE 世界，旧协议保持原样"""
    companion = context.get('companion_context')
    if isinstance(companion, dict) and 'remembered_home_objects' in companion:
        previous = companion['remembered_home_objects']
        if context.get('capability_version') not in (8, 9, 10) or not isinstance(previous, list) or len(previous) > 8:
            raise ValueError('invalid remembered home objects')
        for item in previous:
            if (not isinstance(item, dict) or set(item) != {'name', 'description'}
                    or not _text(item['name'], 64) or not _text(item['description'], 100, empty=True)):
                raise ValueError('invalid remembered home object')
    if context.get('capability_version') not in (5, 6, 7, 8, 9, 10) or 'companion_context' not in context:
        return {}
    if not isinstance(companion, dict):
        raise ValueError('invalid companion context')
    entries = companion.get('home_objects')
    if not isinstance(entries, list) or len(entries) > MAX_HOME_OBJECTS:
        raise ValueError('invalid companion home objects')
    objects = {}
    allowed_uses = _home_uses(context)
    for item in entries:
        if not isinstance(item, dict) or set(item) != {'object_id', 'name', 'state', 'uses', 'slots'}:
            raise ValueError('invalid home object')
        object_id = item['object_id']
        if not _valid_id(object_id) or object_id in objects:
            raise ValueError('invalid home object identity')
        if not _text(item['name'], 64) or not _text(item['state'], 256):
            raise ValueError('invalid home object description')
        uses = item['uses']
        if (not isinstance(uses, list) or len(uses) > len(allowed_uses)
                or any(not isinstance(use, str) or use not in allowed_uses for use in uses)
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
    """检查家居候选：请求人必须是当前主人，家具编号、用途和目的地都要在本次请求里"""
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
    if not isinstance(use, str) or use not in _home_uses(context) or use not in objects[object_id]['uses']:
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


def validate_candidate_travel(context, candidate):
    """未提供出行方式表示保留游戏当前安排，不能当作允许自行选择"""
    if 'travel_mode' not in candidate:
        return None
    mode = candidate['travel_mode']
    if (context.get('capability_version') not in (8, 9, 10) or candidate.get('action') not in TRAVEL_ACTIONS
            or not isinstance(mode, str) or mode not in TRAVEL_MODES):
        raise ValueError('invalid candidate travel mode')
    companion = context.get('companion_context', {})
    if (not isinstance(companion, dict) or companion.get('requester_is_owner') is not True
            or context.get('request_source', 'ambient') != 'player'):
        raise ValueError('travel mode requires current owner request')
    if mode == 'OwnVehicle' and companion.get('own_vehicle_available') is not True:
        raise ValueError('own vehicle is unavailable')
    return mode


def validate_candidate_item_fields(context, candidate):
    """检查候选的 item_source 和 package_task_id 字段，只有能力版本 10 的对应动作可以带"""
    action = candidate.get('action')
    source = candidate.get('item_source')
    if ('item_source' in candidate or action == 'TakeItem') and (
            context.get('capability_version') != 10 or source not in ITEM_SOURCES.get(action, ())):
        raise ValueError('invalid candidate item source')
    task_id = candidate.get('package_task_id')
    if action == 'InsertCaterpillar':
        task = context.get('companion_context', {}).get('task_id')
        if context.get('capability_version') != 10 or not _text(task_id, 64) or task_id != task:
            raise ValueError('invalid caterpillar candidate package task')
    elif 'package_task_id' in candidate:
        raise ValueError('candidate has no package task field')
    meaning = source
    if source is None and action in ITEM_SOURCES and len(ITEM_SOURCES[action]) == 1:
        meaning = ITEM_SOURCES[action][0]
    return meaning, task_id


def validate_item_choice_echo(offered, decision):
    """模型回复里的 item_source 和 package_task_id 必须和所选候选一致"""
    for key in ('item_source', 'package_task_id'):
        if key in decision and (key not in offered or decision[key] != offered[key]):
            raise ValueError('decision ' + key.replace('_', ' ') + ' differs from selected candidate')


def validate_v5_candidates(context):
    """候选编号仅在当前请求中解析，同一安排不能用多个编号重复提供"""
    if context.get('capability_version') not in (5, 6, 7, 8, 9, 10):
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
        travel_mode = validate_candidate_travel(context, candidate)
        preference = validate_speech_preference(context, candidate)
        item_fields = validate_candidate_item_fields(context, candidate)
        if action == 'HomeAction':
            meaning = (action, target, *_home_choice(context, candidate, objects))
        else:
            if 'home' in candidate:
                raise ValueError('non-home candidate has home choice')
            meaning = (action, target, travel_mode, preference, item_fields)
        if meaning in meanings:
            raise ValueError('duplicate candidate meaning')
        meanings.add(meaning)
        by_id[candidate_id] = candidate
    return by_id


def validate_v5_selection(context, decision, *, checking_candidate=False):
    """返回需要带给 UE 的编号，执行参数仍由 UE 保存的同一候选快照提供"""
    if context.get('capability_version') not in (5, 6, 7, 8, 9, 10):
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
    if 'travel_mode' in decision and ('travel_mode' not in offered or decision['travel_mode'] != offered['travel_mode']):
        raise ValueError('decision travel mode differs from selected candidate')
    validate_item_choice_echo(offered, decision)
    validate_speech_preference_echo(offered, decision)
    return {'candidate_id': candidate_id}
