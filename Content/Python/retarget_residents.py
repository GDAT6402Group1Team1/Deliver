"""Blender 5: 将已有 Quaternius CC0 动画重定向到共用 Mixamo 骨架的居民

保留目标骨长和静止位移，不复用源人物的身高，不写入现有玩家骨架
运行 blender --background --python Content/Python/retarget_residents.py
追加 -- --locomotion-only 时只创建新的走路和慢跑 FBX，保留原有动画
追加 -- --light-hit-only 时只创建站立受击动画，保留倒地和起身素材
"""
import bpy
import math
import json
import sys
from pathlib import Path
from mathutils import Matrix, Quaternion

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'Design/Art/NPC/Grandpa'
OUT = ROOT / 'Design/Art/NPC/Residents'
OUT.mkdir(parents=True, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=str(SOURCE / 'Quaternius_AnimationLibrary_Standard.glb'))
source = bpy.data.objects['Rig']
actions = {a.name: (source, a) for a in bpy.data.actions}
before_rigs = set(bpy.data.objects)
bpy.ops.import_scene.gltf(filepath=str(SOURCE / 'Quaternius_AnimationLibrary2_Standard.glb'))
source2 = next(o for o in set(bpy.data.objects)-before_rigs if o.type == 'ARMATURE')
for track in source2.animation_data.nla_tracks: track.mute = True
source2.animation_data.action = None
actions.update({a.name: (source2, a) for a in bpy.data.actions if a.name not in actions})
for track in source.animation_data.nla_tracks:
    track.mute = True
source.animation_data.action = None
before = set(bpy.data.objects)
bpy.ops.import_scene.fbx(filepath=str(ROOT / 'Design/Art/NPC/中年男.fbx'))
added = set(bpy.data.objects) - before
target = next(o for o in added if o.type == 'ARMATURE')
mesh = next(o for o in added if o.type == 'MESH')
target.name = 'GrandpaRig'
mesh.name = 'Grandpa'
print('RETARGET_TRANSFORMS', source.matrix_world, target.matrix_world, mesh.matrix_world)

mapping = {'Hips':'DEF-hips','Spine':'DEF-spine.001','Spine1':'DEF-spine.002',
           'Spine2':'DEF-spine.003','Neck':'DEF-neck','Head':'DEF-head'}
for side, suffix in [('Left','L'),('Right','R')]:
    for dst, src in [('Shoulder','shoulder'),('Arm','upper_arm'),('ForeArm','forearm'),('Hand','hand'),
                     ('UpLeg','thigh'),('Leg','shin'),('Foot','foot'),('ToeBase','toe')]:
        mapping[side+dst] = 'DEF-'+src+'.'+suffix
mapping2 = {'Hips':'pelvis','Spine':'spine_01','Spine1':'spine_02','Spine2':'spine_03','Neck':'neck_01','Head':'Head'}
for side, suffix in [('Left','l'),('Right','r')]:
    for dst, src in [('Shoulder','clavicle'),('Arm','upperarm'),('ForeArm','lowerarm'),('Hand','hand'),('UpLeg','thigh'),('Leg','calf'),('Foot','foot'),('ToeBase','ball')]:
        mapping2[side+dst] = src+'_'+suffix
align = Quaternion((0,0,1), math.pi / 2)
rest_target = {b.name:b.matrix_local.to_quaternion() for b in target.data.bones}
rest_source = {b.name:b.matrix_local.to_quaternion() for b in source.data.bones}
ordered = sorted(target.pose.bones, key=lambda b:len(b.parent_recursive))

def select_target(include_mesh):
    bpy.ops.object.select_all(action='DESELECT')
    target.select_set(True)
    if include_mesh: mesh.select_set(True)
    bpy.context.view_layer.objects.active = target

