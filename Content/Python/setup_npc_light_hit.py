"""只导入并绑定 NPC 站立受击动画，保留主动画图和其他角色设置

默认只读；使用 -ApplyNpcLightHit 写入，保存后可在新进程中再次只读检查
"""
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import sys

import unreal

sys.path.insert(0, str(Path(__file__).resolve().parent))
import setup_npc_body_animation as body

LIGHT_HIT = '/Game/NPC/Residents/Animation/A_NpcLightHit'
LIGHT_HIT_OBJECT = LIGHT_HIT+'.A_NpcLightHit'


def check_pose(animation):
    options = unreal.AnimPoseEvaluationOptions()
    options.set_editor_property('optional_skeletal_mesh', unreal.load_asset('/Game/Characters/NPC/SK_NpcMan'))
    samples = []
    for index in range(11):
        time = animation.get_play_length()*index/10
        pose = unreal.AnimPoseExtensions.get_anim_pose_at_time(animation, time, options)
        values = {}
        for bone in ['Hips', 'Head', 'LeftFoot', 'RightFoot']:
            p = unreal.AnimPoseExtensions.get_bone_pose(pose, bone, unreal.AnimPoseSpaces.WORLD).translation
            values[bone] = [p.x, p.y, p.z]
        samples.append({'time': time, **values})
    hip_min = min(row['Hips'][2] for row in samples)
    hip_max = max(row['Hips'][2] for row in samples)
    head_above_hip = min(row['Head'][2]-row['Hips'][2] for row in samples)
    if hip_max <= 0 or hip_min < hip_max*.85 or head_above_hip <= 0:
        raise RuntimeError('导入后的受击动画没有保持站立，停止绑定')
    return {'samples_cm': samples, 'hip_min_cm': hip_min, 'hip_final_cm': samples[-1]['Hips'][2],
            'head_above_hip_min_cm': head_above_hip}


def main(apply=False):
    root = Path(unreal.Paths.project_dir()).resolve()
    output = root/'Saved/AIResearch/NpcLightHit'/datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output.mkdir(parents=True, exist_ok=False)
    report = {'status': 'preparing', 'apply': apply, 'backups': [], 'blueprints': {}}

    def write_report():
        (output/'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    try:
        source = root/'Design/Art/NPC/Residents/LightHitSource.json'
        report['source'] = json.loads(source.read_text(encoding='utf-8'))
        clip = report['source']['animations'][0]
        if clip['source'] != 'Hit_Chest' or clip['output'] != 'A_NpcLightHit' or clip['hip_min'] < clip['reference_hip_height']*.85:
            raise RuntimeError('源动画记录与站立受击不符')
        before = {path: body.snapshot(path) for path in body.BLUEPRINTS}
        allowed = {body.HIT+'.A_ResidentHit', LIGHT_HIT_OBJECT}
        for path, state in before.items():
            if state['preserved']['hit_animation'] not in allowed:
                raise RuntimeError('居民已有其他受击配置，请保留现有修改：'+path)
            if state['anim_class'] != body.BODY+'.ABP_NpcBody_C':
                raise RuntimeError('居民没有使用正式主动画图：'+path)
        protected = set(body.PROTECTED+[body.BODY, body.HIT])
        for state in before.values():
            protected.update(state['preserved'][field].split('.')[0] for field in body.ANIMATIONS
                             if field != 'hit_animation' and state['preserved'][field])
        hashes = {path: body.file_hash(body.asset_file(root, path)) for path in sorted(protected)}
        report['protected_before'] = hashes
        report['blueprints'] = {path: {'before': state} for path, state in before.items()}
        if apply:
            for path in body.BLUEPRINTS+[LIGHT_HIT]:
                package = body.asset_file(root, path)
                for suffix in ['.uasset', '.uexp', '.ubulk']:
                    original = package.with_suffix(suffix)
                    if not original.exists():
                        continue
                    backup = output/'backup'/original.relative_to(root)
                    backup.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(original, backup)
                    digest = body.file_hash(original)
                    if body.file_hash(backup) != digest:
                        raise RuntimeError('资源备份校验失败')
                    report['backups'].append({'original': str(original), 'backup': str(backup), 'sha256': digest})
            write_report()
            animation, report['import'] = body.import_hit_animation(root, light_hit=True)
        else:
            animation = unreal.load_asset(LIGHT_HIT)
        if not isinstance(animation, unreal.AnimSequence) or abs(animation.get_play_length()-1/3) > .04:
            raise RuntimeError('站立受击动画不存在或长度不符')
        if animation.get_editor_property('enable_root_motion'):
            raise RuntimeError('站立受击不能启用 Root Motion')
        report['pose'] = check_pose(animation)
        report['graph_check'] = unreal.DeliveryNpcAnimationAssetTools.check_body_animation()
        if not report['graph_check'].startswith('NPC body graph checked; poses=10;'):
            raise RuntimeError(report['graph_check'])
        if apply:
            for path in body.BLUEPRINTS:
                cls = unreal.EditorAssetLibrary.load_blueprint_class(path)
                unreal.get_default_object(cls).get_editor_property('presentation').set_editor_property('hit_animation', animation)
                blueprint = unreal.load_asset(path)
                if not unreal.BlueprintEditorLibrary.compile_blueprint(blueprint):
                    raise RuntimeError('绑定站立受击后蓝图编译失败：'+path)
        for path, state in before.items():
            expected = dict(state)
            expected['preserved'] = dict(state['preserved'], hit_animation=LIGHT_HIT_OBJECT)
            after = body.snapshot(path)
            report['blueprints'][path]['after'] = after
            if after != expected:
                raise RuntimeError('NPC 的其他配置发生变化，停止保存：'+path)
        report['protected_after'] = {path: body.file_hash(body.asset_file(root, path)) for path in sorted(protected)}
        if hashes != report['protected_after']:
            raise RuntimeError('原动画、骨架或面部资源被改动，停止保存')
        if apply:
            for path in body.BLUEPRINTS:
                if not unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=True):
                    raise RuntimeError('居民蓝图保存失败：'+path)
        report['status'] = 'saved_and_checked' if apply else 'readback_passed'
        report['saved_assets'] = [LIGHT_HIT]+body.BLUEPRINTS if apply else []
        report['notes'] = ['只替换轻拳动画，伤害、倒地门槛、倒地和起身规则不变',
                           '骨骼位置与绑定检查通过后，仍需实机检查挥拳命中及受击衔接']
        write_report()
        unreal.log('NPC_LIGHT_HIT_REPORT='+str(output/'report.json'))
    except Exception as error:
        report.update(status='failed', error=str(error))
        write_report()
        raise


if __name__ == '__main__':
    main('-ApplyNpcLightHit' in unreal.SystemLibrary.get_command_line())
