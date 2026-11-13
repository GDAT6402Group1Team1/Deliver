"""创建 NPC 主动画蓝图并记录资源备份，已有节点默认保留

UE 命令行加 -ReplaceNpcBodyAnimation 才重建已有图
加 -ImportNpcHitAnimation 导入已有受击素材并绑定两名居民
加 -InspectNpcBodyAnimation 检查保存结果，不保存资源
"""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil

import unreal


BODY = '/Game/NPC/Residents/Animation/ABP_NpcBody'
HIT = '/Game/NPC/Residents/Animation/A_ResidentHit'
BLUEPRINTS = ['/Game/NPC/Residents/BP_ResidentLoud', '/Game/NPC/Residents/BP_ResidentShrewd']
PROTECTED = [
    '/Game/Characters/NPC/SK_NpcMan',
    '/Game/Characters/NPC/SK_NpcMan_Skeleton',
    '/Game/NPC/Residents/Appearance/SK_NpcManFace',
    '/Game/NPC/Residents/Appearance/SK_NpcWomanFace',
    '/Game/NPC/Residents/Animation/ABP_NpcConversation',
]
ANIMATIONS = [
    'idle_animation', 'talk_animation', 'walk_animation', 'pace_animation', 'hurry_animation',
    'hold_animation', 'attack_animation', 'down_animation', 'get_up_animation', 'hit_animation',
]


def object_path(value):
    return value.get_path_name() if value else None


def snapshot(path):
    cls = unreal.EditorAssetLibrary.load_blueprint_class(path)
    if not cls:
        raise RuntimeError('无法加载居民蓝图：'+path)
    npc = unreal.get_default_object(cls)
    mesh = npc.get_editor_property('mesh')
    asset = mesh.get_editor_property('skeletal_mesh_asset')
    presentation = npc.get_editor_property('presentation')
    transform = mesh.get_relative_transform()
    p, r, s = transform.translation, transform.rotation, transform.scale3d
    profile = npc.get_editor_property('profile')
    return {
        'animation_mode': str(mesh.get_editor_property('animation_mode')),
        'anim_class': object_path(mesh.get_editor_property('anim_class')),
        'preserved': {
            'mesh': object_path(asset),
            'skeleton': object_path(asset.get_editor_property('skeleton')),
            'post_process': object_path(asset.get_editor_property('post_process_anim_blueprint')),
            'post_process_disabled': bool(mesh.get_editor_property('disable_post_process_blueprint')),
            'mesh_transform': {'position': [p.x, p.y, p.z], 'rotation': [r.x, r.y, r.z, r.w], 'scale': [s.x, s.y, s.z]},
            'materials': [object_path(mesh.get_material(i)) for i in range(mesh.get_num_materials())],
            'display_name': str(npc.get_editor_property('display_name')),
            'persona': npc.get_editor_property('agent').get_editor_property('persona'),
            'profile_table': object_path(profile.get_editor_property('data_table')),
            'profile_row': str(profile.get_editor_property('row_name')),
            'behavior_tree': object_path(npc.get_editor_property('behavior_tree')),
            'walk_speed': float(npc.get_editor_property('walk_speed')),
            'tts_voice': str(presentation.get_editor_property('tts_voice')),
            **{field: object_path(presentation.get_editor_property(field))
               for field in ANIMATIONS+['voice_set', 'expressions']},
            **{field: float(presentation.get_editor_property(field))
               for field in ['walk_animation_speed', 'pace_animation_speed', 'hurry_animation_speed']},
        },
    }


def asset_file(root, asset_path):
    if not asset_path.startswith('/Game/'):
        raise RuntimeError('只允许备份本项目资源：'+asset_path)
    file = (root/'Content'/(asset_path[len('/Game/'):]+'.uasset')).resolve()
    if not file.is_relative_to((root/'Content').resolve()):
        raise RuntimeError('资源路径超出项目 Content 目录')
    return file


def file_hash(file):
    return hashlib.sha256(file.read_bytes()).hexdigest()