def export(name, animated, include_mesh):
    select_target(include_mesh)
    bpy.ops.export_scene.fbx(filepath=str(OUT / (name+'.fbx')), use_selection=True,
        object_types={'ARMATURE','MESH'} if include_mesh else {'ARMATURE'},
        add_leaf_bones=False, bake_anim=animated, bake_anim_use_all_actions=False,
        bake_anim_use_nla_strips=False, bake_anim_force_startend_keying=True,
        bake_anim_simplify_factor=0.0, axis_forward='-Y', axis_up='Z',
        apply_unit_scale=True, path_mode='STRIP', embed_textures=False)

select_target(True)
# 保持现有居民网格与骨架不变，只输出独立动画
bpy.context.scene.render.fps = 30
locomotion_only = '--locomotion-only' in sys.argv
light_hit_only = '--light-hit-only' in sys.argv
if locomotion_only and light_hit_only:
    raise RuntimeError('请分别生成移动动画和站立受击动画')
clips = [('Hit_Chest','A_NpcLightHit')] if light_hit_only else [('Walk_Loop','A_NpcWalk'), ('Jog_Fwd_Loop','A_NpcJog')] if locomotion_only else [
    ('Idle_FoldArms_Loop','A_ResidentIdle'), ('Idle_Talking_Loop','A_ResidentTalk'),
    ('Walk_Formal_Loop','A_ResidentWalk'), ('Punch_Jab','A_ResidentPunch'), ('Death01','A_ResidentDown'),
    ('Push_Loop','A_ResidentHold'), ('Hit_Knockback','A_ResidentHit'), ('LayToIdle','A_ResidentGetUp')]
reports = []
for source_name, output in clips:
    # 增量制作保留已经生成的文件，避免重做时覆盖后来人工调整的动作
    if locomotion_only and (OUT / (output+'.fbx')).exists():
        reports.append({'source':source_name,'output':output,'status':'existing'})
        continue
    source, action = actions[source_name]
    current_mapping = mapping2 if source == source2 else mapping
    rest_source = {b.name:b.matrix_local.to_quaternion() for b in source.data.bones}
    source.animation_data.action = action
    if action.slots:
        source.animation_data.action_slot = action.slots[0]
    target.animation_data_create()
    baked = bpy.data.actions.new(output)
    target.animation_data.action = baked
    # GLB 导入动作按 24 fps 定义，输出统一采样为 30 fps
    duration = float(action.frame_range[1] - action.frame_range[0]) / 24.0
    count = round(duration*30)
    standing_samples = []
    for frame in range(count+1):
        source_frame = float(action.frame_range[0]) + frame / 30.0 * 24.0
        bpy.context.scene.frame_set(int(source_frame), subframe=source_frame % 1)
        for bone in ordered:
            bone.rotation_mode = 'QUATERNION'
            bone.location = (0,0,0)
            bone.scale = (1,1,1)
            if bone.name in current_mapping:
                name = current_mapping[bone.name]
                delta = source.pose.bones[name].matrix.to_quaternion() @ rest_source[name].inverted()
                desired = align @ delta @ align.inverted() @ rest_target[bone.name]
                parent_rest = rest_target[bone.parent.name] if bone.parent else Quaternion()
                parent_pose = bone.parent.matrix.to_quaternion() if bone.parent else Quaternion()
                local_rest = parent_rest.inverted() @ rest_target[bone.name]
                bone.rotation_quaternion = local_rest.inverted() @ parent_pose.inverted() @ desired
            else:
                bone.rotation_quaternion = Quaternion()
            bpy.context.view_layer.update()
            if bone.name == 'Hips':
                src_name = current_mapping['Hips']
                source_height = max(abs(source.data.bones[src_name].matrix_local.translation.z), .01)
                ratio = abs(bone.bone.matrix_local.translation.z) / source_height
                displacement = source.pose.bones[src_name].matrix.translation - source.data.bones[src_name].matrix_local.translation
                displacement.x = displacement.y = 0
                bone.location = rest_target['Hips'].inverted() @ (align @ displacement) * ratio
                bone.keyframe_insert('location', frame=frame)
            bone.keyframe_insert('rotation_quaternion', frame=frame)
        bpy.context.view_layer.update()
        if light_hit_only:
            # 记录重定向后的髋、头和双脚，确认普通受击没有包含倒地
            standing_samples.append({'time': frame/30.0,
                **{name: list(target.pose.bones[name].matrix.translation)
                   for name in ['Hips', 'Head', 'LeftFoot', 'RightFoot']}})
    bpy.context.scene.frame_start = 0
    bpy.context.scene.frame_end = count
    source.animation_data.action = None
    bpy.context.scene.frame_set(0)
    export(output, True, False)
    reports.append({'source':source_name,'output':output,'status':'created',
        'duration_seconds':duration,'sample_rate':30,'last_frame':count,'bone_count':len(ordered)})
    if light_hit_only:
        report = reports[-1]
        reference_hip = float(target.data.bones['Hips'].matrix_local.translation.z)
        report.update(reference_hip_height=reference_hip, samples=standing_samples,
            hip_min=min(row['Hips'][2] for row in standing_samples),
            hip_final=standing_samples[-1]['Hips'][2],
            head_above_hip_min=min(row['Head'][2]-row['Hips'][2] for row in standing_samples))
        if report['hip_min'] < reference_hip*.85 or report['head_above_hip_min'] <= 0:
            raise RuntimeError('站立受击姿势检查未通过，停止后续导入')
    print('RETARGET_EXPORTED', output, duration, count)

