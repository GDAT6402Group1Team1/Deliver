"""Blender 5：只把既有 Quaternius Punch_Jab 重定向到项目 The Boss 骨架

输出 A_NoodleBossEject.fbx，不导出网格、不修改原 FBX/Skeleton 或居民动画
源动作和目标 FBX 的对象坐标不同，先换算到同一世界方向，再保留目标骨长
"""
import bpy
import hashlib
import json
import math
from pathlib import Path
from mathutils import Vector, Quaternion

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'Design/Art/NPC/Grandpa/Quaternius_AnimationLibrary_Standard.glb'
TARGET = ROOT / 'Content/Characters/TestDeliveryMan/The Boss.fbx'
OUT = ROOT / 'Design/Art/NPC/Companion/Boss'
EVIDENCE = ROOT / 'Saved/AIResearch/NoodleBossRetarget'
OUT.mkdir(parents=True, exist_ok=True)
EVIDENCE.mkdir(parents=True, exist_ok=True)
protected_before = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in (SOURCE, TARGET)}
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=str(SOURCE))
source = next(obj for obj in bpy.data.objects if obj.type == 'ARMATURE')
action = bpy.data.actions.get('Punch_Jab')
if not action:
    raise RuntimeError('已有动画库缺少 Punch_Jab')
source_objects = list(bpy.data.objects)
for track in source.animation_data.nla_tracks:
    track.mute = True
source.animation_data.action = action
if action.slots:
    source.animation_data.action_slot = action.slots[0]
before = set(bpy.data.objects)
bpy.ops.import_scene.fbx(filepath=str(TARGET))
target_objects = list(set(bpy.data.objects) - before)
target = next(obj for obj in target_objects if obj.type == 'ARMATURE')
target.name = 'Armature'
target_meshes = [obj for obj in target_objects if obj.type == 'MESH']
if target.animation_data:
    for track in target.animation_data.nla_tracks:
        track.mute = True
    target.animation_data.action = None
# 现有 UE The_Boss_Skeleton 已剥除 mixamorig:，仅改导出副本的骨名和蒙皮组名
for bone in target.data.bones:
    bone.name = bone.name.removeprefix('mixamorig:')
for obj in target_meshes:
    for group in obj.vertex_groups:
        group.name = group.name.removeprefix('mixamorig:')
mapping = {'Hips': 'DEF-hips', 'Spine': 'DEF-spine.001', 'Spine1': 'DEF-spine.002',
           'Spine2': 'DEF-spine.003', 'Neck': 'DEF-neck', 'Head': 'DEF-head'}
for side, suffix in [('Left', 'L'), ('Right', 'R')]:
    for dst, src in [('Shoulder','shoulder'), ('Arm','upper_arm'), ('ForeArm','forearm'),
                     ('Hand','hand'), ('UpLeg','thigh'), ('Leg','shin'), ('Foot','foot'), ('ToeBase','toe')]:
        mapping[side+dst] = 'DEF-' + src + '.' + suffix
if any(name not in source.pose.bones for name in mapping.values()):
    raise RuntimeError('源动画骨骼映射不完整')
rest_source = {bone.name: bone.matrix_local.to_quaternion() for bone in source.data.bones}
rest_target = {bone.name: bone.matrix_local.to_quaternion() for bone in target.data.bones}
rest_world = {bone.name: list(target.matrix_world @ bone.head_local) for bone in target.data.bones}
alignment = target.matrix_world.to_quaternion().inverted() @ source.matrix_world.to_quaternion()
ordered = sorted(target.pose.bones, key=lambda bone: len(bone.parent_recursive))
target.animation_data_create()
baked = bpy.data.actions.new('A_NoodleBossEject')
target.animation_data.action = baked
duration = float(action.frame_range[1] - action.frame_range[0]) / 24.0
last_frame = round(duration * 30)
samples = []
for frame in range(last_frame + 1):
    source_frame = float(action.frame_range[0]) + frame / 30 * 24
    bpy.context.scene.frame_set(int(source_frame), subframe=source_frame % 1)
    for bone in ordered:
        bone.rotation_mode = 'QUATERNION'
        bone.location = (0, 0, 0)
        bone.scale = (1, 1, 1)
        if bone.name in mapping:
            source_name = mapping[bone.name]
            delta = source.pose.bones[source_name].matrix.to_quaternion() @ rest_source[source_name].inverted()
            desired = alignment @ delta @ alignment.inverted() @ rest_target[bone.name]
            parent_rest = rest_target[bone.parent.name] if bone.parent else Quaternion()
            parent_pose = bone.parent.matrix.to_quaternion() if bone.parent else Quaternion()
            local_rest = parent_rest.inverted() @ rest_target[bone.name]
            bone.rotation_quaternion = local_rest.inverted() @ parent_pose.inverted() @ desired
        else:
            # 原骨架独有的 Neck1、眼睛及手指保持自身参考姿势
            bone.rotation_quaternion = Quaternion()
        bpy.context.view_layer.update()
        if bone.name == 'Hips':
            src = mapping['Hips']
            displacement = source.matrix_world.to_3x3() @ (source.pose.bones[src].matrix.translation
                                                           - source.data.bones[src].head_local)
            source_height = (source.matrix_world @ source.data.bones[src].head_local).z
            target_height = (target.matrix_world @ bone.bone.head_local).z
            displacement.x = displacement.y = 0
            displacement *= target_height / max(source_height, .01)
            bone.location = rest_target['Hips'].inverted() @ (target.matrix_world.to_3x3().inverted() @ displacement)
            bone.keyframe_insert('location', frame=frame)
        bone.keyframe_insert('rotation_quaternion', frame=frame)
    bpy.context.view_layer.update()
    samples.append({'frame':frame, 'time':frame/30, **{name:list(target.matrix_world @ target.pose.bones[name].matrix.translation)
                    for name in ('Hips','Head','LeftHand','RightHand','LeftFoot','RightFoot')}})
