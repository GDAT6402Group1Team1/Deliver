"""将当前配置表中的两名 NPC 改名为大军、小芳，保留已有性格调整与声音配置"""
import copy
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import unreal


def rename_persona(text):
    text = text.replace('你是邵春来，街口修电瓶车的春来师傅。', '你是大军，在街口修电瓶车。')
    text = text.replace('你是陶秀梅，开面店的秀梅姐。', '你是小芳，开面店。')
    for old, new in (('邵春来', '大军'), ('春来师傅', '大军'), ('陶秀梅', '小芳'), ('秀梅姐', '小芳')):
        text = text.replace(old, new)
    return text


def rename_display_name(serialized, display_name):
    # 保留 FText 的命名空间和键，只替换玩家看见的文字
    quoted = r'"(?:\\.|[^"\\])*"'
    match = re.fullmatch(r'(NSLOCTEXT\(\s*'+quoted+r'\s*,\s*'+quoted+r'\s*,\s*)'+quoted+r'(\s*\))', serialized)
    if not match:
        raise RuntimeError('Expected NSLOCTEXT for the existing NPC display name')
    return match[1]+json.dumps(display_name, ensure_ascii=False)+match[2]


def export_rows(table, path):
    if not unreal.DataTableFunctionLibrary.export_data_table_to_json_file(table, str(path)):
        raise RuntimeError('Could not export NPC profile table')
    rows = json.loads(path.read_text(encoding='utf-8-sig'))
    if not isinstance(rows, list) or len({row['Name'] for row in rows}) != len(rows):
        raise RuntimeError('Invalid or duplicate NPC profile rows')
    return rows


def npc_settings(npc):
    agent = npc.get_editor_property('agent')
    presentation = npc.get_editor_property('presentation')
    if not agent or not presentation:
        raise RuntimeError('NPC components are missing')
    voice = presentation.get_editor_property('voice_set')
    return {'display_name': str(npc.get_editor_property('display_name')),
            'persona': agent.get_editor_property('persona'),
            'tts_voice': str(presentation.get_editor_property('tts_voice')),
            'voice_set': voice.get_path_name() if voice else None}


def main():
    root = Path(unreal.Paths.project_dir()).resolve()
    source = json.loads((root/'Design/AI/NpcProfiles.json').read_text(encoding='utf-8'))
    expected = {'chunlai': ('大军', '/Game/NPC/Residents/BP_ResidentLoud'),
                'xiumei': ('小芳', '/Game/NPC/Residents/BP_ResidentShrewd')}
    profiles = {row['id']: row for row in source['characters']}
    if source.get('version') != 1 or set(profiles) != set(expected) or len(source['characters']) != 2:
        raise RuntimeError('Unexpected NPC profile source')
    for name, (display_name, blueprint_path) in expected.items():
        if profiles[name]['display_name'] != display_name or profiles[name]['blueprint'] != blueprint_path:
            raise RuntimeError('Unexpected NPC name or blueprint: '+name)

    table_path = '/Game/NPC/Residents/Config/DT_NpcProfiles'
    table = unreal.load_asset(table_path)
    row_struct = unreal.load_object(None, '/Script/Delivery.DeliveryNpcProfileRow')
    if not isinstance(table, unreal.DataTable) or table.get_editor_property('row_struct') != row_struct:
        raise RuntimeError('Existing DT_NpcProfiles is required')
    report_root = root/'Saved/AIResearch/NpcProfileRename'
    output = report_root/datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output.mkdir(parents=True, exist_ok=False)
    before = export_rows(table, output/'before.json')
    after = copy.deepcopy(before)
    indexed = {row['Name']: row for row in after}
    if not set(expected).issubset(indexed):
        raise RuntimeError('Existing NPC profile rows are missing')
    updates = []
    for name, (display_name, blueprint_path) in expected.items():
        row = indexed[name]
        if not isinstance(row.get('Persona'), str) or not row['Persona']:
            raise RuntimeError('NPC persona is missing: '+name)
        # 从当前表保留人物介绍，只替换姓名与原称呼
        row['DisplayName'] = rename_display_name(row['DisplayName'], display_name)
        row['Persona'] = rename_persona(row['Persona'])
        blueprint = unreal.load_asset(blueprint_path)
        cls = unreal.EditorAssetLibrary.load_blueprint_class(blueprint_path)
        if not blueprint or not cls:
            raise RuntimeError('Existing NPC blueprint is missing: '+blueprint_path)
        npc = unreal.get_default_object(cls)
        updates.append((name, blueprint_path, blueprint, npc, npc_settings(npc)))

    # 导入内容来自刚导出的正式表；核对每一行，防止覆盖用户在表内的其他调整
    table.modify()
    try:
        if not unreal.DataTableFunctionLibrary.fill_data_table_from_json_string(table, json.dumps(after, ensure_ascii=False), row_struct):
            raise RuntimeError('Could not update NPC names')
        verified = export_rows(table, output/'after.json')
        if {row['Name']: row for row in verified} != indexed:
            raise RuntimeError('NPC profile export changed unrelated data')
        results = []
        for name, path, blueprint, npc, old_settings in updates:
            handle = unreal.DataTableRowHandle()
            handle.set_editor_property('data_table', table)
            handle.set_editor_property('row_name', unreal.Name(name))
            blueprint.modify()
            npc.modify()
            npc.set_editor_property('profile', handle)
            if not npc.apply_profile():
                raise RuntimeError('Could not apply NPC profile: '+name)
            unreal.BlueprintEditorLibrary.compile_blueprint(blueprint)
            current = unreal.get_default_object(unreal.EditorAssetLibrary.load_blueprint_class(path))
            settings = npc_settings(current)
            if settings['display_name'] != expected[name][0] or settings['persona'] != indexed[name]['Persona']:
                raise RuntimeError('NPC blueprint did not keep the new name: '+name)
            if any(settings[field] != old_settings[field] for field in ('tts_voice', 'voice_set')):
                raise RuntimeError('NPC blueprint voice differs from its table; assets not saved: '+name)
            binding = current.get_editor_property('profile')
            if binding.get_editor_property('data_table') != table or str(binding.get_editor_property('row_name')) != name:
                raise RuntimeError('NPC profile binding changed: '+name)
            results.append({'row_id': name, 'blueprint': path, 'before': old_settings, 'after': settings})
    except Exception:
        # 核对失败时恢复内存中的表，正式资产尚未保存
        unreal.DataTableFunctionLibrary.fill_data_table_from_json_string(table, json.dumps(before, ensure_ascii=False), row_struct)
        raise

    if not unreal.EditorAssetLibrary.save_loaded_asset(table):
        raise RuntimeError('Could not save NPC profile table')
    for _, path, blueprint, _, _ in updates:
        if not unreal.EditorAssetLibrary.save_loaded_asset(blueprint):
            raise RuntimeError('Could not save NPC blueprint: '+path)
    report = {'status': 'saved', 'table': table_path, 'evidence_folder': str(output),
              'table_rows': len(verified), 'unchanged_other_fields': True, 'blueprints': results}
    text = json.dumps(report, ensure_ascii=False, indent=2)
    (output/'report.json').write_text(text, encoding='utf-8')
    (report_root/'latest.json').write_text(text, encoding='utf-8')
    unreal.log('NPC_PROFILE_NAMES_SAVED '+str(output/'report.json'))


if __name__ == '__main__':
    main()
