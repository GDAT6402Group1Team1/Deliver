"""按服务器提供的完整候选省略不可用技能说明，保留人物、对话与游戏事实规则

这是纯准备函数，当前没有接入正式请求；调用者须先完成服务器协议校验
"""
from dataclasses import dataclass
import hashlib
from companion_context import COMPANION_COMMANDS


# 保存已审阅的原段落，原提示改动后整次保留原文，避免误删新增规则
ACTION_DESCRIPTION_BLOCKS = (
    ('Wait 是原地停留，LookAt 是看向指定目标，MoveTo 是靠近指定目标；MoveTo 不能表示走开、后退或去任意地点\n', (
        ('Wait', 'Wait 是原地停留\n'),
        ('LookAt', 'LookAt 是看向指定目标\n'),
        ('MoveTo', 'MoveTo 是靠近指定目标；MoveTo 不能表示走开、后退或去任意地点\n'))),
    ('Pace 是在身旁慢踱一小段再回来，没有目标；可以在思考、等人或没话可接时选择，游戏会检查附近是否有可走的地方，总时长最多5秒\n', None),
    ('MoveAside 是你自己走到指定人物侧旁的空地给他让路，目标是被让路的人；它不能让对方走开、不能挪车，也不是靠近质问。是否让路、继续站着或踱步仍由你结合现场与性格选择\n', None),
    ('StrikeOnce 是靠近目标后挥拳一次，对 NPC 使用重拳，实际命中会将其击倒；对玩家沿用原来的伤害和受击规则。能否靠近与命中由游戏判断，动作可能失败\n', None),
    ('HoldVehicle 是拽车，ReleaseVehicle 是松手，LieDown 是躺下，GetUp 是起身；只有候选里出现的动作才能选\n', (
        ('HoldVehicle', 'HoldVehicle 是拽车\n'), ('ReleaseVehicle', 'ReleaseVehicle 是松手\n'),
        ('LieDown', 'LieDown 是躺下\n'), ('GetUp', 'GetUp 是起身\n'))),
    ('Fight 是追赶同一个目标并连续尝试出拳，每次按候选时长执行，范围1.2到8秒；对 NPC 使用轻拳，对玩家沿用原来的伤害和受击规则。行动中可以收到新的现场信息并再次决定：继续选同目标 Fight 会衔接当前交战，选 Flee、Wait 或其他动作则改变安排。出拳间会短暂收拳，轻拳受击后仍可继续；目标倒地、离得太远、路线受阻或整场时间到时停止，不保证命中或获胜。继续交战也要依据当前情况与性格，不能只因为上一轮选了 Fight 就照选\n', None),
    ('Flee 是你自己跑离 target_id 指定的人，按候选时长执行，范围1.2到5秒；游戏选择可达路线，拉开距离、时间到或路走不通就结束。它不保证逃脱，不控制对方；MoveTo 不能代替逃跑。对方正在逃跑时，可以选 Fight 短时追打，也可以停下，不自动继续上一轮交战\n', None),
)
KNOWN_ACTIONS = frozenset(('Wait', 'LookAt', 'MoveTo', 'Pace', 'MoveAside', 'StrikeOnce',
    'HoldVehicle', 'ReleaseVehicle', 'LieDown', 'GetUp', 'Fight', 'Flee') + COMPANION_COMMANDS)


@dataclass(frozen=True)
class ActionPromptSelection:
    """按请求裁剪后的动作说明，以及裁剪前后的长度和哈希"""
    instructions: str
    status: str
    candidate_actions: tuple
    removed_actions: tuple
    removed_characters: int
    removed_utf8_bytes: int
    source_sha256: str
    instructions_sha256: str


def select_action_instructions(instructions, allowed_actions, candidates):
    """只准备说明，不筛候选、不解释玩家原话、不选择动作，也不修改任何传入对象"""
    offered = ()

    def result(text, status, removed=()):
        """用裁剪后的文字 text 生成选择结果，记录删掉的动作和长度差"""
        return ActionPromptSelection(text, status, offered, removed, len(instructions)-len(text),
            len(instructions.encode('utf-8'))-len(text.encode('utf-8')),
            hashlib.sha256(instructions.encode('utf-8')).hexdigest(),
            hashlib.sha256(text.encode('utf-8')).hexdigest())

    if (not isinstance(allowed_actions, (list, tuple)) or not allowed_actions or
            any(not isinstance(action, str) or action not in KNOWN_ACTIONS for action in allowed_actions)):
        return result(instructions, 'invalid_or_unknown_capabilities')
    if not isinstance(candidates, list) or not candidates:
        return result(instructions, 'missing_candidates')
    if any(not isinstance(candidate, dict) or not isinstance(candidate.get('action'), str) or
           candidate['action'] not in allowed_actions for candidate in candidates):
        return result(instructions, 'candidate_capability_mismatch')
    offered = tuple(dict.fromkeys(candidate['action'] for candidate in candidates))
    if (instructions.count('动作只能从本轮候选列表中选择\n') != 1 or
            any(instructions.count(block) != 1 for block, _ in ACTION_DESCRIPTION_BLOCKS)):
        return result(instructions, 'action_descriptions_changed')
    selected, removed = instructions, []
    for block, parts in ACTION_DESCRIPTION_BLOCKS:
        entries = parts or ((block.split(' ', 1)[0], block),)
        missing = [action for action, _ in entries if action not in offered]
        if missing:
            replacement = ''.join(text for action, text in entries if action in offered)
            selected = selected.replace(block, replacement, 1)
            removed.extend(missing)
    return result(selected, 'selected' if removed else 'unchanged_all_described_actions_offered', tuple(removed))
