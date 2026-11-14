"""导入已下载的 Quaternius 成品动作，再用 UE 原生 IK Retargeter 适配二狗

默认 / -InspectCompanionHomeAnimations：只读 GLB、许可、已导入 clip、真实骨名与姿势
-ImportCompanionHomeAnimationSources：AssetImportTask(GLB) 导入独立 Source 目录
-RetargetCompanionHomeAnimations -HomeAnimationManifest="..."：使用确认后的实际路径与骨名重定向
-ImportAlignedCompanionHomeAnimationSources：原生导入偏航 -90 度的独立源副本
-RebuildAlignedCompanionHomeAnimations -HomeAnimationManifest="..."：备份并重建本批五个动作
-InspectAlignedCompanionHomeAnimations：只读独立对齐源及前向校验

三个阶段分开运行，先读取导入结果再确认 manifest.imported_names_confirmed=true
只使用 UE IK Rig / IK Retargeter / run_batch_retarget，不编写或生成肢体动作轨道
不改原人物、Skeleton、居民、UI、地图、声音或既有骑乘动作，不运行 PIE / BeginPlay
"""
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import struct

import unreal


ROOT = '/Game/NPC/Companion/Home'
SOURCE_ROOT = ROOT + '/Source/Quaternius'
ALIGNED_SOURCE_ROOT = ROOT + '/Source/QuaterniusAligned'
RIG_ROOT = ROOT + '/Retarget'
ALIGNED_RIG_ROOT = RIG_ROOT + '/Aligned'
TARGET_ROOT = '/Game/NPC/Companion/Animation'
COMPANION = '/Game/NPC/Companion/BP_Companion_Ergou'
SOURCE_TAG = 'DeliveryHomeAnimationSource'
RETARGET_TAG = 'DeliveryHomeAnimationRetarget'
VERSION = '1'
ALIGNMENT_VERSION = 'source_y_forward_to_x_forward_v1'
ALIGNMENT_YAW = -90.0
ORIENTATION_EVIDENCE = 'Saved/AIResearch/HomeObjects/AnimationOrientation/20261114T104335416049Z/report.json'
LIBRARIES = {
    'Library1': {'file': 'Design/Art/NPC/Grandpa/Quaternius_AnimationLibrary_Standard.glb',
                 'license': 'Design/Art/NPC/Grandpa/Quaternius_License.txt',
                 'url': 'https://quaternius.com/packs/universalanimationlibrary.html'},
    'Library2': {'file': 'Design/Art/NPC/Grandpa/Quaternius_AnimationLibrary2_Standard.glb',
                 'license': 'Design/Art/NPC/Grandpa/Quaternius2_License.txt',
                 'url': 'https://quaternius.com/packs/universalanimationlibrary2.html'},
}
CLIPS = {
    'SitDown': ('Library1', 'Sitting_Enter', 'A_ErgouSitDown'),
    'Sit': ('Library1', 'Sitting_Idle_Loop', 'A_ErgouSit'),
    'StandUp': ('Library1', 'Sitting_Exit', 'A_ErgouStandUp'),
    'PickUp': ('Library1', 'PickUp_Table', 'A_ErgouPickUp'),
    'Carry': ('Library2', 'Walk_Carry_Loop', 'A_ErgouCarry'),
}
LIBRARY1_CHAINS = {
    # 本机 Interchange 实际导入将骨名中的点替换为下划线，原 GLB 保持不变
    'Spine': ['DEF-spine_001', 'DEF-spine_003'], 'Head': ['DEF-neck', 'DEF-head'],
    'Clavicle_L': ['DEF-shoulder_L', 'DEF-shoulder_L'], 'Arm_L': ['DEF-upper_arm_L', 'DEF-hand_L'],
    'Clavicle_R': ['DEF-shoulder_R', 'DEF-shoulder_R'], 'Arm_R': ['DEF-upper_arm_R', 'DEF-hand_R'],
    'Leg_L': ['DEF-thigh_L', 'DEF-foot_L'], 'Leg_R': ['DEF-thigh_R', 'DEF-foot_R'],
    'Toe_L': ['DEF-toe_L', 'DEF-toe_L'], 'Toe_R': ['DEF-toe_R', 'DEF-toe_R'],
}
LIBRARY2_CHAINS = {
    'Spine': ['spine_01', 'spine_03'], 'Head': ['neck_01', 'Head'],
    'Clavicle_L': ['clavicle_l', 'clavicle_l'], 'Arm_L': ['upperarm_l', 'hand_l'],
    'Clavicle_R': ['clavicle_r', 'clavicle_r'], 'Arm_R': ['upperarm_r', 'hand_r'],
    'Leg_L': ['thigh_l', 'foot_l'], 'Leg_R': ['thigh_r', 'foot_r'],
    'Toe_L': ['ball_l', 'ball_l'], 'Toe_R': ['ball_r', 'ball_r'],
}
TARGET_CHAINS = {
    'Spine': ['Spine', 'Spine2'], 'Head': ['Neck', 'Head'],
    'Clavicle_L': ['LeftShoulder', 'LeftShoulder'], 'Arm_L': ['LeftArm', 'LeftHand'],
    'Clavicle_R': ['RightShoulder', 'RightShoulder'], 'Arm_R': ['RightArm', 'RightHand'],
    'Leg_L': ['LeftUpLeg', 'LeftFoot'], 'Leg_R': ['RightUpLeg', 'RightFoot'],
    'Toe_L': ['LeftToeBase', 'LeftToeBase'], 'Toe_R': ['RightToeBase', 'RightToeBase'],
}


def object_path(value):
    return value.get_path_name() if value else None


def package_name(path):
    return path.split('.', 1)[0]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def asset_file(project, path):
    package = package_name(path)
    if not package.startswith('/Game/'):
        raise RuntimeError('只处理本项目资产：' + path)
    result = (project/'Content'/(package[6:]+'.uasset')).resolve()
    if not result.is_relative_to((project/'Content').resolve()):
        raise RuntimeError('资产路径超出 Content')
    return result


