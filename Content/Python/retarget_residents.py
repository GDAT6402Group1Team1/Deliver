"""Blender 5: 将已有 Quaternius CC0 动画重定向到共用 Mixamo 骨架的居民

保留目标骨长和静止位移，不复用源人物的身高，不写入现有玩家骨架
运行 blender --background --python Content/Python/retarget_grandpa.py
"""
import bpy
import math
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
for source_name, output in [('Idle_FoldArms_Loop','A_ResidentIdle'), ('Idle_Talking_Loop','A_ResidentTalk'),
    ('Walk_Formal_Loop','A_ResidentWalk'), ('Punch_Jab','A_ResidentPunch'), ('Death01','A_ResidentDown'),
    ('Push_Loop','A_ResidentHold'), ('Hit_Knockback','A_ResidentHit'), ('LayToIdle','A_ResidentGetUp')]:
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
    bpy.context.scene.frame_start = 0
    bpy.context.scene.frame_end = count
    source.animation_data.action = None
    bpy.context.scene.frame_set(0)
    export(output, True, False)
    print('RETARGET_EXPORTED', output, duration, count)

(OUT / 'Sources.txt').write_text('Animations: Quaternius Universal Animation Library 1 and 2 Standard, CC0\nSource files and license: ../Grandpa/Quaternius*_License.txt\nhttps://quaternius.com/packs/universalanimationlibrary.html\nhttps://quaternius.com/packs/universalanimationlibrary2.html\nClips: Idle_FoldArms_Loop, Idle_Talking_Loop, Walk_Formal_Loop, Punch_Jab, Death01, Push_Loop, Hit_Knockback, LayToIdle\nRetarget target: existing Design/Art/NPC/中年男.fbx shared NPC skeleton\n', encoding='utf-8')
