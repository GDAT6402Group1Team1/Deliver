"""只更新两名 NPC 的人物性格，保存前核对人物表、蓝图和声音配置的其他内容"""
import copy
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path


TABLE_PATH = '/Game/NPC/Residents/Config/DT_NpcProfiles'
BLUEPRINTS = {'chunlai': '/Game/NPC/Residents/BP_ResidentLoud',
              'xiumei': '/Game/NPC/Residents/BP_ResidentShrewd'}
# 只接受这次读过的旧设定和已确认的新设定，遇到用户后来修改的文字就停止
OLD_PERSONA_HASHES = {'chunlai': 'fb3e5f758d9899676507f38f1a4a395f4a16e76104816c7331df1b23860e9a5c',
                      'xiumei': '9f5feb66271637fe81a5077d43b7516dbd4138f00836038454737cafd57fc2f5'}
NEW_PERSONA_HASHES = {'chunlai': '571b075b048750f71aa37f5f37fa5b667df579d22b157703a0e88e0fad309fd1',
                      'xiumei': 'a8c50296961501cc6cb1e1b0fecd6ba79b20e15559c26bfe1b7dc78089a86133'}


def text_hash(value):
    return hashlib.sha256(value.encode('utf-8')).hexdigest() if isinstance(value, str) else ''


def updated_rows(before, profiles):
    """从刚导出的表复制，只替换已确认的两段文字，其他行和字段原样保留"""
    result = copy.deepcopy(before)
    indexed = {row['Name']: row for row in result}
    if len(indexed) != len(result) or not set(BLUEPRINTS).issubset(indexed):
        raise RuntimeError('人物表存在重复行或缺少目标人物')
    for name in BLUEPRINTS:
        if text_hash(profiles[name]['persona']) != NEW_PERSONA_HASHES[name]:
            raise RuntimeError('JSON 人物设定已变化，请先检查：'+name)
        if text_hash(indexed[name].get('Persona')) not in (OLD_PERSONA_HASHES[name], NEW_PERSONA_HASHES[name]):
            raise RuntimeError('正式表中的人物设定已被修改，停止覆盖：'+name)
        indexed[name]['Persona'] = profiles[name]['persona']
    return result


def main():
    import unreal

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
        return {'display_name': str(npc.get_editor_property('display_name')),
                'persona': agent.get_editor_property('persona'),
                'tts_voice': str(presentation.get_editor_property('tts_voice')),
                'voice_set': object_path(voice), 'sounds': sound_settings,
                'table': object_path(binding.get_editor_property('data_table')),
                'row_name': str(binding.get_editor_property('row_name'))}

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
        if text_hash(settings['persona']) not in (OLD_PERSONA_HASHES[name], NEW_PERSONA_HASHES[name]):
            raise RuntimeError('蓝图中的人物设定已被修改，停止覆盖：'+name)
        updates.append((name, path, blueprint, settings))
    report = {'status': 'checked_before_update', 'table': TABLE_PATH, 'folder': str(output),
              'before': {name: settings for name, _, _, settings in updates}, 'after': {}, 'saved_assets': []}

    def save_report():
        (output/'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    save_report()
    table.modify()
    try:
        if not unreal.DataTableFunctionLibrary.fill_data_table_from_json_string(table, json.dumps(after, ensure_ascii=False), row_struct):
            raise RuntimeError('人物设定导入失败')
        verified = export_rows(table, output/'after.json')
        if {row['Name']: row for row in verified} != {row['Name']: row for row in after}:
            raise RuntimeError('人物表除 Persona 外发生了变化，停止保存')
        for name, path, blueprint, old_settings in updates:
            npc = unreal.get_default_object(unreal.EditorAssetLibrary.load_blueprint_class(path))
            blueprint.modify();npc.modify()
            if not npc.apply_profile():
                raise RuntimeError('人物设定无法应用到蓝图：'+name)
            unreal.BlueprintEditorLibrary.compile_blueprint(blueprint)
            current = unreal.get_default_object(unreal.EditorAssetLibrary.load_blueprint_class(path))
            settings = npc_settings(current)
            report['after'][name] = settings
            expected_settings = {**old_settings, 'persona': profiles[name]['persona']}
            if settings != expected_settings:
                raise RuntimeError('蓝图姓名、音色、声音配置或表绑定发生变化，停止保存：'+name)
        # 编译蓝图后再核对整张表，避免编译期间的回调改动其他字段
        final_rows = export_rows(table, output/'after.json')
        if {row['Name']: row for row in final_rows} != {row['Name']: row for row in after}:
            raise RuntimeError('蓝图编译后人物表发生了意外变化')
    except Exception as error:
        unreal.DataTableFunctionLibrary.fill_data_table_from_json_string(table, json.dumps(before, ensure_ascii=False), row_struct)
        for name, path, _, old_settings in updates:
            current = unreal.get_default_object(unreal.EditorAssetLibrary.load_blueprint_class(path))
            current.get_editor_property('agent').set_editor_property('persona', old_settings['persona'])
        report.update(status='failed_no_assets_saved', error=str(error))
        save_report()
        raise

    report.update(status='checked_before_save', unchanged_other_fields=True)
    save_report()
    try:
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
