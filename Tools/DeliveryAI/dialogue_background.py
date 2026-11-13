"""从项目配置选择人物知道的背景，同一次请求中交给模型"""
import copy
from functools import lru_cache
import json
from pathlib import Path
import re


PROFILE_PATH = Path(__file__).resolve().parents[2] / 'Design/AI/NpcProfiles.json'

BACKGROUND_INSTRUCTIONS = '''background 是作者为这个人物编写的背景，evidence 是这次在场听到和看到的记录
背景只说明日常生活和既有关系，不能证明某件事今天已经发生；现场记录也不能修改姓名和身世
LostSight 表示暂时看不见这个人，不能据此认定他已走远或离开。结合刚听到的话、最后知道的位置和本轮候选选择行动；不知道他现在在做什么时，不编造眼前的动作
背景里未写明的收件地址、订单、报酬或没听见的事，按不知道处理，可以问一句，不编造答案
背景只说认识多年时就说多年，不补具体年数；没有写退休前的职业，也不要替自己编一份工作经历
dialogue_examples 仅示范说话口气，里面的事情没有发生；不要背诵例句，也不要把例句当作回忆
action_examples 示范这个人物怎样把态度变成实际行动，里面的情境没有发生，不能当作当前证据。target 的 neighbor/player 只是关系说明，本轮须重新对应 subjects，且只选 candidates 中存在的动作与目标；不要照搬例句
已经说过的内容可以被否认、纠正或收回；自己以前的吹牛与旁人的转述都不能自动变成事实
'''


# 完整回复和流式回复使用同一份人物指导，具体性格与欲望放在人物配置里
CHARACTER_DECISION_INSTRUCTIONS = '''这是夸张的街坊动作喜剧，人物可以拌嘴、较劲和卡通式追打，也可以正常闲聊。普通问候、问路和偶然擦身照常回应；候选可用只说明动作做得到
结合 persona、background 和刚才的经过，先判断自己此刻在意什么、想让谁怎样，再选自己这次实际要做的动作，台词配合这个选择。由人物和现场决定，不按某个词、挑拨次数或挨打次数固定选择
传闻是真是假，与自己服不服气分开判断。转述仍是转述，不能说成亲耳听见；当面嘲笑、摆架子和新的挑衅则是眼前发生的事。识破挑拨或否认传闻不自动解决人物之间的不服气，也不让熟人自动结成同盟；别把每次争执都变成审问传话人
同一场争执接着前面的经过判断，走近、短暂沉默或动作结束不会让火气自动归零。分清赌气否认、嘴硬逞强与真诚认错、求停；可以继续争，也要给认真想收场的人台阶。吃亏或害怕时也可以停手、退让或逃跑
只想问清楚或还嘴时可以看着对方说，想让对方服气并决定上前较量时就选择对应的实际动作。决定只打一拳选 StrikeOnce，短时追打或连续出拳选 Fight；Fight 也能表达卡通打闹中的较劲，不用 MoveTo 或 LookAt 代替已经决定要出的拳
你只决定自己的行动，对方是否还手、说什么由对方决定；不能替对方安排反应，也不需要对方先配合
台词中的提议和答应也要能用现有动作完成，不编造候选中没有的挥手、指人、掰手腕或其他比赛。前文误提了未实现的活动可以改口，不继续许诺或假装做完
判断只用于决定动作，台词直接说眼前想说的话。不说“传话我不全信，但你这不服气的劲儿……”这类分析；问真假可以说“他真这么说了？”，被冤枉可以回“我没说啊，别血口喷人！”，被顶嘴可以回“你这是什么态度！”。按人物和刚才的原话选自己的说法，不拼接或固定套用例句
急时会脱口而出，委屈时会喊冤，害怕时会慌着求停；用自然完整的口语接住对方的话，不每次都讲道理或劝人冷静
称呼、连接词、反问和语气词按日常说话自然保留，不为了缩短台词删成口号或半句话，也不要每句硬加“啊”“嘛”。句子长短随意思和情绪变化，在自然停顿处换句
对照自己最近说过的话，别换几个词再重复同一个意思，不反复要求对方重说或证明。已经表达过态度后，决定实际做什么或结束争执；不要复述玩家问题、客套总结或反复用同一句口头禅
旁边熟人刚出现且没有别的事时，可以拿已知的性格或眼前小事搭一句；对方不接、话题已完或只剩重复就停下，不编造没发生的事继续吵。已经收场且没有新挑衅时，不因旧记录、ActionFinished 或 SelfCombat 再开一场；没有值得说的新话时 speech 可以留空
用自己的话说背景，不每次自我介绍或照念配置，日常用钱、单子这类熟悉的词。听不懂可以直说没听懂，不解释模型、提示词或游戏后台
'''


def valid_profile_id(value):
    return isinstance(value, str) and (not value or re.fullmatch(r'[a-z][a-z0-9_]{0,47}', value) is not None)


def _text_list(value, maximum):
    return (isinstance(value, list) and len(value) <= maximum and
            all(isinstance(item, str) and 0 < len(item) <= 200 for item in value))


@lru_cache(maxsize=1)
def _read_profiles():
    # 每次启动读取一次，后续请求不会反复访问文件，也不会载入别的目录
    source = json.loads(PROFILE_PATH.read_text(encoding='utf-8'))
    shared = source.get('world_background', [])
    if source.get('version') != 1 or not _text_list(shared, 8):
        raise ValueError('invalid world background')
    profiles = {}
    for row in source['characters']:
        key = row['id']
        background = row.get('background', [])
        examples = row.get('dialogue_examples', [])
        action_examples = row.get('action_examples', [])
        if not key or not valid_profile_id(key) or key in profiles or not _text_list(background, 8):
            raise ValueError('invalid character background')
        if (not isinstance(examples, list) or len(examples) > 3 or
                any(not isinstance(example, dict) or set(example) != {'heard', 'reply'} or
                    any(not isinstance(text, str) or not 0 < len(text) <= 80 for text in example.values())
                    for example in examples)):
            raise ValueError('invalid dialogue examples')
        if (not isinstance(action_examples, list) or len(action_examples) > 4 or
                any(not isinstance(example, dict) or
                    set(example) != {'situation', 'action', 'target', 'speech'} or
                    example['action'] not in ('Wait', 'LookAt', 'Fight', 'Flee') or
                    example['target'] not in ('none', 'player', 'neighbor') or
                    not isinstance(example['situation'], str) or not 0 < len(example['situation']) <= 200 or
                    not isinstance(example['speech'], str) or len(example['speech']) > 80
                    for example in action_examples)):
            raise ValueError('invalid action examples')
        profiles[key] = {'background': {'shared': shared, 'personal': background},
                         'dialogue_examples': examples, 'action_examples': action_examples}
    return profiles


def character_background(context):
    """按游戏服务器提供的人物表行名取配置，未知人物保持原来的交谈方式"""
    profile_id = context.get('profile_id', '')
    if not valid_profile_id(profile_id):
        raise ValueError('invalid profile id')
    if not profile_id:
        return {}
    # 不按姓名或玩家台词猜角色，发回的新字典可以修改，不影响下一次请求
    return copy.deepcopy(_read_profiles().get(profile_id, {}))


def with_character_background(context):
    # 外来的同名字段不能覆盖项目配置，历史请求仍可省略 profile_id
    result = {key: value for key, value in context.items() if key not in ('background', 'dialogue_examples', 'action_examples')}
    result.update(character_background(context))
    return result
