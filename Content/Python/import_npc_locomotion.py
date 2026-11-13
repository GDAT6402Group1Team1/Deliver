"""导入两条已有 CC0 动画，并为居民蓝图配置踱步、普通行走和急走

先运行 Blender 的 retarget_residents.py -- --locomotion-only，再由 UE 执行本脚本
已存在的动画不会重新导入，原模型、骨架、表情和其他动作保持原样
"""
import hashlib
import json
from pathlib import Path

import unreal


ROOT = Path(unreal.Paths.project_dir()).resolve()
ANIMATION_FOLDER = '/Game/NPC/Residents/Animation'
BLUEPRINTS = ['/Game/NPC/Residents/BP_ResidentLoud', '/Game/NPC/Residents/BP_ResidentShrewd']
NEW_ANIMATIONS = {'A_NpcWalk': 1.333333, 'A_NpcJog': .933333}
GAIT_SETTINGS = {
    'walk_animation': 'A_NpcWalk',
    'pace_animation': 'A_ResidentWalk',
    'hurry_animation': 'A_NpcJog',
    'walk_animation_speed': 180.,
    'pace_animation_speed': 80.,
    'hurry_animation_speed': 240.,
}
REPORT_PATH = ROOT / 'Saved/AIResearch/NpcLocomotionImport/latest.json'


def object_path(value):
    return value.get_path_name() if value else None


def unchanged_settings(npc):
    mesh = npc.get_editor_property('mesh')
    presentation = npc.get_editor_property('presentation')
    profile = npc.get_editor_property('profile')
    transform = mesh.get_relative_transform()
    position = transform.translation
    rotation = transform.rotation
    scale = transform.scale3d
    return {
        'mesh': object_path(mesh.get_editor_property('skeletal_mesh_asset')),
        # UE 的 Struct 字符串含临时对象地址，比较数值才能判断模型是否真的改变
        'mesh_transform': {'position': [position.x, position.y, position.z],
                           'rotation': [rotation.x, rotation.y, rotation.z, rotation.w],
                           'scale': [scale.x, scale.y, scale.z]},
        'display_name': str(npc.get_editor_property('display_name')),
        'persona': npc.get_editor_property('agent').get_editor_property('persona'),
        'profile_table': object_path(profile.get_editor_property('data_table')),
        'profile_row': str(profile.get_editor_property('row_name')),
        'behavior_tree': object_path(npc.get_editor_property('behavior_tree')),
        'tts_voice': str(presentation.get_editor_property('tts_voice')),
        **{name: object_path(presentation.get_editor_property(name)) for name in [
            'voice_set', 'expressions', 'idle_animation', 'talk_animation',
            'attack_animation', 'down_animation', 'get_up_animation', 'hold_animation']},
    }


def gait_settings(npc):
    presentation = npc.get_editor_property('presentation')
    return {name: presentation.get_editor_property(name) for name in GAIT_SETTINGS}


def describe_gait(values):
    return {name: object_path(value) if name.endswith('_animation') else float(value)
            for name, value in values.items()}


def protected_hashes():
    paths = ['Characters/NPC/SK_NpcMan_Skeleton', 'NPC/Residents/Appearance/SK_NpcManFace',
             'NPC/Residents/Appearance/SK_NpcWomanFace', 'NPC/Residents/Animation/A_ResidentWalk']
    return {path: hashlib.sha256((ROOT / 'Content' / (path+'.uasset')).read_bytes()).hexdigest()
            for path in paths}


