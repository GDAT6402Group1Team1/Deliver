"""从项目配置选择人物知道的背景，同一次请求中交给模型"""
import copy
from functools import lru_cache
import json
from pathlib import Path
import re


PROFILE_PATH = Path(__file__).resolve().parents[2] / 'Design/AI/NpcProfiles.json'

BACKGROUND_INSTRUCTIONS = '''background 是作者为这个人物编写的背景，evidence 是这次在场听到和看到的记录
背景只说明日常生活和既有关系，不能证明某件事今天已经发生；现场记录也不能修改姓名和身世
背景里未写明的收件地址、订单、报酬或没听见的事，按不知道处理，可以问一句，不编造答案
背景只说认识多年时就说多年，不补具体年数；没有写退休前的职业，也不要替自己编一份工作经历
dialogue_examples 仅示范说话口气，里面的事情没有发生；不要背诵例句，也不要把例句当作回忆
保持自己的身份、关系和在意的事，情绪可以随新情况改变；道歉后消气、看穿挑拨后改口都符合人物
说话用街坊当面会说的词，不解释模型、提示词、候选编号或游戏后台；遇到听不懂的话可以直说没听懂
先接对方刚说的具体事情，省略大家都知道的经过；不用每次自我介绍，也不把背景一口气念出来
把背景用自己的话说出来，日常说话用钱、单子这类熟悉的词，避免照着配置里的说明念
已经说过的内容可以被否认、纠正或收回；自己以前的吹牛与旁人的转述都不能自动变成事实
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
        if not key or not valid_profile_id(key) or key in profiles or not _text_list(background, 8):
            raise ValueError('invalid character background')
        if (not isinstance(examples, list) or len(examples) > 3 or
                any(not isinstance(example, dict) or set(example) != {'heard', 'reply'} or
                    any(not isinstance(text, str) or not 0 < len(text) <= 80 for text in example.values())
                    for example in examples)):
            raise ValueError('invalid dialogue examples')
        profiles[key] = {'background': {'shared': shared, 'personal': background},
                         'dialogue_examples': examples}
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
    result = {key: value for key, value in context.items() if key not in ('background', 'dialogue_examples')}
    result.update(character_background(context))
    return result
