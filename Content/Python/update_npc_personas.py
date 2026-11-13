"""更新两名 NPC 的姓名与人物介绍，保存前核对声音、动作和表绑定"""
import copy
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


TABLE_PATH = '/Game/NPC/Residents/Config/DT_NpcProfiles'
BLUEPRINTS = {'chunlai': '/Game/NPC/Residents/BP_ResidentLoud',
              'xiumei': '/Game/NPC/Residents/BP_ResidentShrewd'}
# 只接受这次读过的旧设定和已确认的新设定，遇到用户后来修改的文字就停止
OLD_PERSONA_HASHES = {'chunlai': '571b075b048750f71aa37f5f37fa5b667df579d22b157703a0e88e0fad309fd1', 'xiumei': 'a8c50296961501cc6cb1e1b0fecd6ba79b20e15559c26bfe1b7dc78089a86133'}
PREVIOUS_PERSONA_HASHES = {'chunlai': 'bbd1a685727bc0c074c04e55026e22064c49dd0a0e4bdf6d21a0ce53e16d69ed', 'xiumei': 'ad4ae12a088b67376dcff6976151f1870da9969cf9cd18aa9e50c771a0cf6014'}
PLAYTEST_PERSONA_HASHES = {'chunlai': '751bbb6afbc478d1a2c4c4a409f7f941b4b6074912e01f291d4d12413c464645', 'xiumei': 'b8ddc8ef6aa3893b23ed8264c9874eb9723ccab3c463bef43412bfeb88877f0d'}
CHALLENGE_PERSONA_HASHES = {'chunlai': '111073dc3beb7e3fbaec4ab2e41c1fbd4823ae81961d472874fbe704cac5b69a', 'xiumei': '892b1c5f038bb296c2173697decb60438d950c51d357fea8fc259eab532e4a8b'}
SAVED_CHALLENGE_PERSONA_HASHES = {'chunlai': 'f89c1f6c8b8ebd9643c4b5c4528c30fd001c445e302fa8c3c65fe489cc682dc6', 'xiumei': '7364db30830a59d37721ce3f3c7bab3c74f6e5a47656758f62ab467cdba5f3a8'}
NEW_PERSONA_HASHES = {'chunlai': '471a45019f45f82ea3f1e8b5a737e9190a2be59b552139cd54b8121ce6797d95', 'xiumei': 'd50c9a48808dd5809aa39790bf1f748acbe8290042c1f3e78fa4eccd634a7f86'}
DISPLAY_NAMES = {'chunlai': ('大军', '大壮'), 'xiumei': ('小芳', '翠花')}


def text_hash(value):
    return hashlib.sha256(value.encode('utf-8')).hexdigest() if isinstance(value, str) else ''


def checked_display_name(serialized, name):
    # 保留 FText 的命名空间和键，只修改玩家看见的名字
    quoted = r'"(?:\\.|[^"\\])*"'
    match = re.fullmatch(r'(NSLOCTEXT\(\s*'+quoted+r'\s*,\s*'+quoted+r'\s*,\s*)('+quoted+r')(\s*\))', serialized)
    if not match or json.loads(match[2]) not in DISPLAY_NAMES[name]:
        raise RuntimeError('正式表中的姓名已被修改，停止覆盖：'+name)
    return match[1]+json.dumps(DISPLAY_NAMES[name][1], ensure_ascii=False)+match[3]


def ensure_editor_closed():
    if sys.platform != 'win32':
        return
    command = "Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'UnrealEditor.exe' -and $_.CommandLine -match 'Delivery\\.uproject' } | Select-Object -ExpandProperty ProcessId"
    result = subprocess.run(['powershell.exe', '-NoProfile', '-Command', command],
                            capture_output=True, text=True, check=True)
    if result.stdout.strip():
        raise RuntimeError('Delivery 编辑器正在运行，停止写入人物资源')


def updated_rows(before, profiles):
    """从刚导出的表复制，只替换已确认的姓名和介绍，其他行和字段原样保留"""
    result = copy.deepcopy(before)
    indexed = {row['Name']: row for row in result}
    if len(indexed) != len(result) or not set(BLUEPRINTS).issubset(indexed):
        raise RuntimeError('人物表存在重复行或缺少目标人物')
    for name in BLUEPRINTS:
        if text_hash(profiles[name]['persona']) != NEW_PERSONA_HASHES[name]:
            raise RuntimeError('JSON 人物设定已变化，请先检查：'+name)
        if profiles[name]['display_name'] != DISPLAY_NAMES[name][1]:
            raise RuntimeError('JSON 人物姓名已变化，请先检查：'+name)
        if text_hash(indexed[name].get('Persona')) not in (OLD_PERSONA_HASHES[name], PREVIOUS_PERSONA_HASHES[name], PLAYTEST_PERSONA_HASHES[name], CHALLENGE_PERSONA_HASHES[name], SAVED_CHALLENGE_PERSONA_HASHES[name], NEW_PERSONA_HASHES[name]):
            raise RuntimeError('正式表中的人物设定已被修改，停止覆盖：'+name)
        indexed[name]['DisplayName'] = checked_display_name(indexed[name].get('DisplayName', ''), name)
        indexed[name]['Persona'] = profiles[name]['persona']
    return result