def save_asset(asset):
    if not unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False):
        raise RuntimeError('保存失败：' + object_path(asset))


def editor_guard(write=False):
    subsystem = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    # GetGameWorld 只查询 PIE WorldContext，支持无视口的 Python commandlet
    if not subsystem:
        raise RuntimeError('缺少 UnrealEditorSubsystem，停止资源操作')
    if subsystem.get_game_world():
        raise RuntimeError('PIE 或游戏世界存在，停止资源操作')
    if write:
        util = unreal.EditorLoadingAndSavingUtils
        dirty = list(util.get_dirty_map_packages())+list(util.get_dirty_content_packages())
        if dirty:
            raise RuntimeError('存在未保存改动，停止写入：' + str([object_path(item) for item in dirty]))


def read_glb(path):
    data = path.read_bytes()
    if len(data) < 20 or data[:4] != b'glTF' or struct.unpack_from('<I', data, 4)[0] != 2:
        raise RuntimeError('不是有效 GLB 2：' + str(path))
    cursor = 12
    document = None
    while cursor+8 <= len(data):
        length, kind = struct.unpack_from('<II', data, cursor)
        cursor += 8
        if cursor+length > len(data):
            raise RuntimeError('GLB chunk 越界')
        if kind == 0x4E4F534A:
            document = json.loads(data[cursor:cursor+length].decode('utf-8').rstrip('\x00 \r\n'))
            break
        cursor += length
    if not document:
        raise RuntimeError('GLB 缺少 JSON chunk')
    nodes = document.get('nodes', [])
    joints = sorted({index for skin in document.get('skins', []) for index in skin.get('joints', [])})
    return {'animations': [item.get('name', '') for item in document.get('animations', [])],
            'skin_bones': [nodes[index].get('name', '') for index in joints],
            'mesh_count': len(document.get('meshes', [])), 'skin_count': len(document.get('skins', []))}


def local_sources(project):
    result = {}
    for key, source in LIBRARIES.items():
        file, license_file = project/source['file'], project/source['license']
        if not file.is_file() or not license_file.is_file():
            raise RuntimeError('缺少既有源文件或其随包许可：' + key)
        license_text = license_file.read_text(encoding='utf-8-sig')
        if 'CC0 1.0' not in license_text or 'Quaternius' not in license_text:
            raise RuntimeError('本包许可内容与已确认 CC0 来源不符：' + key)
        document = read_glb(file)
        for library, clip, _ in CLIPS.values():
            if library == key and clip not in document['animations']:
                raise RuntimeError('原 GLB 未包含指定成品动作：' + clip)
        result[key] = {**source, 'absolute_file': str(file), 'absolute_license': str(license_file),
                       'sha256': digest(file), 'license_sha256': digest(license_file),
                       'author': 'Quaternius', 'license_name': 'CC0 1.0 Universal', 'glb': document}
    return result


def target_data():
    cls = unreal.EditorAssetLibrary.load_blueprint_class(COMPANION)
    if not cls:
        raise RuntimeError('缺少二狗蓝图')
    npc = unreal.get_default_object(cls)
    mesh = npc.get_editor_property('mesh').get_editor_property('skeletal_mesh_asset')
    idle = npc.get_editor_property('presentation').get_editor_property('idle_animation')
    if not mesh or not idle or mesh.get_editor_property('skeleton') != idle.get_editor_property('skeleton'):
        raise RuntimeError('当前二狗模型和 Idle 骨架不一致')
    return mesh, idle


def pose_snapshot(animation, mesh, samples=False):
    if not isinstance(animation, unreal.AnimSequence) or animation.get_editor_property('skeleton') != mesh.get_editor_property('skeleton'):
        raise RuntimeError('动画与预览网格骨架不符')
    duration = animation.get_play_length()
    if not math.isfinite(duration) or duration <= 0:
        raise RuntimeError('动画时长无效')
    options = unreal.AnimPoseEvaluationOptions()
    options.set_editor_property('evaluation_type', unreal.AnimDataEvalType.COMPRESSED)
    options.set_editor_property('optional_skeletal_mesh', mesh)
    first = unreal.AnimPoseExtensions.get_anim_pose_at_time(animation, 0, options)
    names = [str(bone) for bone in unreal.AnimPoseExtensions.get_bone_names(first)]
    result = {'path': object_path(animation), 'skeleton': object_path(animation.get_editor_property('skeleton')),
              'length': duration, 'root_motion': bool(animation.get_editor_property('enable_root_motion')), 'bones': names}
    if samples:
        selected = [name for name in names if name in {'Hips', 'Head', 'LeftUpLeg', 'LeftLeg', 'LeftFoot', 'LeftToeBase',
                    'RightUpLeg', 'RightLeg', 'RightFoot', 'RightToeBase', 'LeftArm', 'LeftForeArm', 'LeftHand', 'RightArm', 'RightForeArm', 'RightHand'}]
        result['samples'] = []
        for fraction in (0, .25, .5, .75, 1):
            pose = unreal.AnimPoseExtensions.get_anim_pose_at_time(animation, duration*fraction, options)
            points = {}
            for name in selected:
                value = unreal.AnimPoseExtensions.get_bone_pose(pose, name, unreal.AnimPoseSpaces.WORLD)
                points[name] = {'position': [value.translation.x, value.translation.y, value.translation.z],
                                'scale': [value.scale3d.x, value.scale3d.y, value.scale3d.z]}
            result['samples'].append({'time': duration*fraction, 'bones_mesh_cm': points})
    return result


def list_assets(path):
    if not unreal.EditorAssetLibrary.does_directory_exist(path):
        return []
    return [asset for value in unreal.EditorAssetLibrary.list_assets(path, recursive=True, include_folder=False)
            if (asset := unreal.load_asset(value))]


