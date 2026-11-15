"""Bind the measured ground-pickup clip and the production axe grip in the editor."""
import json
from pathlib import Path

import unreal


ROOT = '/Game/NPC/Companion/Animation/'
SOURCE = ROOT + 'Candidates/GroundPickup/A_ErgouGroundPickup_GroundedCandidate'
OUTPUT = ROOT + 'A_ErgouGroundPickup'
TUNING = '/Game/NPC/Companion/DA_CompanionTuning'
AXE = '/Game/Blueprint/Item/BP_Axe'
AXE_GRIP = (-28.066, 45.451, 0.141)
PALM = (26.48, 48.88, 3.73)
CONTACT = 1.65
TOLERANCE = 16.0
SOURCE_TAG = 'GroundPickupValidatedSource'


def matches(vector, expected):
    return all(abs(actual - value) < 0.005 for actual, value in zip((vector.x, vector.y, vector.z), expected))


def axe_fields(axe):
    offset = axe.get_editor_property('npc_held_offset')
    rotation = axe.get_editor_property('npc_held_rotation')
    carry = axe.get_editor_property('npc_carry_animation')
    return {'npc_hand_socket': str(axe.get_editor_property('npc_hand_socket')),
            'npc_held_offset': (offset.x, offset.y, offset.z),
            'npc_held_rotation': (rotation.pitch, rotation.yaw, rotation.roll),
            'uses_axe_swing': bool(axe.get_editor_property('uses_axe_swing')),
            'carry_animation': carry.get_path_name() if carry else None}


def named_actor(label, cls):
    actor_tools = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    matching = [actor for actor in actor_tools.get_all_level_actors()
                if actor.get_class() == cls and actor.get_actor_label() == label]
    if len(matching) != 1:
        raise RuntimeError('在 TestForCharacter 找不到唯一的 ' + label)
    return matching[0]