scene = bpy.context.scene
scene.render.fps = 30
scene.frame_start = 0
scene.frame_end = last_frame
source.animation_data.action = None
# FBX 的无网格导出会把当前姿势写为默认参考姿势，导出前使用范围外的 T 姿势
# 动画采样范围仍从 0 开始，-1 帧只提供与原 SkeletalMesh 相同的绑定参考
for bone in ordered:
    bone.rotation_quaternion = Quaternion()
    bone.location = (0, 0, 0)
    bone.keyframe_insert('rotation_quaternion', frame=-1)
    if bone.name == 'Hips':
        bone.keyframe_insert('location', frame=-1)
scene.frame_set(-1)
bpy.ops.object.select_all(action='DESELECT')
target.select_set(True)
bpy.context.view_layer.objects.active = target
destination = OUT / 'A_NoodleBossEject.fbx'
bpy.ops.export_scene.fbx(filepath=str(destination), use_selection=True, object_types={'ARMATURE'},
                        add_leaf_bones=False, bake_anim=True, bake_anim_use_all_actions=False,
                        bake_anim_use_nla_strips=False, bake_anim_force_startend_keying=True,
                        bake_anim_simplify_factor=0.0, axis_forward='-Y', axis_up='Z',
                        apply_unit_scale=True, path_mode='STRIP', embed_textures=False)

# 独立重新导入导出文件，比较参考骨架世界位置和帧采样，不只确认文件存在
before = set(bpy.data.objects)
bpy.ops.import_scene.fbx(filepath=str(destination), anim_offset=0)
roundtrip_objects = list(set(bpy.data.objects) - before)
roundtrip = next(obj for obj in roundtrip_objects if obj.type == 'ARMATURE')
if set(roundtrip.data.bones.keys()) != set(rest_world):
    raise RuntimeError('FBX 回读骨名不匹配')
rest_error = max(((roundtrip.matrix_world @ roundtrip.data.bones[name].head_local) - Vector(position)).length
                 for name, position in rest_world.items())
if rest_error > .0001:
    (EVIDENCE/'roundtrip_failure.json').write_text(json.dumps({'rest_error':rest_error,
        'target_matrix':[list(row) for row in target.matrix_world],
        'roundtrip_matrix':[list(row) for row in roundtrip.matrix_world],
        'points':{name:{'before':rest_world[name],'after':list(roundtrip.matrix_world @ roundtrip.data.bones[name].head_local)}
                  for name in rest_world}},indent=2))
    raise RuntimeError('FBX 回读参考骨架位移超过 0.01 cm：' + str(rest_error))