def imported_clip_names(library, clip):
    # 原生导入将文件名直接接在 clip 前，本次报告确认两者之间没有下划线
    prefix = Path(LIBRARIES[library]['file']).stem
    return {clip, prefix+clip, prefix+'_'+clip}


def inspect_sources(source_root=SOURCE_ROOT):
    result = {}
    for key in LIBRARIES:
        assets = list_assets(source_root+'/'+key)
        meshes = [asset for asset in assets if isinstance(asset, unreal.SkeletalMesh)]
        animations = [asset for asset in assets if isinstance(asset, unreal.AnimSequence)]
        entry = {'assets': [{'path': object_path(asset), 'class': asset.get_class().get_path_name()} for asset in assets],
                 'meshes': [{'path': object_path(mesh), 'skeleton': object_path(mesh.get_editor_property('skeleton'))} for mesh in meshes],
                 'animations': [], 'clip_candidates': {}}
        for animation in animations:
            compatible = [mesh for mesh in meshes if mesh.get_editor_property('skeleton') == animation.get_editor_property('skeleton')]
            item = {'path': object_path(animation), 'name': animation.get_name(), 'length': animation.get_play_length(),
                    'skeleton': object_path(animation.get_editor_property('skeleton')), 'compatible_meshes': [object_path(mesh) for mesh in compatible]}
            if len(compatible) == 1 and not entry.get('bones'):
                entry['bones'] = pose_snapshot(animation, compatible[0])['bones']
            entry['animations'].append(item)
        for role, (library, clip, _) in CLIPS.items():
            if library != key:
                continue
            # 这里只给出候选，不据此自动选中或重定向；manifest 仍需确认真实完整路径
            entry['clip_candidates'][role] = [item['path'] for item in entry['animations'] if item['name'] in imported_clip_names(key, clip)]
        result[key] = entry
    return result


def import_sources(project, source_info, aligned=False):
    imported = {}
    for key, source in source_info.items():
        folder = (ALIGNED_SOURCE_ROOT if aligned else SOURCE_ROOT)+'/'+key
        existing = list_assets(folder)
        if existing:
            if any(unreal.EditorAssetLibrary.get_metadata_tag(asset, SOURCE_TAG) != VERSION or
                   unreal.EditorAssetLibrary.get_metadata_tag(asset, 'SourceSHA256') != source['sha256'] or
                   (aligned and unreal.EditorAssetLibrary.get_metadata_tag(asset, 'SourceBasisAlignment') != ALIGNMENT_VERSION)
                   for asset in existing):
                raise RuntimeError('已有 Source 目录含未知或不同来源资产，保留原件：' + folder)
            imported[key] = {'status': 'existing_source_preserved', 'assets': [object_path(asset) for asset in existing]}
            continue
        pipeline = unreal.InterchangeGenericAssetsPipeline()
        pipeline.set_editor_property('ImportOffsetTranslation', unreal.Vector(0, 0, 0))
        pipeline.set_editor_property('ImportOffsetRotation', unreal.Rotator(pitch=0, yaw=ALIGNMENT_YAW if aligned else 0, roll=0))
        pipeline.set_editor_property('ImportOffsetUniformScale', 1.0)
        shared = pipeline.get_editor_property('CommonSkeletalMeshesAndAnimationsProperties')
        shared.set_editor_property('bImportOnlyAnimations', False)
        shared.set_editor_property('bTryAutoSelectSkeleton', False)
        shared.set_editor_property('Skeleton', None)
        meshes = pipeline.get_editor_property('MeshPipeline')
        meshes.set_editor_property('bImportStaticMeshes', False)
        meshes.set_editor_property('bImportSkeletalMeshes', True)
        meshes.set_editor_property('bCreatePhysicsAsset', False)
        meshes.set_editor_property('bUpdateSkeletonReferencePose', False)
        pipeline.get_editor_property('AnimationPipeline').set_editor_property('bImportAnimations', True)
        overrides = unreal.InterchangePipelineStackOverride()
        overrides.add_pipeline(pipeline)
        task = unreal.AssetImportTask()
        task.filename = source['absolute_file']; task.destination_path = folder
        task.automated = True; task.save = False; task.replace_existing = False
        task.set_editor_property('async_', False)
        task.options = overrides
        # 持有 pipeline/override 直到同步导入完成，避免使用编辑器上次导入选项
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        paths = list(task.imported_object_paths)
        if not paths:
            raise RuntimeError('GLB 原生导入未返回任何资产：' + key)
        for path in paths:
            if not package_name(path).startswith(folder+'/'):
                raise RuntimeError('GLB 导入结果越出独立 Source 目录：' + path)
            asset = unreal.load_asset(path)
            if not asset:
                raise RuntimeError('无法加载新导入结果：' + path)
            for name, value in {SOURCE_TAG: VERSION, 'SourceSHA256': source['sha256'],
                'SourceFile': source['file'], 'SourceAuthor': source['author'], 'SourceURL': source['url'],
                'SourceLicense': source['license_name'], 'SourceLicenseFile': source['license'],
                'SourceLicenseSHA256': source['license_sha256']}.items():
                unreal.EditorAssetLibrary.set_metadata_tag(asset, name, value)
            if aligned:
                unreal.EditorAssetLibrary.set_metadata_tag(asset, 'SourceBasisAlignment', ALIGNMENT_VERSION)
                unreal.EditorAssetLibrary.set_metadata_tag(asset, 'ImportOffsetYaw', str(ALIGNMENT_YAW))
            save_asset(asset)
        imported[key] = {'status': 'imported_source_only', 'assets': paths,
                          'import_offset_translation': [0, 0, 0],
                          'import_offset_rotation_pitch_yaw_roll': [0, ALIGNMENT_YAW if aligned else 0, 0], 'import_scale': 1}
    return imported