def main(verify_only=False):
    import unreal
    ensure_editor_closed()

    def object_path(value):
        return value.get_path_name() if value else None

    def npc_settings(npc):
        agent = npc.get_editor_property('agent')
        presentation = npc.get_editor_property('presentation')
        if not agent or not presentation:
            raise RuntimeError('人物组件不完整')
        voice = presentation.get_editor_property('voice_set')
        sound_settings = None
        if voice:
            sound_settings = {field: object_path(voice.get_editor_property(field))
                              for field in ('heard_player', 'question', 'displeased', 'laugh', 'surprised', 'hit')}
            sound_settings.update({field: voice.get_editor_property(field)
                                   for field in ('volume', 'pitch', 'max_duration', 'dialogue_volume')})
            attenuation = voice.get_editor_property('dialogue_attenuation')
            sound_settings['dialogue_attenuation'] = object_path(attenuation)
            sound_settings['attenuation_settings'] = attenuation.get_editor_property('attenuation').export_text() if attenuation else None
        binding = npc.get_editor_property('profile')
        mesh = npc.get_editor_property('mesh')
        unchanged = {'model': object_path(mesh.get_editor_property('skeletal_mesh_asset')),
                     'behavior_tree': object_path(npc.get_editor_property('behavior_tree')),
                     'expressions': object_path(presentation.get_editor_property('expressions'))}
        unchanged['animations'] = {field: object_path(presentation.get_editor_property(field)) for field in
                                   ('idle_animation', 'walk_animation', 'pace_animation', 'hurry_animation',
                                    'attack_animation', 'hit_animation', 'down_animation', 'talk_animation', 'get_up_animation', 'hold_animation')}
        unchanged['movement'] = {field: npc.get_editor_property(field) for field in
                                 ('walk_speed', 'pace_speed', 'angry_approach_speed', 'pace_distance', 'side_step_distance')}
        return {'display_name': str(npc.get_editor_property('display_name')),
                'persona': agent.get_editor_property('persona'),
                'tts_voice': str(presentation.get_editor_property('tts_voice')),
                'voice_set': object_path(voice), 'sounds': sound_settings,
                'table': object_path(binding.get_editor_property('data_table')),
                'row_name': str(binding.get_editor_property('row_name')), 'other_settings': unchanged}

    def export_rows(table, path):
        if not unreal.DataTableFunctionLibrary.export_data_table_to_json_file(table, str(path)):
            raise RuntimeError('人物表导出失败')
        rows = json.loads(path.read_text(encoding='utf-8-sig'))
        if not isinstance(rows, list) or len({row['Name'] for row in rows}) != len(rows):
            raise RuntimeError('人物表导出内容无效')
        return rows

    root = Path(unreal.Paths.project_dir()).resolve()
    source = json.loads((root/'Design/AI/NpcProfiles.json').read_text(encoding='utf-8'))
    profiles = {row['id']: row for row in source['characters']}
    if source.get('version') != 1 or len(source['characters']) != 2 or set(profiles) != set(BLUEPRINTS):
        raise RuntimeError('人物 JSON 结构已变化')
    if any(profiles[name]['blueprint'] != path for name, path in BLUEPRINTS.items()):
        raise RuntimeError('人物 JSON 的蓝图路径已变化')
    table = unreal.load_asset(TABLE_PATH)
    row_struct = unreal.load_object(None, '/Script/Delivery.DeliveryNpcProfileRow')
    if not isinstance(table, unreal.DataTable) or table.get_editor_property('row_struct') != row_struct:
        raise RuntimeError('缺少正式人物表或行类型不符')
    report_root = root/'Saved/AIResearch/NpcPersonaUpdate'
    output = report_root/datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output.mkdir(parents=True, exist_ok=False)
    before = export_rows(table, output/'before.json')
    after = updated_rows(before, profiles)
    (output/'expected.json').write_text(json.dumps(after, ensure_ascii=False, indent=2), encoding='utf-8')
    updates = []
    for name, path in BLUEPRINTS.items():
        blueprint = unreal.load_asset(path)
        cls = unreal.EditorAssetLibrary.load_blueprint_class(path)
        if not blueprint or not cls:
            raise RuntimeError('缺少正式人物蓝图：'+path)
        npc = unreal.get_default_object(cls)
        settings = npc_settings(npc)
        if settings['table'] != table.get_path_name() or settings['row_name'] != name:
            raise RuntimeError('蓝图人物表绑定已变化，停止修改：'+name)
        if text_hash(settings['persona']) not in (OLD_PERSONA_HASHES[name], PREVIOUS_PERSONA_HASHES[name], PLAYTEST_PERSONA_HASHES[name], CHALLENGE_PERSONA_HASHES[name], SAVED_CHALLENGE_PERSONA_HASHES[name], NEW_PERSONA_HASHES[name]):
            raise RuntimeError('蓝图中的人物设定已被修改，停止覆盖：'+name)
        if settings['display_name'] not in DISPLAY_NAMES[name]:
            raise RuntimeError('蓝图中的姓名已被修改，停止覆盖：'+name)
        updates.append((name, path, blueprint, settings))
    report = {'status': 'checked_before_update', 'table': TABLE_PATH, 'folder': str(output),
              'before': {name: settings for name, _, _, settings in updates}, 'after': {}, 'saved_assets': []}

    def save_report():
        (output/'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    if verify_only:
        saved = json.loads((report_root/'latest.json').read_text(encoding='utf-8'))
        expected_rows = json.loads((Path(saved['folder'])/'after.json').read_text(encoding='utf-8-sig'))
        if before != expected_rows or before != after or report['before'] != saved['after']:
            raise RuntimeError('重新打开资产后的姓名或其他配置不一致')
        report.update(status='read_back_verified', verified_saved_report=saved['folder'], unchanged_other_fields=True)
        save_report()
        (report_root/'readback-latest.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        unreal.log('NPC_PERSONAS_READBACK_VERIFIED '+str(output/'report.json'))
        return

    save_report()
    table.modify()
    try:
        if not unreal.DataTableFunctionLibrary.fill_data_table_from_json_string(table, json.dumps(after, ensure_ascii=False), row_struct):
            raise RuntimeError('人物设定导入失败')
        verified = export_rows(table, output/'after.json')
        if {row['Name']: row for row in verified} != {row['Name']: row for row in after}:
            raise RuntimeError('人物表除姓名和介绍外发生了变化，停止保存')
        for name, path, blueprint, old_settings in updates:
            npc = unreal.get_default_object(unreal.EditorAssetLibrary.load_blueprint_class(path))
            blueprint.modify();npc.modify()
            if not npc.apply_profile():
                raise RuntimeError('人物设定无法应用到蓝图：'+name)
            unreal.BlueprintEditorLibrary.compile_blueprint(blueprint)
            current = unreal.get_default_object(unreal.EditorAssetLibrary.load_blueprint_class(path))
            settings = npc_settings(current)
            report['after'][name] = settings
            expected_settings = {**old_settings, 'display_name': profiles[name]['display_name'], 'persona': profiles[name]['persona']}
            if settings != expected_settings:
                raise RuntimeError('蓝图除姓名和介绍外发生了变化，停止保存：'+name)
        # 编译蓝图后再核对整张表，避免编译期间的回调改动其他字段
        final_rows = export_rows(table, output/'after.json')
        if {row['Name']: row for row in final_rows} != {row['Name']: row for row in after}:
            raise RuntimeError('蓝图编译后人物表发生了意外变化')
    except Exception as error:
        unreal.DataTableFunctionLibrary.fill_data_table_from_json_string(table, json.dumps(before, ensure_ascii=False), row_struct)
        for name, path, _, old_settings in updates:
            current = unreal.get_default_object(unreal.EditorAssetLibrary.load_blueprint_class(path))
            current.get_editor_property('agent').set_editor_property('persona', old_settings['persona'])
            current.set_editor_property('display_name', unreal.Text(old_settings['display_name']))
        report.update(status='failed_no_assets_saved', error=str(error))
        save_report()
        raise

    report.update(status='checked_before_save', unchanged_other_fields=True)
    save_report()
    try:
        ensure_editor_closed()
        for asset in [table]+[entry[2] for entry in updates]:
            if not unreal.EditorAssetLibrary.save_loaded_asset(asset):
                raise RuntimeError('资产保存失败：'+asset.get_path_name())
            report['saved_assets'].append(asset.get_path_name())
            save_report()
    except Exception as error:
        report.update(status='save_incomplete', error=str(error))
        save_report()
        raise
    report['status'] = 'saved'
    save_report()
    (report_root/'latest.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    unreal.log('NPC_PERSONAS_SAVED '+str(output/'report.json'))


if __name__ == '__main__':
    main()