def main():
    report = {'status': 'preparing', 'animations': [], 'blueprints': {}, 'saved_assets': []}
    REPORT_PATH.parent.mkdir(parents=True, exist_ok=True)

    def save_report():
        REPORT_PATH.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    updates = []
    try:
        original_mesh = unreal.load_asset('/Game/Characters/NPC/SK_NpcMan')
        if not original_mesh:
            raise RuntimeError('缺少居民原模型，无法确定动画骨架和导入比例')
        skeleton = original_mesh.get_editor_property('skeleton')
        if object_path(skeleton) != '/Game/Characters/NPC/SK_NpcMan_Skeleton.SK_NpcMan_Skeleton':
            raise RuntimeError('居民共用骨架已变化，请先检查再导入')
        original_files = protected_hashes()
        # 编译新字段后才允许导入，避免使用旧 DLL 时保存不完整的蓝图配置
        for path in BLUEPRINTS:
            blueprint = unreal.load_asset(path)
            cls = unreal.EditorAssetLibrary.load_blueprint_class(path)
            if not blueprint or not cls:
                raise RuntimeError('缺少居民蓝图：'+path)
            npc = unreal.get_default_object(cls)
            before = unchanged_settings(npc)
            gait = gait_settings(npc)
            updates.append((path, blueprint, before, gait))
            report['blueprints'][path] = {'before': describe_gait(gait), 'unchanged_before': before}
        report['skeleton'] = object_path(skeleton)
        save_report()

        for name, duration in NEW_ANIMATIONS.items():
            path = ANIMATION_FOLDER+'/'+name
            animation = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
            created = animation is None
            if created:
                source = ROOT / 'Design/Art/NPC/Residents' / (name+'.fbx')
                if not source.is_file():
                    raise RuntimeError('缺少已重定向的源动画：'+str(source))
                options = unreal.FbxImportUI()
                options.set_editor_property('automated_import_should_detect_type', False)
                options.mesh_type_to_import = unreal.FBXImportType.FBXIT_ANIMATION
                options.import_as_skeletal = True
                options.import_mesh = False
                options.import_animations = True
                options.import_materials = False
                options.import_textures = False
                options.skeleton = skeleton
                data = options.get_editor_property('anim_sequence_import_data')
                data.set_editor_property('use_default_sample_rate', True)
                scale = original_mesh.get_editor_property('asset_import_data').get_editor_property('import_uniform_scale')
                data.set_editor_property('import_uniform_scale', scale)
                task = unreal.AssetImportTask()
                task.filename = str(source)
                task.destination_path = ANIMATION_FOLDER
                task.destination_name = name
                task.automated = True
                task.save = True
                task.replace_existing = False
                task.options = options
                task.factory = unreal.FbxFactory()
                unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
                animation = unreal.load_asset(path)
            if not isinstance(animation, unreal.AnimSequence) or animation.get_editor_property('skeleton') != skeleton:
                raise RuntimeError('新动画类型或骨架不符：'+path)
            if abs(animation.get_play_length()-duration) > .04:
                raise RuntimeError('动画长度与源剪辑不符：'+path)
            if created:
                animation.set_editor_property('enable_root_motion', False)
                if not unreal.EditorAssetLibrary.save_loaded_asset(animation):
                    raise RuntimeError('新动画保存失败：'+path)
                report['saved_assets'].append(path)
            elif animation.get_editor_property('enable_root_motion'):
                raise RuntimeError('已存在的新动画启用了 Root Motion，保留原设置并停止绑定：'+path)
            report['animations'].append({'path': path, 'created': created,
                'duration': animation.get_play_length(), 'skeleton': object_path(skeleton)})
            save_report()

        if protected_hashes() != original_files:
            raise RuntimeError('动画导入改变了原骨架、模型或踱步动画，停止保存蓝图')
        expected = {name: unreal.load_asset(ANIMATION_FOLDER+'/'+value) if name.endswith('_animation') else value
                    for name, value in GAIT_SETTINGS.items()}
        if any(value is None for value in expected.values()):
            raise RuntimeError('三种步态所需的动画不完整')
        for path, blueprint, before, _ in updates:
            npc = unreal.get_default_object(unreal.EditorAssetLibrary.load_blueprint_class(path))
            presentation = npc.get_editor_property('presentation')
            blueprint.modify()
            npc.modify()
            presentation.modify()
            for name, value in expected.items():
                presentation.set_editor_property(name, value)
            unreal.BlueprintEditorLibrary.compile_blueprint(blueprint)
            current = unreal.get_default_object(unreal.EditorAssetLibrary.load_blueprint_class(path))
            after = unchanged_settings(current)
            actual_gait = describe_gait(gait_settings(current))
            expected_gait = describe_gait(expected)
            differences = {key: {'before': value, 'after': after.get(key)} for key, value in before.items()
                           if value != after.get(key)}
            gait_differences = {key: {'expected': value, 'actual': actual_gait.get(key)}
                                for key, value in expected_gait.items() if value != actual_gait.get(key)}
            report['blueprints'][path].update(after=actual_gait, unchanged_after=after,
                differences=differences, gait_differences=gait_differences)
            save_report()
            if differences or gait_differences:
                raise RuntimeError('蓝图编译后设置与预期不符，停止保存：'+path)

        for path, blueprint, _, _ in updates:
            if not unreal.EditorAssetLibrary.save_loaded_asset(blueprint):
                raise RuntimeError('居民蓝图保存失败：'+path)
            report['saved_assets'].append(path)
            save_report()
        if protected_hashes() != original_files:
            raise RuntimeError('保存蓝图时原骨架、模型或踱步动画发生变化')
        report.update(status='saved', protected_assets_unchanged=True, other_npc_settings_unchanged=True)
        save_report()
        unreal.log('NPC_LOCOMOTION_SAVED '+str(REPORT_PATH))
    except Exception as error:
        # 已导入的新资源保留；尚未保存的蓝图配置恢复到开始时的数值
        for path, _, _, before_gait in updates:
            cls = unreal.EditorAssetLibrary.load_blueprint_class(path)
            if cls:
                presentation = unreal.get_default_object(cls).get_editor_property('presentation')
                for name, value in before_gait.items():
                    presentation.set_editor_property(name, value)
        report.update(status='failed', error=str(error))
        save_report()
        raise


main()
