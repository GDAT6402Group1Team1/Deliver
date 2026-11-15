"""检查已有 Quaternius 原片，用 UE 原生 IK 适配居民和二狗并配置可编辑主动画蓝图。

默认打印原片、正式网格和已有成品。-RetargetNpcJumpAnimations 创建成品和独立 IK 资产，并升级本脚本已生成的六个成品的根锁。
-InstallNpcJumpAnimations 只读取已保存成品，再为两个主动画蓝图分别增补跳跃输入。
"""
import json
import math
from pathlib import Path
import sys

import unreal

sys.path.insert(0, str(Path(__file__).resolve().parent))
import import_companion_home_animations as home


LIBRARY = 'Library1'
SOURCE_PREFIX = 'Quaternius_AnimationLibrary_Standard'
SOURCE_FOLDER = home.ALIGNED_SOURCE_ROOT + '/' + LIBRARY
SOURCE_RIG = home.ALIGNED_RIG_ROOT + '/IK_HomeLibrary1'
CLIPS = (('Jump_Start', 'JumpStart'), ('Jump_Loop', 'JumpLoop'), ('Jump_Land', 'JumpLand'))
CLIP_TAG = 'DeliveryNpcJumpAnimation'
TARGETS = (
    ('Resident', '/Game/NPC/Residents/Animation/ABP_NpcBody',
     ('/Game/NPC/Residents/BP_ResidentLoud', '/Game/NPC/Residents/BP_ResidentShrewd')),
    ('Ergou', '/Game/NPC/Companion/Animation/ABP_CompanionBody', (home.COMPANION,)),
)


def target_mesh(body_path, characters):
    mesh = None
    skeleton = None
    for path in characters:
        npc_class = unreal.EditorAssetLibrary.load_blueprint_class(path)
        if not npc_class:
            raise RuntimeError('缺少已配置 NPC：' + path)
        npc = unreal.get_default_object(npc_class)
        component = npc.get_editor_property('mesh')
        current = component.get_editor_property('skeletal_mesh_asset')
        current_skeleton = current.get_editor_property('skeleton') if current else None
        animation_class = component.get_editor_property('anim_class')
        if (not current_skeleton or (skeleton and skeleton != current_skeleton) or
                home.object_path(animation_class) != body_path + '.' + body_path.rsplit('/', 1)[1] + '_C'):
            raise RuntimeError('NPC 没有使用预期共享骨架或主动画蓝图：' + path)
        mesh = mesh or current
        skeleton = current_skeleton
    return mesh


def verify_source(asset, source_info):
    metadata = unreal.EditorAssetLibrary.get_metadata_tag
    if (not asset or metadata(asset, home.SOURCE_TAG) != home.VERSION or
            metadata(asset, 'SourceSHA256') != source_info['sha256'] or
            metadata(asset, 'SourceBasisAlignment') != home.ALIGNMENT_VERSION):
        raise RuntimeError('已导入的作者原片或对齐版本不符：' + str(home.object_path(asset)))


def retargeter_for(role, source_mesh, mesh, source_rig, body_path):
    if role == 'Ergou':
        target_rig = unreal.load_asset(home.ALIGNED_RIG_ROOT + '/IK_ErgouHome')
        retargeter = unreal.load_asset(home.ALIGNED_RIG_ROOT + '/RTG_HomeLibrary1ToErgou')
    else:
        folder = body_path.rsplit('/', 1)[0] + '/Jump/Retarget'
        default_object = unreal.get_default_object(unreal.EditorAssetLibrary.load_blueprint_class(TARGETS[0][2][0]))
        idle = default_object.get_editor_property('presentation').get_editor_property('idle_animation')
        bones = home.pose_snapshot(idle, mesh)['bones']
        target_rig = home.make_rig(folder + '/IK_ResidentJump', mesh, 'Hips', home.TARGET_CHAINS, bones)
        retargeter = home.make_retargeter(folder + '/RTG_HomeLibrary1ToResidentJump', source_rig, target_rig,
                                          source_mesh, mesh, home.TARGET_CHAINS.keys())
    if not target_rig or not retargeter:
        raise RuntimeError('缺少正式目标 IK 资产：' + role)
    controller = unreal.IKRetargeterController.get_controller(retargeter)
    if (controller.get_ik_rig(unreal.RetargetSourceOrTarget.SOURCE) != source_rig or
            controller.get_ik_rig(unreal.RetargetSourceOrTarget.TARGET) != target_rig or
            unreal.IKRigController.get_controller(target_rig).get_skeletal_mesh() != mesh):
        raise RuntimeError('IK 源、目标或正式网格发生变化：' + role)
    return retargeter