def main():
    level = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    if not level or level.get_game_world():
        raise RuntimeError('只在普通编辑器中配置拾取动画')
    project = Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()))
    assets = unreal.EditorAssetLibrary
    dirty = unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()
    if {package.get_path_name() for package in dirty} & {OUTPUT, TUNING, AXE}:
        raise RuntimeError('保留已有的未保存拾取资产改动')
    source = assets.load_asset(SOURCE)
    tuning = assets.load_asset(TUNING)
    axe_bp = assets.load_asset(AXE)
    npc_class = assets.load_blueprint_class('/Game/NPC/Companion/BP_Companion_Ergou')
    npc = named_actor('NPC_Companion_Ergou', npc_class)
    mesh = npc.get_editor_property('mesh').get_editor_property('skeletal_mesh_asset')
    if (not isinstance(source, unreal.AnimSequence) or source.get_editor_property('skeleton') != mesh.get_editor_property('skeleton')
            or source.get_editor_property('enable_root_motion') or source.get_play_length() <= CONTACT or not tuning or not axe_bp):
        raise RuntimeError('当前候选动画、二狗骨架、斧头或调参资产不符')
    if npc.get_editor_property('companion').get_editor_property('tuning') != tuning:
        raise RuntimeError('正式地图二狗没有使用本次拾取调参')
    axe = unreal.get_default_object(axe_bp.generated_class())
    map_axe = named_actor('OldManHouse_Axe', axe.get_class())
    before = axe_fields(axe)
    if before['npc_hand_socket'] != 'RightHand' or not axe.get_editor_property('item_component').get_editor_property('companion_can_fetch'):
        raise RuntimeError('正式斧头的握点或可取配置已变化')
    existing_tuning = tuning.get_editor_property('ground_pickup_animation')
    if existing_tuning and existing_tuning.get_path_name().split('.')[0] != OUTPUT:
        raise RuntimeError('当前拾取动画由其他配置提供')
    if axe.get_editor_property('use_npc_ground_pickup_pose') and not matches(axe.get_editor_property('npc_ground_pickup_grip'), AXE_GRIP):
        raise RuntimeError('斧头已有不同的地面握点')
    original_grip = axe.get_editor_property('npc_ground_pickup_grip')
    if (map_axe.get_editor_property('use_npc_ground_pickup_pose') != axe.get_editor_property('use_npc_ground_pickup_pose') or
            not matches(map_axe.get_editor_property('npc_ground_pickup_grip'), (original_grip.x, original_grip.y, original_grip.z))):
        raise RuntimeError('地图斧头有独立的拾取握点配置')

    clip = assets.load_asset(OUTPUT)
    if clip:
        if assets.get_metadata_tag(clip, SOURCE_TAG) != source.get_path_name():
            raise RuntimeError('保留已存在的同名动画')
    else:
        clip = assets.duplicate_loaded_asset(source, OUTPUT)
        if not clip:
            raise RuntimeError('未能复制已校准的拾取动画')
        assets.set_metadata_tag(clip, SOURCE_TAG, source.get_path_name())
        if not assets.save_loaded_asset(clip):
            raise RuntimeError('拾取动画保存失败')
    if clip.get_editor_property('skeleton') != mesh.get_editor_property('skeleton') or clip.get_editor_property('enable_root_motion'):
        raise RuntimeError('正式拾取动画骨架或根运动不符')

    if not axe.get_editor_property('use_npc_ground_pickup_pose'):
        axe_bp.modify()
        axe.set_editor_property('npc_ground_pickup_grip', unreal.Vector(*AXE_GRIP))
        axe.set_editor_property('use_npc_ground_pickup_pose', True)
        if not unreal.BlueprintEditorLibrary.compile_blueprint(axe_bp):
            raise RuntimeError('斧头蓝图编译失败')
        axe = unreal.get_default_object(axe_bp.generated_class())
        if (axe_fields(axe) != before or not axe.get_editor_property('use_npc_ground_pickup_pose')
                or not matches(axe.get_editor_property('npc_ground_pickup_grip'), AXE_GRIP)):
            raise RuntimeError('斧头配置读回不符')
        if not assets.save_loaded_asset(axe_bp):
            raise RuntimeError('斧头蓝图保存失败')

    for name, value in (('ground_pickup_animation', clip), ('ground_pickup_contact_time', CONTACT),
                        ('ground_pickup_contact_tolerance', TOLERANCE), ('ground_pickup_palm_position', unreal.Vector(*PALM))):
        tuning.set_editor_property(name, value)
    if not assets.save_loaded_asset(tuning):
        raise RuntimeError('拾取调参保存失败')

    if (tuning.get_editor_property('ground_pickup_animation') != clip or
            abs(tuning.get_editor_property('ground_pickup_contact_time') - CONTACT) > 0.001 or
            abs(tuning.get_editor_property('ground_pickup_contact_tolerance') - TOLERANCE) > 0.001 or
            not matches(tuning.get_editor_property('ground_pickup_palm_position'), PALM)):
        raise RuntimeError('正式拾取调参读回不符')

    map_axe = named_actor('OldManHouse_Axe', axe.get_class())
    report = {'clip': clip.get_path_name(), 'authored_contact_seconds': CONTACT,
              'mesh_palm_cm': PALM, 'axe_grip_cm': AXE_GRIP, 'axe_class': axe.get_class().get_path_name(),
              'map_tuning': npc.get_editor_property('companion').get_editor_property('tuning').get_path_name(),
              'placed_axe': map_axe.get_path_name(),
              'placed_axe_uses_pose': bool(map_axe.get_editor_property('use_npc_ground_pickup_pose')
                                           and matches(map_axe.get_editor_property('npc_ground_pickup_grip'), AXE_GRIP))}
    evidence = project / 'Saved/AIResearch/CompanionGroundPickup-20261115/ground_pickup_production_bindings.json'
    evidence.write_text(json.dumps(report, indent=2, ensure_ascii=False)+'\n', encoding='utf-8')
    if not report['placed_axe_uses_pose']:
        raise RuntimeError('地图中的正式斧头尚未继承拾取握点')
    unreal.log('DELIVERY_GROUND_PICKUP_CONFIGURED ' + str(report))


if __name__ == '__main__':
    main()