def draft_manifest(source_info, inventory, target, idle, aligned=False):
    libraries = {}
    for key, info in source_info.items():
        meshes = inventory[key]['meshes']
        libraries[key] = {'source_sha256': info['sha256'], 'source_mesh': meshes[0]['path'] if len(meshes) == 1 else None,
                          'root': 'DEF-hips' if key == 'Library1' else 'pelvis',
                          'chains': dict(LIBRARY1_CHAINS if key == 'Library1' else LIBRARY2_CHAINS)}
    clips = {}
    for role, (key, clip, target_name) in CLIPS.items():
        found = inventory[key]['clip_candidates'].get(role, [])
        clips[role] = {'library': key, 'original_clip': clip, 'source_animation': found[0] if len(found) == 1 else None,
                       'target_path': TARGET_ROOT+'/'+target_name}
    result = {'schema_version': 2 if aligned else 1, 'imported_names_confirmed': False,
            'target_mesh': object_path(target), 'target_skeleton': object_path(target.get_editor_property('skeleton')),
            'target_reference_animation': object_path(idle), 'target_root': 'Hips', 'target_chains': dict(TARGET_CHAINS),
            'libraries': libraries, 'clips': clips,
            'notes': '核对 source_mesh/clip 的完整路径及实际导入骨名后确认；只使用作者现成动作和 UE 原生重定向'}
    if aligned:
        result.update(source_basis_alignment=ALIGNMENT_VERSION, import_offset_yaw=ALIGNMENT_YAW,
                      orientation_evidence=ORIENTATION_EVIDENCE)
        project = Path(unreal.Paths.project_dir()).resolve()
        for role, clip in result['clips'].items():
            existing = unreal.load_asset(clip['target_path'])
            file = asset_file(project, clip['target_path'])
            clip['replace_existing'] = {'asset_sha256': digest(file) if file.is_file() else None,
                'source_animation': unreal.EditorAssetLibrary.get_metadata_tag(existing, 'SourceAnimation') if existing else None}
    return result


def backup(project, output, paths):
    result = {}
    for path in paths:
        file = asset_file(project, path)
        for suffix in ('.uasset', '.uexp', '.ubulk'):
            source = file.with_suffix(suffix)
            if source.is_file():
                destination = output/'Backup'/source.relative_to(project)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, destination)
                result[str(source)] = {'sha256': digest(source), 'backup': str(destination)}
    return result


def body_orientation(animation, mesh, library, reference=False):
    """读取作者动作的真实方向，比较单位方向时不受目标参考根缩放影响"""
    names = {
        'Library1': ['DEF-hips', 'DEF-thigh_L', 'DEF-thigh_R', 'DEF-shin_L', 'DEF-shin_R',
                     'DEF-foot_L', 'DEF-foot_R', 'DEF-toe_L', 'DEF-toe_R', 'DEF-hand_L', 'DEF-hand_R',
                     'DEF-upper_arm_L', 'DEF-forearm_L', 'DEF-upper_arm_R', 'DEF-forearm_R'],
        'Library2': ['pelvis', 'thigh_l', 'thigh_r', 'calf_l', 'calf_r',
                     'foot_l', 'foot_r', 'ball_l', 'ball_r', 'hand_l', 'hand_r',
                     'upperarm_l', 'lowerarm_l', 'upperarm_r', 'lowerarm_r'],
        'Target': ['Hips', 'LeftUpLeg', 'RightUpLeg', 'LeftLeg', 'RightLeg',
                   'LeftFoot', 'RightFoot', 'LeftToeBase', 'RightToeBase', 'LeftHand', 'RightHand',
                   'LeftArm', 'LeftForeArm', 'RightArm', 'RightForeArm'],
    }[library]
    options = unreal.AnimPoseEvaluationOptions()
    options.set_editor_property('evaluation_type', unreal.AnimDataEvalType.COMPRESSED)
    options.set_editor_property('optional_skeletal_mesh', mesh)
    def sub(a, b):
        return [x-y for x, y in zip(a, b)]
    def unit(value):
        length = math.sqrt(sum(x*x for x in value))
        if not all(math.isfinite(x) for x in value) or not math.isfinite(length) or length < 1.e-6:
            raise RuntimeError('骨点距离为零，不能检查方向')
        return [x/length for x in value]
    samples = []
    for fraction in ((0.,) if reference else (0., .5, 1.)):
        pose = unreal.AnimPoseExtensions.get_anim_pose_at_time(animation, animation.get_play_length()*fraction, options)
        reader = unreal.AnimPoseExtensions.get_ref_bone_pose if reference else unreal.AnimPoseExtensions.get_bone_pose
        points = []
        for name in names:
            value = reader(pose, name, unreal.AnimPoseSpaces.WORLD).translation
            if not all(math.isfinite(x) for x in (value.x, value.y, value.z)):
                raise RuntimeError('动画骨点包含非有限数，停止方向校验：'+object_path(animation)+' / '+name)
            points.append([value.x, value.y, value.z])
        hips, lh, rh, lk, rk, lf, rf, lt, rt, left_hand, right_hand, la, le, ra, re = points
        hip_axis = sub(rh, lh)
        right = unit([hip_axis[0], hip_axis[1], 0.])
        forward = [right[1], -right[0], 0.]
        feet = [a+b for a, b in zip(sub(lt, lf), sub(rt, rf))]
        def body(value):
            return [sum(a*b for a, b in zip(value, forward)),
                    sum(a*b for a, b in zip(value, right)), value[2]]
        samples.append({'fraction': fraction, 'forward': forward, 'right': right,
                        'feet_forward': unit([feet[0], feet[1], 0.]),
                        'left_thigh_direction': unit(body(sub(lk, lh))),
                        'right_thigh_direction': unit(body(sub(rk, rh))),
                        'left_upper_arm_direction': unit(body(sub(le, la))),
                        'right_upper_arm_direction': unit(body(sub(re, ra))),
                        'left_forearm_direction': unit(body(sub(left_hand, le))),
                        'right_forearm_direction': unit(body(sub(right_hand, re))),
                        'left_hand_from_hips': body(sub(left_hand, hips)),
                        'right_hand_from_hips': body(sub(right_hand, hips))})
    return samples