def verify_output_identity(asset, source, source_info, mesh):
    metadata = unreal.EditorAssetLibrary.get_metadata_tag
    if (not isinstance(asset, unreal.AnimSequence) or metadata(asset, CLIP_TAG) != home.VERSION or
            metadata(asset, home.RETARGET_TAG) != home.VERSION or
            metadata(asset, 'SourceBasisAlignment') != home.ALIGNMENT_VERSION or
            metadata(asset, 'SourceAnimation') != home.object_path(source) or
            metadata(asset, 'SourceSHA256') != source_info['sha256'] or
            asset.get_editor_property('skeleton') != mesh.get_editor_property('skeleton') or
            asset.get_editor_property('enable_root_motion') or not asset.get_editor_property('force_root_lock') or
            abs(asset.get_play_length()-source.get_play_length()) > 1/30):
        raise RuntimeError('成品已有不同来源或根设置，保留现有资产：' + str(home.object_path(asset)))


def pose_options(mesh, evaluation, ignore_root_lock):
    options = unreal.AnimPoseEvaluationOptions()
    options.set_editor_property('evaluation_type', evaluation)
    options.set_editor_property('optional_skeletal_mesh', mesh)
    options.set_editor_property('incorporate_root_motion_into_pose', ignore_root_lock)
    return options


def pose_transform(bone):
    return {'position': [bone.translation.x, bone.translation.y, bone.translation.z],
            'rotation_xyzw': [bone.rotation.x, bone.rotation.y, bone.rotation.z, bone.rotation.w],
            'scale': [bone.scale3d.x, bone.scale3d.y, bone.scale3d.z]}


def root_has_identity_basis(root):
    rotation = root['rotation_xyzw']
    return (math.dist(root['position'], (0, 0, 0)) < .01 and math.dist(root['scale'], (1, 1, 1)) < .01 and
            min(math.dist(rotation, (0, 0, 0, w)) for w in (-1, 1)) < .001)


def verify_zero_root_basis(asset, mesh):
    # 核对经过重定向的成品原首帧，不用导入前带坐标转换的作者源根姿势
    options = pose_options(mesh, unreal.AnimDataEvalType.RAW, True)
    pose = unreal.AnimPoseExtensions.get_anim_pose_at_time(asset, 0, options)
    root_name = unreal.AnimPoseExtensions.get_bone_names(pose)[0]
    root = pose_transform(unreal.AnimPoseExtensions.get_bone_pose(pose, root_name, unreal.AnimPoseSpaces.LOCAL))
    if not root_has_identity_basis(root):
        raise RuntimeError(f'原首帧与零位根不一致，保留现有设置：{home.object_path(asset)} root={root}')


def locked_pose_snapshot(asset, mesh):
    if asset.get_editor_property('root_motion_root_lock') != unreal.RootMotionRootLock.ZERO:
        raise RuntimeError('正式跳跃成品需使用零位根锁，请执行 -RetargetNpcJumpAnimations：' + home.object_path(asset))
    report = home.pose_snapshot(asset, mesh)
    bones = report['bones']
    root = bones[0]
    selected = [name for name in bones if name in {root, 'Hips', 'Head', 'LeftUpLeg', 'LeftLeg', 'LeftFoot',
                'LeftToeBase', 'RightUpLeg', 'RightLeg', 'RightFoot', 'RightToeBase', 'LeftArm', 'LeftForeArm',
                'LeftHand', 'RightArm', 'RightForeArm', 'RightHand'}]
    # 编辑器默认忽略根锁，关闭后按正式 Sequence Evaluator 的根锁设置求值
    options = pose_options(mesh, unreal.AnimDataEvalType.COMPRESSED, False)
    report.update(root_lock='Zero', samples=[])
    for fraction in (0, .25, .5, .75, 1):
        time = report['length'] * fraction
        pose = unreal.AnimPoseExtensions.get_anim_pose_at_time(asset, time, options)
        points = {}
        for name in selected:
            bone = unreal.AnimPoseExtensions.get_bone_pose(pose, name, unreal.AnimPoseSpaces.WORLD)
            points[name] = pose_transform(bone)
        root_pose = points[root]
        hips = points['Hips']['position']
        head_hips = math.dist(points['Head']['position'], hips)
        # 蓝图负责恢复约 140 倍的目标根尺度；剪辑根锁只应固定未修正剪辑的根
        if (not all(math.isfinite(value) for point in points.values() for values in point.values() for value in values) or
                not root_has_identity_basis(root_pose) or not .05 < head_hips < 2 or math.dist(hips, (0, 0, 0)) >= 150):
            raise RuntimeError(f'正式根锁求值异常：{home.object_path(asset)} time={time:.4f} '
                               f'rootScale={root_pose["scale"]} rootPos={root_pose["position"]} '
                               f'headHipsLength={head_hips:.4f} hipsPos={hips}')
        report['samples'].append(dict(time=time, bones_mesh_cm=points))
    return report