def import_hit_animation(root, light_hit=False):
    name = 'A_NpcLightHit' if light_hit else 'A_ResidentHit'
    path = '/Game/NPC/Residents/Animation/'+name
    source = root/('Design/Art/NPC/Residents/'+name+'.fbx')
    license_file = root/('Design/Art/NPC/Grandpa/Quaternius_License.txt' if light_hit
                         else 'Design/Art/NPC/Grandpa/Quaternius2_License.txt')
    if not source.is_file() or not license_file.is_file():
        raise RuntimeError('缺少受击源动画或已有许可文件')
    original_mesh = unreal.load_asset('/Game/Characters/NPC/SK_NpcMan')
    if not original_mesh:
        raise RuntimeError('缺少居民原模型，无法确定受击动画的骨架和比例')
    skeleton = original_mesh.get_editor_property('skeleton')
    created = not unreal.EditorAssetLibrary.does_asset_exist(path)
    if created:
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
        task.destination_path = '/Game/NPC/Residents/Animation'
        task.destination_name = name
        task.automated = True
        task.save = False
        task.replace_existing = False
        task.options = options
        task.factory = unreal.FbxFactory()
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    animation = unreal.load_asset(path)
    if not isinstance(animation, unreal.AnimSequence) or animation.get_editor_property('skeleton') != skeleton:
        raise RuntimeError('受击动画的类型或骨架与居民不符')
    # 站立受击使用完整 Hit_Chest，原倒地素材保留给已有资源
    duration = 10/30 if light_hit else 25/30
    if abs(animation.get_play_length()-duration) > .04:
        raise RuntimeError('受击动画长度与源剪辑不符')
    if created:
        animation.set_editor_property('enable_root_motion', False)
    elif animation.get_editor_property('enable_root_motion'):
        raise RuntimeError('已有受击动画启用了 Root Motion，停止绑定并保留原配置')
    if created and not unreal.EditorAssetLibrary.save_loaded_asset(animation):
        raise RuntimeError('受击动画保存失败')
    return animation, {'path': path, 'created': created, 'source': str(source),
                       'source_sha256': file_hash(source), 'duration': animation.get_play_length(),
                       'source_clip': 'Hit_Chest' if light_hit else 'Hit_Knockback', 'author': 'Quaternius', 'license': 'CC0',
                       'license_file': str(license_file), 'license_sha256': file_hash(license_file),
                       'source_url': 'https://quaternius.com/packs/universalanimationlibrary.html' if light_hit
                                     else 'https://quaternius.com/packs/universalanimationlibrary2.html',
                       'retarget_script': 'Content/Python/retarget_residents.py'}