if light_hit_only:
    report_path = OUT / 'LightHitSource.json'
    report_path.write_text(json.dumps({'author':'Quaternius','license':'CC0 1.0 Universal',
        'source_url':'https://quaternius.com/packs/universalanimationlibrary.html',
        'source_file':'Design/Art/NPC/Grandpa/Quaternius_AnimationLibrary_Standard.glb',
        'license_file':'Design/Art/NPC/Grandpa/Quaternius_License.txt',
        'retarget_script':'Content/Python/retarget_residents.py --light-hit-only',
        'notes':['只替换普通轻拳表现，倒地和起身动画保持原样',
                 '目标骨架坐标为 Blender 米制，站姿数据检查不代替游戏画面检查'],
        'animations':reports}, ensure_ascii=False,indent=2),encoding='utf-8')
elif locomotion_only:
    (OUT / 'LocomotionSources.txt').write_text(
        'Author: Quaternius\nLicense: CC0 1.0 Universal\n'
        'Source: https://quaternius.com/packs/universalanimationlibrary.html\n'
        'Local original: ../Grandpa/Quaternius_AnimationLibrary_Standard.glb\n'
        'License copy: ../Grandpa/Quaternius_License.txt\n'
        'Walk_Loop -> A_NpcWalk.fbx; Jog_Fwd_Loop -> A_NpcJog.fbx\n'
        'Target: existing 中年男.fbx / SK_NpcMan_Skeleton; same mapping as retarget_residents.py\n'
        '30 fps; original bone lengths; horizontal root movement removed\n'
        'Original A_ResidentWalk (Walk_Formal_Loop) is unchanged\n', encoding='utf-8')
    report_path = ROOT / 'Saved/AIResearch/NpcLocomotionRetarget/latest.json'
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(reports,ensure_ascii=False,indent=2),encoding='utf-8')
else:
    (OUT / 'Sources.txt').write_text('Animations: Quaternius Universal Animation Library 1 and 2 Standard, CC0\nSource files and license: ../Grandpa/Quaternius*_License.txt\nhttps://quaternius.com/packs/universalanimationlibrary.html\nhttps://quaternius.com/packs/universalanimationlibrary2.html\nClips: Idle_FoldArms_Loop, Idle_Talking_Loop, Walk_Formal_Loop, Punch_Jab, Death01, Push_Loop, Hit_Knockback, LayToIdle\nRetarget target: existing Design/Art/NPC/中年男.fbx shared NPC skeleton\n', encoding='utf-8')
