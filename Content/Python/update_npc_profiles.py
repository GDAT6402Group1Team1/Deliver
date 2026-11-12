"""首次从 JSON 建立 NPC 配置表；之后保留表内编辑结果，只给蓝图绑定对应行"""
import json
from pathlib import Path
import unreal

root = Path(unreal.Paths.project_dir())
source = json.loads((root / 'Design/AI/NpcProfiles.json').read_text(encoding='utf-8'))
if source.get('version') != 1 or len(source.get('characters', [])) != 2:
    raise RuntimeError('Unsupported NPC profile source')
rows = source['characters']
seen = set()
for row in rows:
    if row['id'] in seen or not row['display_name'] or not 1 <= len(row['persona']) <= 1024 or row['tts_voice'] not in ('', 'male', 'female'):
        raise RuntimeError('Invalid NPC profile: ' + row['id'])
    seen.add(row['id'])

tools = unreal.AssetToolsHelpers.get_asset_tools()
voice_paths = {'chunlai': '/Game/NPC/Residents/Audio/DA_Voice_OldMan',
               'xiumei': '/Game/NPC/Residents/Audio/DA_Voice_OldWoman'}
voice_sets = {}
for row in rows:
    path = row.get('voice_set') or voice_paths[row['id']]
    asset = unreal.load_asset(path)
    if not asset:
        folder, name = path.rsplit('/', 1)
        factory = unreal.DataAssetFactory()
        factory.set_editor_property('data_asset_class', unreal.DeliveryNpcVoiceSet.static_class())
        asset = tools.create_asset(name, folder, unreal.DeliveryNpcVoiceSet, factory)
        if not asset or not unreal.EditorAssetLibrary.save_loaded_asset(asset):
            raise RuntimeError('Could not create NPC voice set: ' + path)
    if not isinstance(asset, unreal.DeliveryNpcVoiceSet):
        raise RuntimeError('Wrong voice set asset type: ' + path)
    # 已有短音保持原配置，未找到合适素材的类别继续留空
    voice_sets[row['id']] = asset

table_path = '/Game/NPC/Residents/Config/DT_NpcProfiles'
row_struct = unreal.load_object(None, '/Script/Delivery.DeliveryNpcProfileRow')
if not row_struct:
    raise RuntimeError('Build DeliveryEditor before importing NPC profiles')
table = unreal.load_asset(table_path)
if not table:
    folder, name = table_path.rsplit('/', 1)
    factory = unreal.DataTableFactory()
    factory.set_editor_property('struct', row_struct)
    table = tools.create_asset(name, folder, unreal.DataTable, factory)
    imported = [{'Name': row['id'], 'DisplayName': row['display_name'], 'Persona': row['persona'],
                 'TtsVoice': row['tts_voice'], 'VoiceSet': voice_sets[row['id']].get_path_name()} for row in rows]
    if not table or not unreal.DataTableFunctionLibrary.fill_data_table_from_json_string(
            table, json.dumps(imported, ensure_ascii=False), row_struct):
        raise RuntimeError('Could not import NPC profile rows')
    if not unreal.EditorAssetLibrary.save_loaded_asset(table):
        raise RuntimeError('Could not save NPC profile table')
elif not isinstance(table, unreal.DataTable) or table.get_editor_property('row_struct') != row_struct:
    raise RuntimeError('DT_NpcProfiles has the wrong row structure')

names = {str(name) for name in unreal.DataTableFunctionLibrary.get_data_table_row_names(table)}
if not seen.issubset(names):
    raise RuntimeError('DT_NpcProfiles is missing required rows: ' + ', '.join(sorted(seen - names)))

updates = []
for row in rows:
    blueprint = unreal.load_asset(row['blueprint'])
    cls = unreal.EditorAssetLibrary.load_blueprint_class(row['blueprint'])
    if not blueprint or not cls:
        # 新工程可以先建表，再用 CreateResidentAssets 创建居民蓝图
        unreal.log_warning('NPC_PROFILE_BLUEPRINT_MISSING ' + row['blueprint'])
        continue
    npc = unreal.get_default_object(cls)
    agent = npc.get_editor_property('agent')
    presentation = npc.get_editor_property('presentation')
    if not agent or not presentation:
        raise RuntimeError('NPC components missing: ' + row['blueprint'])
    updates.append((row, blueprint, npc))

for row, blueprint, npc in updates:
    handle = unreal.DataTableRowHandle()
    handle.set_editor_property('data_table', table)
    handle.set_editor_property('row_name', unreal.Name(row['id']))
    blueprint.modify(); npc.modify()
    npc.set_editor_property('profile', handle)
    if not npc.apply_profile():
        raise RuntimeError('Invalid NPC profile row: ' + row['id'])
    unreal.BlueprintEditorLibrary.compile_blueprint(blueprint)
    if not unreal.EditorAssetLibrary.save_loaded_asset(blueprint):
        raise RuntimeError('Could not save NPC profile: ' + row['blueprint'])
    unreal.log('NPC_PROFILE_BOUND ' + row['id'] + ' ' + table_path)