def verify_aligned_sources(inventory, target, idle):
    """必须实际读回源骨架朝向，导入设置写入成功不等于方向已经生效"""
    target_ref = body_orientation(idle, target, 'Target', True)[0]
    result = {}
    for key in LIBRARIES:
        entry = inventory[key]
        if len(entry['meshes']) != 1:
            raise RuntimeError('Aligned Source 未返回唯一网格：'+key)
        role = 'Sit' if key == 'Library1' else 'Carry'
        clips = entry['clip_candidates'][role]
        if len(clips) != 1:
            raise RuntimeError('Aligned Source 动作真实名称不唯一：'+role)
        mesh, clip = unreal.load_asset(entry['meshes'][0]['path']), unreal.load_asset(clips[0])
        ref = body_orientation(clip, mesh, key, True)[0]
        dots = {axis: sum(a*b for a, b in zip(ref[axis], target_ref[axis]))
                for axis in ('forward', 'right', 'feet_forward')}
        result[key] = {'reference': ref, 'target_reference': target_ref, 'basis_dots': dots,
                       'authored_motion': body_orientation(clip, mesh, key)}
        if not all(math.isfinite(value) and value >= .999 for value in dots.values()):
            raise RuntimeError('Aligned Source 与目标参考前向不符，保留输出供检查：'+str(result[key]))
    return result


def verify_retarget_direction(role, source, source_mesh, target_clip, target_mesh, library):
    source_samples = body_orientation(source, source_mesh, library)
    target_samples = body_orientation(target_clip, target_mesh, 'Target')
    result = {'source': source_samples, 'target': target_samples, 'checks': []}
    for a, b in zip(source_samples, target_samples):
        check = {'fraction': a['fraction']}
        # 五段动作首中末均对照作者肢段方向，允许不同体态的偏差但拒绝约90度坐标错转
        for part in ('left_thigh', 'right_thigh', 'left_upper_arm', 'right_upper_arm', 'left_forearm', 'right_forearm'):
            key = part+'_direction'
            check[part+'_dot'] = sum(x*y for x, y in zip(a[key], b[key]))
            if not math.isfinite(check[part+'_dot']) or check[part+'_dot'] < math.cos(math.radians(30.)):
                raise RuntimeError('重定向肢段偏离作者方向超过30度：'+str({'role': role, 'part': part, 'source': a, 'target': b}))
        if role == 'Sit':
            # 原坐姿双腿朝前且略张开，防止把相同坏剪辑和图输出互相当作证明
            for side in ('left', 'right'):
                key = side+'_thigh_direction'
                check[side+'_thigh_dot'] = sum(x*y for x, y in zip(a[key], b[key]))
                if not math.isfinite(check[side+'_thigh_dot']) or check[side+'_thigh_dot'] < .9 or b[key][0] < .8:
                    raise RuntimeError('重定向坐姿大腿方向偏离作者原动作：'+str({'source': a, 'target': b}))
        if role == 'Carry':
            # 身体比例不同，手间距离不硬套源人物，但两手必须仍在身体前方且保持左右
            for side, sign in (('left', -1), ('right', 1)):
                hand = b[side+'_hand_from_hips']
                check[side+'_hand_forward'] = hand[0]
                if hand[0] <= 0 or hand[1]*sign <= 0:
                    raise RuntimeError('重定向搬运动作手部未保持前方和左右关系：'+str({'source': a, 'target': b}))
        result['checks'].append(check)
    result['checked'] = True
    return result


def preflight_aligned_rebuild(manifest, source_info, project):
    """覆盖前同时核对五个旧结果，拒绝未知动作、变更过的输出及不同源文件"""
    if manifest.get('schema_version') != 2 or manifest.get('source_basis_alignment') != ALIGNMENT_VERSION or manifest.get('import_offset_yaw') != ALIGNMENT_YAW:
        raise RuntimeError('Aligned manifest 坐标修正版本不符')
    for role, (key, original_name, target_name) in CLIPS.items():
        config = manifest['clips'][role]
        expected = TARGET_ROOT+'/'+target_name
        if config['target_path'] != expected:
            raise RuntimeError('只能覆盖本批指定五个目标动作')
        old = unreal.load_asset(expected)
        replacement = config.get('replace_existing', {})
        file = asset_file(project, expected)
        old_source = unreal.EditorAssetLibrary.get_metadata_tag(old, 'SourceAnimation') if old else None
        valid_sources = {SOURCE_ROOT+'/'+key+'/'+name+'.'+name for name in imported_clip_names(key, original_name)}
        valid_sources.add(config['source_animation'])
        if (not isinstance(old, unreal.AnimSequence) or not file.is_file() or
            replacement.get('asset_sha256') != digest(file) or replacement.get('source_animation') != old_source or
            old_source not in valid_sources or unreal.EditorAssetLibrary.get_metadata_tag(old, RETARGET_TAG) != VERSION or
            unreal.EditorAssetLibrary.get_metadata_tag(old, 'SourceSHA256') != source_info[key]['sha256'] or
            unreal.EditorAssetLibrary.get_metadata_tag(old, 'SourceClip') != original_name):
            raise RuntimeError('旧目标的来源、归属或确认后文件 SHA 已变，停止整批覆盖：'+expected)


