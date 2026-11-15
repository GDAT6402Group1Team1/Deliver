"""按人物表编号读取 NPC 平日的交情，供对话和动作模型使用"""
from copy import deepcopy
from functools import lru_cache
import json
from pathlib import Path
import re


DESIGN_PATH = Path(__file__).resolve().parents[2] / 'Design/AI'
PROFILE_PATH = DESIGN_PATH / 'NpcProfiles.json'
RELATIONSHIPS_PATH = DESIGN_PATH / 'NpcRelationships.json'
PROFILE_ID = re.compile(r'[a-z][a-z0-9_]{0,47}')
TENDENCIES = ('familiarity', 'trust', 'rivalry', 'care')
EDGE_FIELDS = {*TENDENCIES, 'source_profile_id', 'other_profile_id', 'description'}


@lru_cache(maxsize=1)
def _read_relationships():
    profiles = json.loads(PROFILE_PATH.read_text(encoding='utf-8'))
    if not isinstance(profiles, dict) or profiles.get('version') != 1 or not isinstance(profiles.get('characters'), list):
        raise ValueError('人物关系需要版本 1 的 NPC 人物表')
    names = {}
    for profile in profiles['characters']:
        profile_id = profile.get('id') if isinstance(profile, dict) else None
        if not isinstance(profile_id, str) or not PROFILE_ID.fullmatch(profile_id) or profile_id in names:
            raise ValueError('NPC 人物表存在无效或重复编号')
        name = profile.get('display_name')
        if not isinstance(name, str) or not name.strip():
            raise ValueError(f'NPC 人物表缺少显示名 {profile_id}')
        names[profile_id] = name

    source = json.loads(RELATIONSHIPS_PATH.read_text(encoding='utf-8'))
    if (not isinstance(source, dict) or source.get('version') != 1 or not isinstance(source.get('edges'), list)
            or not isinstance(source.get('guidance'), str) or not source['guidance'].strip()):
        raise ValueError('NPC 关系表格式有误')
    relationships = {}
    seen = set()
    for edge in source['edges']:
        if not isinstance(edge, dict) or set(edge) != EDGE_FIELDS:
            raise ValueError('NPC 关系字段有误')
        own, other = edge['source_profile_id'], edge['other_profile_id']
        if not all(isinstance(profile_id, str) and profile_id in names for profile_id in (own, other)):
            raise ValueError('NPC 关系指向人物表中没有的人物')
        if own == other:
            raise ValueError(f'NPC 关系不能指向自己 {own}')
        if (own, other) in seen:
            raise ValueError(f'NPC 关系重复 {own} → {other}')
        if any(type(edge[key]) is not int or not 0 <= edge[key] <= 100 for key in TENDENCIES):
            raise ValueError(f'NPC 关系倾向需要 0 到 100 的整数 {own} → {other}')
        if not isinstance(edge['description'], str) or not edge['description'].strip():
            raise ValueError(f'NPC 关系缺少说明 {own} → {other}')
        seen.add((own, other))
        relationships.setdefault(own, {'guidance': source['guidance'], 'edges': []})['edges'].append({
            'profile_id': other, 'display_name': names[other], 'description': edge['description'],
            **{key: edge[key] for key in TENDENCIES},
        })
    return relationships


def prepare_relationships():
    """后端启动时读表并检查，之后查询沿用同一份配置"""
    _read_relationships()


def character_relationships(profile_id):
    """查这个 NPC 平日怎么看其他人，未配置关系时返回空列表"""
    if not isinstance(profile_id, str) or not profile_id:
        return []
    return deepcopy(_read_relationships().get(profile_id, []))
