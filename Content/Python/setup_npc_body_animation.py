"""创建 NPC 主动画蓝图并记录资源备份，已有节点默认保留

UE 命令行加 -ReplaceNpcBodyAnimation 才重建已有图
加 -InspectNpcBodyAnimation 只读检查，可在独立进程验证保存结果
"""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil

import unreal


BODY = '/Game/NPC/Residents/Animation/ABP_NpcBody'
BLUEPRINTS = ['/Game/NPC/Residents/BP_ResidentLoud', '/Game/NPC/Residents/BP_ResidentShrewd']
PROTECTED = [
    '/Game/Characters/NPC/SK_NpcMan_Skeleton',
    '/Game/NPC/Residents/Appearance/SK_NpcManFace',
    '/Game/NPC/Residents/Appearance/SK_NpcWomanFace',
    '/Game/NPC/Residents/Animation/ABP_NpcConversation',
]
ANIMATIONS = [
    'idle_animation', 'talk_animation', 'walk_animation', 'pace_animation', 'hurry_animation',
    'hold_animation', 'attack_animation', 'down_animation', 'get_up_animation',
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


def main(replace_graph=False, inspect_only=False):
    root = Path(unreal.Paths.project_dir()).resolve()
    reports = root/'Saved/AIResearch/NpcBodyAnimation'
    output = reports/datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output.mkdir(parents=True, exist_ok=False)
    report = {'status': 'preparing', 'replace_graph': replace_graph, 'inspect_only': inspect_only,
              'backups': {}, 'blueprints': {}, 'body_blueprint': BODY}

    def save_report():
        (output/'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    try:
        if not hasattr(unreal, 'DeliveryNpcBodyAnimInstance'):
            raise RuntimeError('尚未加载 NPC 主动画运行类，请先完成编译')
        for path in BLUEPRINTS:
            report['blueprints'][path] = {'before': snapshot(path)}
        original_files = {path: file_hash(asset_file(root, path)) for path in PROTECTED}
        report['protected_before'] = original_files
        if not inspect_only:
            # 保存磁盘原件后才调用编辑工具，失败时可按清单恢复这三个包
            for path in [BODY]+BLUEPRINTS:
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
            result = unreal.DeliveryNpcAnimationAssetTools.create_body_animation(replace_graph)
            report['tool_result'] = result
            if not result.startswith('NPC body animation saved;'):
                raise RuntimeError(result)

        body = unreal.load_asset(BODY)
        if not body:
            raise RuntimeError('主动画蓝图未能加载')
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
            if after['preserved'] != report['blueprints'][path]['before']['preserved']:
                raise RuntimeError('居民模型、声音或其他配置发生计划外变化：'+path)
            if after['preserved']['skeleton'] != report['body_skeleton']:
                raise RuntimeError('居民和主动画蓝图没有使用同一骨架：'+path)
        report['protected_after'] = {path: file_hash(asset_file(root, path)) for path in PROTECTED}
        if report['protected_after'] != original_files:
            raise RuntimeError('原骨架、面部网格或交谈后处理图被修改')
        report['status'] = 'inspected' if inspect_only else 'saved_and_checked'
        report['saved_assets'] = [] if inspect_only else [BODY]+BLUEPRINTS
        report['notes'] = ['独立进程读回可使用 -InspectNpcBodyAnimation',
                           '主图负责九种动作之间的混合，后处理图继续负责注视和倾听',
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
    main('-ReplaceNpcBodyAnimation' in command, '-InspectNpcBodyAnimation' in command)