def main(retarget=False, install=False):
    if retarget and install:
        raise RuntimeError('原生重定向和主动画蓝图安装应分别执行')
    home.editor_guard(retarget or install)
    project = Path(unreal.Paths.project_dir()).resolve()
    source_info = home.local_sources(project)[LIBRARY]
    source_mesh = unreal.load_asset(SOURCE_FOLDER + '/' + SOURCE_PREFIX)
    verify_source(source_mesh, source_info)
    source_rig = unreal.load_asset(SOURCE_RIG)
    if not source_rig or unreal.IKRigController.get_controller(source_rig).get_skeletal_mesh() != source_mesh:
        raise RuntimeError('已对齐源网格与原生 IK Rig 不匹配')
    sources = {}
    for name, phase in CLIPS:
        if name not in source_info['glb']['animations']:
            raise RuntimeError('随包 GLB 没有该成品动作：' + name)
        source = unreal.load_asset(SOURCE_FOLDER + '/' + SOURCE_PREFIX + name)
        verify_source(source, source_info)
        if not isinstance(source, unreal.AnimSequence) or source.get_editor_property('skeleton') != source_mesh.get_editor_property('skeleton'):
            raise RuntimeError('原片与源网格骨架不符：' + name)
        sources[phase] = source

    graph_inputs = {}
    pending_saves = []
    report = {'source_author': source_info['author'], 'source_license': source_info['license'],
              'source_sha256': source_info['sha256'], 'targets': {}}
    for role, body_path, characters in TARGETS:
        mesh = target_mesh(body_path, characters)
        folder = body_path.rsplit('/', 1)[0] + '/Jump'
        assets = []
        role_report = {'mesh': home.object_path(mesh), 'clips': {}}
        report['targets'][role] = role_report
        rig = None
        for original, phase in CLIPS:
            source = sources[phase]
            name = 'A_' + role + phase
            path = folder + '/' + name
            asset = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
            changed = False
            migrated = False
            if not asset and retarget:
                rig = rig or retargeter_for(role, source_mesh, mesh, source_rig, body_path)
                asset = home.run_native_retarget(source, source_mesh, mesh, rig, folder, name)
                home.set_retarget_metadata(asset, source, source_info, original, rig, aligned=True)
                verify_zero_root_basis(asset, mesh)
                asset.set_editor_property('force_root_lock', True)
                asset.set_editor_property('root_motion_root_lock', unreal.RootMotionRootLock.ZERO)
                unreal.EditorAssetLibrary.set_metadata_tag(asset, CLIP_TAG, home.VERSION)
                changed = True
            if asset:
                verify_output_identity(asset, source, source_info, mesh)
                current_lock = asset.get_editor_property('root_motion_root_lock')
                if retarget and current_lock in (unreal.RootMotionRootLock.REF_POSE, unreal.RootMotionRootLock.ANIM_FIRST_FRAME):
                    verify_zero_root_basis(asset, mesh)
                    asset.set_editor_property('root_motion_root_lock', unreal.RootMotionRootLock.ZERO)
                    changed = migrated = True
                phase_report = locked_pose_snapshot(asset, mesh)
                role_report['clips'][phase] = phase_report
                if changed:
                    pending_saves.append((asset, mesh, role_report['clips'], phase, migrated))
                assets.append(asset)
            elif install:
                raise RuntimeError('先执行原生重定向并核对保存的动画：' + path)
            else:
                role_report['clips'][phase] = {'expected_path': path, 'source': home.object_path(source)}
        if len(assets) == len(CLIPS):
            graph_inputs[body_path] = assets

    for asset, mesh, clips, phase, migrated in pending_saves:
        home.save_asset(asset)
        clips[phase] = locked_pose_snapshot(asset, mesh)
        clips[phase]['status'] = 'root_lock_migrated' if migrated else 'native_retargeted'

    if install:
        configure = unreal.DeliveryNpcAnimationAssetTools.configure_body_jump_animation
        for path, assets in graph_inputs.items():
            result = configure(path, *assets, False)
            if result != 'NPC jump graph preflight passed':
                raise RuntimeError(path + ' ' + result)
        for path, assets in graph_inputs.items():
            result = configure(path, *assets, True)
            if not result.startswith('NPC jump graph ready;'):
                raise RuntimeError(path + ' ' + result)
            checked = unreal.DeliveryNpcAnimationAssetTools.check_body_jump_animation(path)
            if not checked.startswith('NPC jump graph checked;'):
                raise RuntimeError(path + ' ' + checked)
            report['targets'][next(role for role, body, _ in TARGETS if body == path)]['graph'] = result
            report['targets'][next(role for role, body, _ in TARGETS if body == path)]['checked'] = checked
    unreal.log('NPC_JUMP_ANIMATION_REPORT ' + json.dumps(report, ensure_ascii=False))


if __name__ == '__main__':
    flags = unreal.SystemLibrary.get_command_line().lower().split()
    main('-retargetnpcjumpanimations' in flags, '-installnpcjumpanimations' in flags)