def make_rig(path, mesh, root, chains, actual_bones):
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        asset = unreal.load_asset(path)
        if unreal.EditorAssetLibrary.get_metadata_tag(asset, RETARGET_TAG) != VERSION:
            raise RuntimeError('已有 IK Rig 不是本批资产，停止覆盖')
        controller = unreal.IKRigController.get_controller(asset)
        if controller.get_skeletal_mesh() != mesh or str(controller.get_retarget_root()) != root:
            raise RuntimeError('已有 IK Rig 网格或骨盆配置变化，保留资产')
        for name, (start, end) in chains.items():
            if str(controller.get_retarget_chain_start_bone(name)) != start or str(controller.get_retarget_chain_end_bone(name)) != end:
                raise RuntimeError('已有 IK Rig 链配置变化，保留资产')
        return asset
    required = {root} | {bone for pair in chains.values() for bone in pair}
    if not required.issubset(set(actual_bones)):
        raise RuntimeError('实际导入骨名不匹配，先 Inspect 更新 manifest：' + str(sorted(required-set(actual_bones))))
    folder, name = path.rsplit('/', 1)
    asset = unreal.IKRigDefinitionFactory.create_new_ik_rig_asset(folder, name)
    if not asset or package_name(object_path(asset)) != path:
        raise RuntimeError('IK Rig 创建未返回指定独立路径')
    controller = unreal.IKRigController.get_controller(asset)
    if not controller.set_skeletal_mesh(mesh) or not controller.set_retarget_root(root):
        raise RuntimeError('IK Rig 网格或骨盆设置失败')
    for name, (start, end) in chains.items():
        if str(controller.add_retarget_chain(name, start, end, 'None')) != name:
            raise RuntimeError('IK Rig 链创建失败或被重命名：' + name)
    unreal.EditorAssetLibrary.set_metadata_tag(asset, RETARGET_TAG, VERSION)
    save_asset(asset)
    return asset


def make_retargeter(path, source_rig, target_rig, source_mesh, target_mesh, chain_names):
    source_side, target_side = unreal.RetargetSourceOrTarget.SOURCE, unreal.RetargetSourceOrTarget.TARGET
    asset = unreal.load_asset(path)
    if asset:
        if unreal.EditorAssetLibrary.get_metadata_tag(asset, RETARGET_TAG) != VERSION:
            raise RuntimeError('已有 Retargeter 不是本批资产，停止覆盖')
        controller = unreal.IKRetargeterController.get_controller(asset)
        if controller.get_ik_rig(source_side) != source_rig or controller.get_ik_rig(target_side) != target_rig:
            raise RuntimeError('已有 Retargeter 的 IK Rig 已变化，保留编辑结果')
    else:
        folder, name = path.rsplit('/', 1)
        asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, folder, unreal.IKRetargeter, unreal.IKRetargetFactory())
        if not asset:
            raise RuntimeError('IK Retargeter 创建失败')
        controller = unreal.IKRetargeterController.get_controller(asset)
        controller.set_ik_rig(source_side, source_rig); controller.set_ik_rig(target_side, target_rig)
        controller.set_preview_mesh(source_side, source_mesh); controller.set_preview_mesh(target_side, target_mesh)
        controller.add_default_ops()
        controller.auto_map_chains(unreal.AutoMapChainType.EXACT, True)
        controller.auto_align_all_bones(target_side, unreal.RetargetAutoAlignMethod.CHAIN_TO_CHAIN)
        unreal.EditorAssetLibrary.set_metadata_tag(asset, RETARGET_TAG, VERSION)
    for chain in chain_names:
        if str(controller.get_source_chain(chain)) != chain:
            raise RuntimeError('原生重定向未建立明确同名链映射：' + chain)
    save_asset(asset)
    return asset


def run_native_retarget(source, source_mesh, target_mesh, retargeter, folder, name, overwrite=False):
    inputs = unreal.IKRetargetBatchOperationInputs()
    inputs.assets_to_retarget = [unreal.EditorAssetLibrary.find_asset_data(object_path(source))]
    inputs.source_mesh = source_mesh
    inputs.target_mesh = target_mesh
    inputs.ik_retarget_asset = retargeter
    inputs.search = source.get_name()
    inputs.replace = name
    inputs.prefix = ''
    inputs.suffix = ''
    inputs.target_path = folder
    inputs.use_source_path = False
    inputs.include_referenced_assets = False
    inputs.overwrite_existing_files = overwrite
    inputs.retain_additive_flags = True
    output = list(unreal.IKRetargetBatchOperation.run_batch_retarget(inputs))
    if len(output) != 1:
        raise RuntimeError('原生批量重定向结果数量异常：'+name)
    asset = output[0].get_asset()
    if not isinstance(asset, unreal.AnimSequence) or package_name(object_path(asset)) != folder+'/'+name:
        raise RuntimeError('原生重定向返回了不同目标路径：'+str(object_path(asset)))
    if abs(asset.get_play_length()-source.get_play_length()) > 1/30:
        raise RuntimeError('重定向时长与作者原剪辑不符')
    return asset


def set_retarget_metadata(asset, source, source_info, original_name, retargeter, aligned):
    # Actor 位移仍由 CMC 执行，保留源剪辑骨轨道，只关闭根运动提取
    asset.set_editor_property('enable_root_motion', False)
    for name, value in {RETARGET_TAG: VERSION, 'SourceAnimation': object_path(source), 'SourceClip': original_name,
                        'SourceSHA256': source_info['sha256'], 'SourceAuthor': 'Quaternius',
                        'SourceLicense': 'CC0 1.0 Universal', 'SourceLicenseFile': source_info['license'],
                        'SourceLicenseSHA256': source_info['license_sha256'], 'SourceURL': source_info['url'],
                        'NativeRetargeter': object_path(retargeter)}.items():
        unreal.EditorAssetLibrary.set_metadata_tag(asset, name, value)
    if aligned:
        unreal.EditorAssetLibrary.set_metadata_tag(asset, 'SourceBasisAlignment', ALIGNMENT_VERSION)