def main(replace_graph=False, inspect_only=False, import_hit=False):
    root = Path(unreal.Paths.project_dir()).resolve()
    reports = root/'Saved/AIResearch/NpcBodyAnimation'
    output = reports/datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output.mkdir(parents=True, exist_ok=False)
    report = {'status': 'preparing', 'replace_graph': replace_graph, 'inspect_only': inspect_only, 'import_hit': import_hit,
              'backups': {}, 'blueprints': {}, 'body_blueprint': BODY}

    def save_report():
        (output/'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    try:
        if not hasattr(unreal, 'DeliveryNpcBodyAnimInstance'):
            raise RuntimeError('尚未加载 NPC 主动画运行类，请先完成编译')
        if inspect_only and import_hit:
            raise RuntimeError('只读检查不能同时导入受击动画')
        for path in BLUEPRINTS:
            report['blueprints'][path] = {'before': snapshot(path)}
            old_hit = report['blueprints'][path]['before']['preserved']['hit_animation']
            if import_hit and old_hit not in (None, HIT+'.A_ResidentHit'):
                raise RuntimeError('居民已有其他受击动画，请先确认配置：'+path)
            if not import_hit and not old_hit:
                raise RuntimeError('居民尚未配置受击动画，请先使用 -ImportNpcHitAnimation -ReplaceNpcBodyAnimation')
        original_files = {path: file_hash(asset_file(root, path)) for path in PROTECTED}
        report['protected_before'] = original_files
        if not inspect_only:
            # 保存磁盘原件后才导入和绑定，失败时可按清单恢复原包
            for path in [BODY]+BLUEPRINTS+([HIT] if import_hit else []):
                file = asset_file(root, path)
                copies = []
                for extension in ['.uasset', '.uexp', '.ubulk']:
                    source = file.with_suffix(extension)
                    if not source.exists():
                        continue
                    destination = output/'backup'/source.relative_to(root)
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, destination)
                    digest = file_hash(source)
                    if file_hash(destination) != digest:
                        raise RuntimeError('备份校验失败：'+str(source))
                    copies.append({'source': str(source), 'backup': str(destination), 'sha256': digest})
                report['backups'][path] = copies
            report['status'] = 'backed_up'
            save_report()
            if import_hit:
                animation, report['hit_animation'] = import_hit_animation(root)
                if {path: file_hash(asset_file(root, path)) for path in PROTECTED} != original_files:
                    raise RuntimeError('受击动画导入改变了原骨架或模型，停止绑定')
                for path in BLUEPRINTS:
                    cls = unreal.EditorAssetLibrary.load_blueprint_class(path)
                    presentation = unreal.get_default_object(cls).get_editor_property('presentation')
                    presentation.set_editor_property('hit_animation', animation)
                    blueprint = unreal.load_asset(path)
                    if not unreal.BlueprintEditorLibrary.compile_blueprint(blueprint):
                        raise RuntimeError('居民绑定受击动画后编译失败：'+path)
                save_report()
            result = unreal.DeliveryNpcAnimationAssetTools.create_body_animation(replace_graph)
            report['tool_result'] = result
            if not result.startswith('NPC body animation saved;'):
                raise RuntimeError(result)

        body = unreal.load_asset(BODY)
        if not body:
            raise RuntimeError('主动画蓝图未能加载')
        report['graph_check'] = unreal.DeliveryNpcAnimationAssetTools.check_body_animation()
        if not report['graph_check'].startswith('NPC body graph checked; poses=10;'):
            raise RuntimeError(report['graph_check'])
        report['compiled'] = {BODY: unreal.BlueprintEditorLibrary.compile_blueprint(body)}
        if not report['compiled'][BODY]:
            raise RuntimeError('保存的主动画蓝图编译失败')
        body_class = unreal.EditorAssetLibrary.load_blueprint_class(BODY)
        if not body_class:
            raise RuntimeError('主动画蓝图未能生成动画实例类')
        report['body_parent'] = object_path(unreal.BlueprintEditorLibrary.get_blueprint_parent_class(body))
        report['body_skeleton'] = object_path(body.get_editor_property('target_skeleton'))
        if report['body_parent'] != '/Script/Delivery.DeliveryNpcBodyAnimInstance':
            raise RuntimeError('主动画蓝图的父类不符合预期')
        expected_class = BODY+'.ABP_NpcBody_C'
        for path in BLUEPRINTS:
            report['compiled'][path] = unreal.BlueprintEditorLibrary.compile_blueprint(unreal.load_asset(path))
            if not report['compiled'][path]:
                raise RuntimeError('保存的居民蓝图编译失败：'+path)
            after = snapshot(path)
            report['blueprints'][path]['after'] = after
            if after['anim_class'] != expected_class or 'ANIMATION_BLUEPRINT' not in after['animation_mode']:
                raise RuntimeError('居民主动画绑定未保存：'+path)
            expected = dict(report['blueprints'][path]['before']['preserved'])
            if import_hit:
                expected['hit_animation'] = HIT+'.A_ResidentHit'
            if after['preserved'] != expected:
                raise RuntimeError('居民模型、声音或其他配置发生计划外变化：'+path)
            if not after['preserved']['hit_animation']:
                raise RuntimeError('居民尚未配置受击动画：'+path)
            if after['preserved']['skeleton'] != report['body_skeleton']:
                raise RuntimeError('居民和主动画蓝图没有使用同一骨架：'+path)
        report['protected_after'] = {path: file_hash(asset_file(root, path)) for path in PROTECTED}
        if report['protected_after'] != original_files:
            raise RuntimeError('原骨架、面部网格或交谈后处理图被修改')
        report['status'] = 'inspected' if inspect_only else 'saved_and_checked'
        report['saved_assets'] = [] if inspect_only else [BODY]+BLUEPRINTS+([HIT] if report.get('hit_animation', {}).get('created') else [])
        report['notes'] = ['独立进程读回可使用 -InspectNpcBodyAnimation',
                           '主图负责十种动作之间的混合，后处理图继续负责注视和倾听',
                           '资产检查不代替动作过渡和多人画面的试玩验收']
        save_report()
        (reports/'latest.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        unreal.log('NPC_BODY_ANIMATION_REPORT='+str(output/'report.json'))
    except Exception as error:
        report.update(status='failed', error=str(error))
        save_report()
        raise


if __name__ == '__main__':
    command = unreal.SystemLibrary.get_command_line()
    main('-ReplaceNpcBodyAnimation' in command, '-InspectNpcBodyAnimation' in command,
         '-ImportNpcHitAnimation' in command)
