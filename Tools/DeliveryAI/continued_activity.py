"""继续当前活动只复用游戏提供的快照，不创建技能、目标或执行结果"""
import math
import uuid


CONTINUE_INSTRUCTIONS = '''能力版本6的 ContinueCurrent 表示保留游戏当前活动，一边做原来的事一边回答；它不重新开始动作、不延长交战时间、不重发跑腿或家居安排
current_action 和 current_activity 描述现在仍在发生的事。只能在本轮候选提供 ContinueCurrent 时选择，candidate_id 沿用本次候选，duration 固定0、target_id为空、evidence_ids为空
根据玩家整句话、性格和现场决定继续还是选择别的可用动作；玩家开口或使用语音本身不表示要求停手、取消跑腿或改变主人安排
还有玩家等回应时，ContinueCurrent 必须带一句回应原话的自然台词；继续原活动本身不能冒充刚执行了一个新的回应动作
当前动作和说话对象分别判断。新动作仍只从候选中选，真实命中、交付、奖励和物品移动由游戏确认
'''


def continuation_instructions(context):
    return CONTINUE_INSTRUCTIONS if context.get('capability_version') in (6, 7, 8) else ''


def _id(value, empty=False):
    if empty and value == '':
        return True
    try:
        return isinstance(value, str) and str(uuid.UUID(value)) == value and uuid.UUID(value).int != 0
    except (ValueError, AttributeError):
        return False


def validate_current_activity(context, actions):
    """只检查消息结构，活动是否仍有效由 UE 核对保留的服务器快照"""
    current = context.get('current_action')
    activity = context.get('current_activity')
    if current is not None:
        if not isinstance(current, dict):
            raise ValueError('invalid current action')
        source = current.get('source')
        if source not in ('Model', 'GameplayRule', 'Companion'):
            raise ValueError('invalid current action source')
        if (current.get('action') not in actions or current.get('action') == 'ContinueCurrent'
                or not _id(current.get('target_id'), empty=True)
                or not _id(current.get('action_id'), empty=source == 'Companion')):
            raise ValueError('invalid current action identity')
        remaining = current.get('remaining_seconds')
        if type(remaining) not in (int, float) or not math.isfinite(remaining) or not 0 <= remaining <= 3600:
            raise ValueError('invalid current action time')
        revision = current.get('plan_revision')
        if type(revision) is not int or not 0 <= revision <= 2147483647:
            raise ValueError('invalid current action revision')
        if source == 'Companion' and activity is None:
            raise ValueError('missing companion activity')
    if activity is not None:
        if not isinstance(activity, dict) or activity.get('kind') != 'companion' or 'companion_context' not in context:
            raise ValueError('invalid current activity')
        for key in ('mode', 'errand_action', 'errand_phase'):
            if not isinstance(activity.get(key), str) or len(activity[key]) > 64:
                raise ValueError('invalid current activity description')
        if type(activity.get('has_home_arrangement')) is not bool:
            raise ValueError('invalid current activity home state')
    if 'ContinueCurrent' in context.get('allowed_actions', []) and current is None and activity is None:
        raise ValueError('continue requires current activity')


def validate_continuation(context, action, checking_candidate=False):
    if action.get('action') != 'ContinueCurrent':
        return
    if context.get('capability_version') not in (6, 7, 8):
        raise ValueError('continue requires capability version 6')
    if context.get('current_action') is None and context.get('current_activity') is None:
        raise ValueError('continue requires current activity')
    if (type(action.get('duration')) not in (int, float) or action['duration'] != 0
            or action.get('target_id') != '' or action.get('evidence_ids') != []):
        raise ValueError('continue cannot change activity parameters')
    if not checking_candidate and context.get('pending_player_message') is True:
        speech = action.get('speech')
        if not isinstance(speech, str) or not speech.strip():
            raise ValueError('continue requires a player reply')