roundtrip_samples = []
for frame in (0, last_frame // 2, last_frame):
    scene.frame_set(frame)
    roundtrip_samples.append({'frame':frame, 'max_joint_delta_m':max(
        ((roundtrip.matrix_world @ roundtrip.pose.bones[name].matrix.translation)
         - (target.matrix_world @ target.pose.bones[name].matrix.translation)).length
        for name in mapping)})
if max(row['max_joint_delta_m'] for row in roundtrip_samples) > .002:
    (EVIDENCE/'roundtrip_animation_failure.json').write_text(json.dumps(roundtrip_samples,indent=2))
    raise RuntimeError('FBX 动作回读位置偏差超过 0.2 cm')
for obj in roundtrip_objects:
    bpy.data.objects.remove(obj, do_unlink=True)

# 五个关键时刻都由真实烘焙动作生成，平铺成一张姿势检查图
preview_frames = sorted(set([0, last_frame//4, last_frame//2, last_frame*3//4, last_frame]))
preview_objects = []
for index, frame in enumerate(preview_frames):
    scene.frame_set(frame)
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    for obj in target_meshes:
        evaluated = obj.evaluated_get(depsgraph)
        data = bpy.data.meshes.new_from_object(evaluated, preserve_all_data_layers=True, depsgraph=depsgraph)
        copy = bpy.data.objects.new('Preview_%03d_%s' % (frame,obj.name), data)
        bpy.context.collection.objects.link(copy)
        copy.matrix_world = obj.matrix_world.copy()
        copy.location.x += (index - (len(preview_frames)-1)/2) * 2.1
        preview_objects.append(copy)
for obj in source_objects + target_objects:
    obj.hide_render = True
scene.render.engine = 'CYCLES'
scene.cycles.samples = 32
scene.render.resolution_x = 1800
scene.render.resolution_y = 560
scene.render.resolution_percentage = 100
scene.world = bpy.data.worlds.new('PreviewWorld')
scene.world.color = (.3,.3,.3)
bpy.ops.mesh.primitive_plane_add(size=200)
ground = bpy.context.object
ground_material = bpy.data.materials.new('PreviewGround')
ground_material.diffuse_color = (.12,.13,.15,1)
ground.data.materials.append(ground_material)
for position, energy, size in [((0,-5,7),2400,9),((-6,1,4),1400,6),((6,2,4),1800,6)]:
    bpy.ops.object.light_add(type='AREA', location=position)
    light = bpy.context.object
    light.data.energy = energy
    light.data.shape = 'DISK'
    light.data.size = size
    light.rotation_euler = (Vector((0,0,1))-light.location).to_track_quat('-Z','Y').to_euler()
bpy.ops.object.camera_add(location=(3,-12,4.4))
camera=bpy.context.object
camera.rotation_euler=(Vector((0,0,1))-camera.location).to_track_quat('-Z','Y').to_euler()
camera.data.type='ORTHO'
camera.data.ortho_scale=11.3
scene.camera=camera
scene.render.filepath=str(EVIDENCE/'boss_eject_keyframes.png')
bpy.ops.wm.save_as_mainfile(filepath=str(EVIDENCE/'boss_eject_review.blend'))
bpy.ops.render.render(write_still=True)
protected_after = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in (SOURCE,TARGET)}
if protected_before != protected_after:
    raise RuntimeError('原始模型或动画库文件发生改变')
report = {'status':'exported_and_blender_roundtrip_checked', 'author':'Quaternius',
          'license':'CC0 1.0 Universal', 'source_url':'https://quaternius.com/packs/universalanimationlibrary.html',
          'source_file':str(SOURCE), 'license_file':'Design/Art/NPC/Grandpa/Quaternius_License.txt',
          'source_clip':'Punch_Jab', 'target_source':str(TARGET),
          'target_ue_skeleton':'/Game/Characters/TestDeliveryMan/The_Boss_Skeleton',
          'output':str(destination), 'sample_rate':30, 'duration_seconds':last_frame/30,
          'bones':list(target.data.bones.keys()), 'root':'Hips', 'bone_count':len(target.data.bones),
          'roundtrip_rest_error_m':rest_error,'roundtrip_animation_samples':roundtrip_samples,
          'source_sha256':protected_before, 'protected_sources_unchanged':True,
          'output_sha256':hashlib.sha256(destination.read_bytes()).hexdigest(),
          'preview_frames':preview_frames,'samples_meters':samples,
          'notes':['仅输出老板挥赶一段动画，原网格和骨架未修改',
                   '源 FBX 对象保留厘米绑定数据与原对象变换，导出后同 Blender 世界尺寸回读',
                   '动画剥除 mixamorig: 与现有 UE 骨架命名一致；UE 导入需 animation-only 指定原 Skeleton',
                   '移除水平根位移；保留目标骨长；Neck1/眼睛/手指保持原参考姿势',
                   '这是 Blender 骨架和姿势检查，UE 动画导入和实机演出尚未验证']}
(OUT/'A_NoodleBossEject.source.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
(EVIDENCE/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
print('NOODLE_BOSS_RETARGET_REPORT=' + str(EVIDENCE/'report.json'))
