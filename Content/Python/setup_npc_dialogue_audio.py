"""给大军和小芳配置完整台词音量与距离衰减，保留已有短反应音和人物表"""
import datetime
import json
import math
from pathlib import Path
import unreal


TABLE_PATH = '/Game/NPC/Residents/Config/DT_NpcProfiles'
ATTENUATION_PATH = '/Game/NPC/Residents/Audio/SA_NpcDialogue'
CHARACTERS = (
    ('chunlai', '/Game/NPC/Residents/BP_ResidentLoud', '/Game/NPC/Residents/Audio/DA_Voice_OldMan'),
    ('xiumei', '/Game/NPC/Residents/BP_ResidentShrewd', '/Game/NPC/Residents/Audio/DA_Voice_OldWoman'),
)
SHORT_SOUNDS = ('heard_player', 'question', 'displeased', 'laugh', 'surprised', 'hit')


def object_path(value):
    return value.get_path_name() if value else None


def reaction_settings(asset):
    result = {field: object_path(asset.get_editor_property(field)) for field in SHORT_SOUNDS}
    result.update({field: asset.get_editor_property(field) for field in ('volume', 'pitch', 'max_duration')})
    return result


def dialogue_settings(asset):
    return {'volume': asset.get_editor_property('dialogue_volume'),
            'attenuation': object_path(asset.get_editor_property('dialogue_attenuation'))}


def verify_attenuation(asset):
    settings = asset.get_editor_property('attenuation')
    radius = settings.get_editor_property('attenuation_shape_extents').x
    falloff = settings.get_editor_property('falloff_distance')
    if (not settings.get_editor_property('attenuate') or not settings.get_editor_property('spatialize') or
            settings.get_editor_property('distance_algorithm') != unreal.AttenuationDistanceModel.LINEAR or
            settings.get_editor_property('attenuation_shape') != unreal.AttenuationShape.SPHERE or
            not math.isfinite(radius) or not math.isfinite(falloff) or radius < 600 or falloff <= 0):
        raise RuntimeError('台词衰减须为空间球形线性衰减，原音量范围至少 600 厘米')
    # 玩家距 NPC 六米，镜头在玩家后方四米，声音此时仍应有六成以上
    at_camera = max(0.0, 1.0-max(0.0, 1000.0-radius)/falloff)
    if at_camera <= .6:
        raise RuntimeError('现有 SA_NpcDialogue 范围不足，请在编辑器检查后再运行脚本')
    return {'inner_radius_cm': radius, 'falloff_cm': falloff, 'volume_at_1000_cm': at_camera}


def run():
    table = unreal.load_asset(TABLE_PATH)
    if not isinstance(table, unreal.DataTable):
        raise RuntimeError('缺少正式人物表')
    table_before = json.loads(unreal.DataTableFunctionLibrary.export_data_table_to_json_string(table))
    table_rows = {row['Name']: row for row in table_before}
    voices = []
    before = {}
    for row_name, blueprint_path, voice_path in CHARACTERS:
        asset = unreal.load_asset(voice_path)
        blueprint = unreal.load_asset(blueprint_path)
        if not isinstance(asset, unreal.DeliveryNpcVoiceSet) or not blueprint:
            raise RuntimeError('缺少正式人物蓝图或声音资产：'+row_name)
        if row_name not in table_rows or voice_path not in str(table_rows[row_name].get('VoiceSet', '')):
            raise RuntimeError('人物表的声音引用已变化，先检查配置：'+row_name)
        cdo = unreal.get_default_object(blueprint.generated_class())
        profile = cdo.get_editor_property('profile')
        presentation = cdo.get_component_by_class(unreal.DeliveryNpcPresentationComponent)
        if (profile.get_editor_property('data_table') != table or str(profile.get_editor_property('row_name')) != row_name or
                not presentation or presentation.get_editor_property('voice_set') != asset):
            raise RuntimeError('人物蓝图没有引用预期的人物行和声音资产：'+row_name)
        before[row_name] = {'reaction': reaction_settings(asset), 'dialogue': dialogue_settings(asset)}
        voices.append((row_name, asset, presentation))

    attenuation = unreal.load_asset(ATTENUATION_PATH)
    created = attenuation is None
    if attenuation and not isinstance(attenuation, unreal.SoundAttenuation):
        raise RuntimeError('已有其他类型资产占用了 SA_NpcDialogue 路径')
    if created:
        folder, name = ATTENUATION_PATH.rsplit('/', 1)
        attenuation = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, folder, unreal.SoundAttenuation, unreal.SoundAttenuationFactory())
        if not attenuation:
            raise RuntimeError('无法创建台词衰减资产')
        settings = unreal.SoundAttenuationSettings()
        settings.set_editor_property('attenuate', True)
        settings.set_editor_property('spatialize', True)
        settings.set_editor_property('distance_algorithm', unreal.AttenuationDistanceModel.LINEAR)
        settings.set_editor_property('attenuation_shape', unreal.AttenuationShape.SPHERE)
        settings.set_editor_property('attenuation_shape_extents', unreal.Vector(600, 0, 0))
        settings.set_editor_property('falloff_distance', 1200.0)
        attenuation.set_editor_property('attenuation', settings)
    # 已有资产保留编辑器中的调整，只检查交谈距离内能否听清
    attenuation_report = verify_attenuation(attenuation)

    for _, asset, _ in voices:
        asset.set_editor_property('dialogue_volume', 1.0)
        asset.set_editor_property('dialogue_attenuation', attenuation)
    after = {}
    for row_name, asset, presentation in voices:
        after[row_name] = {'reaction': reaction_settings(asset), 'dialogue': dialogue_settings(asset)}
        if before[row_name]['reaction'] != after[row_name]['reaction']:
            raise RuntimeError('短反应音配置发生了意外变化，停止保存：'+row_name)
        if after[row_name]['dialogue'] != {'volume': 1.0, 'attenuation': attenuation.get_path_name()}:
            raise RuntimeError('台词配置没有正确应用：'+row_name)
        if presentation.get_editor_property('voice_set') != asset:
            raise RuntimeError('蓝图声音引用发生了意外变化：'+row_name)
    table_after = json.loads(unreal.DataTableFunctionLibrary.export_data_table_to_json_string(table))
    if table_before != table_after:
        raise RuntimeError('人物表发生了意外变化，停止保存')

    report = {'status': 'checked_before_save', 'before': before, 'after': after,
              'table_unchanged': True, 'attenuation': attenuation_report,
              'attenuation_asset': ATTENUATION_PATH, 'created_attenuation': created}
    root = Path(unreal.Paths.project_dir()).resolve()
    output = root/'Saved/AIResearch/NpcDialogueAudio'
    output.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    report_path = output/(stamp+'.json')
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    if created and not unreal.EditorAssetLibrary.save_loaded_asset(attenuation):
        raise RuntimeError('台词衰减资产保存失败')
    for _, asset, _ in voices:
        if not unreal.EditorAssetLibrary.save_loaded_asset(asset):
            raise RuntimeError('声音资产保存失败：'+asset.get_path_name())
    report['status'] = 'saved'
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    (output/'latest.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    unreal.log('NPC_DIALOGUE_AUDIO_SAVED '+str(report_path))


if __name__ == '__main__':
    run()