def retarget(manifest, source_info, target, idle, aligned=False, results=None):
    if manifest.get('schema_version') != (2 if aligned else 1) or manifest.get('imported_names_confirmed') is not True:
        raise RuntimeError('先确认导入后真实命名的 manifest')
    if manifest['target_mesh'] != object_path(target) or manifest['target_skeleton'] != object_path(target.get_editor_property('skeleton')):
        raise RuntimeError('目标二狗模型或 Skeleton 已变化')
    if manifest['target_reference_animation'] != object_path(idle):
        raise RuntimeError('目标二狗参考动画已变化')
    if set(manifest['clips']) != set(CLIPS):
        raise RuntimeError('成品动作清单不完整')
    if set(manifest['libraries']) != set(LIBRARIES):
        raise RuntimeError('两套独立源骨架清单不完整')
    source_root = ALIGNED_SOURCE_ROOT if aligned else SOURCE_ROOT
    rig_root = ALIGNED_RIG_ROOT if aligned else RIG_ROOT
    if aligned:
        preflight_aligned_rebuild(manifest, source_info, Path(unreal.Paths.project_dir()).resolve())
        verify_aligned_sources(inspect_sources(source_root), target, idle)
        for config in manifest['libraries'].values():
            mesh = unreal.load_asset(config['source_mesh'])
            if (not mesh or unreal.EditorAssetLibrary.get_metadata_tag(mesh, SOURCE_TAG) != VERSION or
                unreal.EditorAssetLibrary.get_metadata_tag(mesh, 'SourceBasisAlignment') != ALIGNMENT_VERSION):
                raise RuntimeError('Aligned Source 网格缺少本批坐标修正归属')
        for config in manifest['clips'].values():
            clip = unreal.load_asset(config['source_animation'])
            if (not clip or unreal.EditorAssetLibrary.get_metadata_tag(clip, SOURCE_TAG) != VERSION or
                unreal.EditorAssetLibrary.get_metadata_tag(clip, 'SourceBasisAlignment') != ALIGNMENT_VERSION):
                raise RuntimeError('Aligned Source 动作缺少本批坐标修正归属')
    target_bones = pose_snapshot(idle, target)['bones']
    target_rig = make_rig(rig_root+'/IK_ErgouHome', target, manifest['target_root'], manifest['target_chains'], target_bones)
    retargeters = {}
    meshes = {}
    for key, config in manifest['libraries'].items():
        if key not in source_info or config['source_sha256'] != source_info[key]['sha256']:
            raise RuntimeError('manifest 与本包来源 SHA 不符')
        mesh = unreal.load_asset(config['source_mesh'])
        if not isinstance(mesh, unreal.SkeletalMesh) or not package_name(object_path(mesh)).startswith(source_root+'/'+key+'/'):
            raise RuntimeError('源网格必须是本批独立导入结果')
        examples = [clip for clip in manifest['clips'].values() if clip['library'] == key]
        animation = unreal.load_asset(examples[0]['source_animation'])
        bones = pose_snapshot(animation, mesh)['bones']
        if set(config['chains']) != set(manifest['target_chains']):
            raise RuntimeError('源/目标链名集合不一致')
        source_rig = make_rig(rig_root+'/IK_Home'+key, mesh, config['root'], config['chains'], bones)
        retargeters[key] = make_retargeter(rig_root+'/RTG_Home'+key+'ToErgou', source_rig, target_rig, mesh, target, config['chains'])
        meshes[key] = mesh
    results = results if results is not None else {}
    # 独立候选保留失败证据，全套方向检查通过前不覆盖已接入的五段动画
    candidate_root = (ALIGNED_RIG_ROOT+'/Candidates/'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')) if aligned else TARGET_ROOT
    sources = {}
    for role, clip in manifest['clips'].items():
        key, original_name, target_name = CLIPS[role]
        expected = TARGET_ROOT+'/'+target_name
        if clip['library'] != key or clip['original_clip'] != original_name or clip['target_path'] != expected:
            raise RuntimeError('clip 来源或目标路径与明确清单不符')
        source = unreal.load_asset(clip['source_animation'])
        if not isinstance(source, unreal.AnimSequence) or not package_name(object_path(source)).startswith(source_root+'/'+key+'/'):
            raise RuntimeError('源动画不在独立 GLB 导入目录')
        if unreal.EditorAssetLibrary.get_metadata_tag(source, 'SourceSHA256') != source_info[key]['sha256']:
            raise RuntimeError('源动画来源元数据不符')
        if source.get_name() not in imported_clip_names(key, original_name):
            raise RuntimeError('实际导入名称尚未匹配原 clip，请先核对 Import/Inspect 报告')
        if source.get_editor_property('skeleton') != meshes[key].get_editor_property('skeleton'):
            raise RuntimeError('该 clip 不是指定源 IK Rig 的 Skeleton')
        sources[role] = source
        if unreal.EditorAssetLibrary.does_asset_exist(expected) and not aligned:
            existing = unreal.load_asset(expected)
            if unreal.EditorAssetLibrary.get_metadata_tag(existing, RETARGET_TAG) != VERSION or unreal.EditorAssetLibrary.get_metadata_tag(existing, 'SourceAnimation') != object_path(source):
                raise RuntimeError('目标已有其他动画，保留原件：' + expected)
            results[role] = {'status': 'existing_preserved', 'readback': pose_snapshot(existing, target, True)}
            continue
        asset = run_native_retarget(source, meshes[key], target, retargeters[key], candidate_root, target_name)
        set_retarget_metadata(asset, source, source_info[key], original_name, retargeters[key], aligned)
        save_asset(asset)
        results[role] = {'status': 'candidate_created' if aligned else 'native_retargeted', 'readback': pose_snapshot(asset, target, True)}
        if aligned:
            results[role]['authored_direction_check'] = verify_retarget_direction(role, source, meshes[key], asset, target, key)
    if aligned:
        # 原生批处理覆盖会替换引用，只对再次确认过归属与文件 SHA 的五个动画启用
        preflight_aligned_rebuild(manifest, source_info, Path(unreal.Paths.project_dir()).resolve())
        for role, (key, original_name, target_name) in CLIPS.items():
            source = sources[role]
            asset = run_native_retarget(source, meshes[key], target, retargeters[key], TARGET_ROOT, target_name, True)
            set_retarget_metadata(asset, source, source_info[key], original_name, retargeters[key], True)
            save_asset(asset)
            results[role]['candidate_readback'] = results[role]['readback']
            results[role]['readback'] = pose_snapshot(asset, target, True)
            results[role]['published_direction_check'] = verify_retarget_direction(role, source, meshes[key], asset, target, key)
            results[role]['status'] = 'aligned_native_retargeted'
    return results


def main(import_source=False, run_retarget=False, manifest_file=None, aligned=False):
    editor_guard(import_source or run_retarget)
    if import_source and run_retarget:
        raise RuntimeError('先独立导入/Inspect，确认真实命名后再重定向')
    project = Path(unreal.Paths.project_dir()).resolve()
    output = project/'Saved/AIResearch/HomeObjects/AnimationImport'/datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output.mkdir(parents=True, exist_ok=False)
    report = {'status': 'preparing', 'import_source': import_source, 'run_retarget': run_retarget, 'aligned': aligned,
              'pie_started': False, 'map_saved': False, 'procedural_animation_generated': False}
    protected = [project/'Content/Level/TestForCharacter.umap']
    for folder in ('Characters', 'NPC/Residents', 'NPC/Grandpa', 'NPC/Companion/Appearance', 'UI'):
        protected.extend((project/'Content'/folder).rglob('*.uasset'))
    protected.extend([asset_file(project, COMPANION), asset_file(project, TARGET_ROOT+'/A_ErgouRide'),
                      asset_file(project, TARGET_ROOT+'/ABP_CompanionBody')])
    if aligned:
        protected.extend((project/'Content/NPC/Companion/Home/Source/Quaternius').rglob('*.uasset'))
        protected.extend((project/'Content/NPC/Companion/Home/Retarget').glob('*.uasset'))
        protected.append(project/ORIENTATION_EVIDENCE)
    if import_source:
        protected.extend(asset_file(project, TARGET_ROOT+'/'+item[2]) for item in CLIPS.values())
    before = {str(path): digest(path) for path in protected if path.is_file()}
    try:
        source_info = local_sources(project)
        report['sources'] = source_info
        for info in source_info.values():
            before[info['absolute_file']] = info['sha256']
            before[info['absolute_license']] = info['license_sha256']
        target, idle = target_data()
        report['target'] = {'mesh': object_path(target), 'idle': pose_snapshot(idle, target, True)}
        before[str(asset_file(project, object_path(idle)))] = digest(asset_file(project, object_path(idle)))
        source_root = ALIGNED_SOURCE_ROOT if aligned else SOURCE_ROOT
        rig_root = ALIGNED_RIG_ROOT if aligned else RIG_ROOT
        owned_paths = [object_path(asset) for asset in list_assets(source_root)+list_assets(rig_root)]
        if run_retarget:
            for asset in list_assets(source_root):
                path = asset_file(project, object_path(asset))
                if path.is_file():
                    before[str(path)] = digest(path)
        owned_paths.extend(TARGET_ROOT+'/'+item[2] for item in CLIPS.values())
        if import_source or run_retarget:
            report['backups'] = backup(project, output, owned_paths)
        if import_source:
            report['import_results'] = import_sources(project, source_info, aligned)
        inventory = inspect_sources(source_root)
        report['imported_sources'] = inventory
        draft = draft_manifest(source_info, inventory, target, idle, aligned)
        (output/'retarget_manifest_draft.json').write_text(json.dumps(draft, ensure_ascii=False, indent=2), encoding='utf-8')
        if aligned:
            report['source_basis_checks'] = verify_aligned_sources(inventory, target, idle)
        if run_retarget:
            if not manifest_file:
                raise RuntimeError('重定向必须显式指定已确认的 HomeAnimationManifest')
            manifest = json.loads(Path(manifest_file).read_text(encoding='utf-8-sig'))
            report['manifest'] = manifest
            report['retarget_results'] = {}
            retarget(manifest, source_info, target, idle, aligned, report['retarget_results'])
        report['status'] = 'retargeted_needs_visual_acceptance' if run_retarget else 'source_imported_needs_name_inspection' if import_source else 'inspected_read_only'
    except Exception as error:
        report.update(status='failed', error=repr(error))
        raise
    finally:
        after = {path: digest(Path(path)) for path in before}
        report.update(protected_before=before, protected_after=after, protected_unchanged=before == after)
        (output/'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        unreal.log('HOME_ANIMATION_IMPORT_REPORT=' + str(output/'report.json'))
        if before != after:
            raise RuntimeError('受保护源模型、骨架、Idle、居民、UI 或地图发生变化')


if __name__ == '__main__':
    command = unreal.SystemLibrary.get_command_line()
    lower = command.lower()
    importing = '-importcompanionhomeanimationsources' in lower
    retargeting = '-retargetcompanionhomeanimations' in lower
    aligned_import = '-importalignedcompanionhomeanimationsources' in lower
    aligned_rebuild = '-rebuildalignedcompanionhomeanimations' in lower
    aligned_inspect = '-inspectalignedcompanionhomeanimations' in lower
    if (aligned_import or aligned_rebuild) and (importing or retargeting):
        raise RuntimeError('原始源与 Aligned 源必须使用各自独立阶段')
    importing = importing or aligned_import
    retargeting = retargeting or aligned_rebuild
    if ('-inspectcompanionhomeanimations' in lower or aligned_inspect) and (importing or retargeting):
        raise RuntimeError('Inspect 不能和写入阶段并用')
    match = re.search(r'(?:^|\s)-HomeAnimationManifest=(?:"([^"]+)"|(\S+))', command, re.IGNORECASE)
    main(importing, retargeting, (match.group(1) or match.group(2)) if match else None, aligned_import or aligned_rebuild or aligned_inspect)
