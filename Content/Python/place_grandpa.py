"""把爷爷、可破坏门接到第一单；按 house2 的实际门洞摆放，不移动房屋"""
import unreal

world = unreal.EditorLoadingAndSavingUtils.load_map('/Game/Level/TestForCharacter')
subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
actors = subsystem.get_all_level_actors()
houses = [a for a in actors if a.get_actor_label() == 'house2']
if len(houses) != 1: raise RuntimeError('Expected one house2')
house = houses[0]
if (house.get_actor_location()-unreal.Vector(9687,13979,2762)).length()>5:
    raise RuntimeError('House moved: remeasure doorway before changing map')
def ensure(label, cls, position, rotation):
    found = [a for a in actors if a.get_actor_label() == label]
    if len(found)>1: raise RuntimeError('Duplicate '+label)
    actor = found[0] if found else subsystem.spawn_actor_from_class(cls, position, rotation)
    expected = cls.static_class() if isinstance(cls, type) else cls
    if actor.get_class() != expected: raise RuntimeError('Wrong class '+label)
    actor.modify()
    actor.set_actor_label(label)
    actor.set_actor_location(position, False, False)
    actor.set_actor_rotation(rotation, False)
    return actor

# 门洞实测：地板 2010.25，前墙 Y=13467.7，中心 X=9812，净宽约 280 cm
door = ensure('OldManHouse_FrontDoor', unreal.DeliveryBreakableDoor, unreal.Vector(9812,13468,2334), unreal.Rotator(pitch=0,yaw=90,roll=0))
door.set_actor_scale3d(unreal.Vector(.3,1.55,1.75))
door.get_component_by_class(unreal.StaticMeshComponent).set_material(0, unreal.load_asset('/Game/materials/wood_dark'))
cls = unreal.EditorAssetLibrary.load_blueprint_class('/Game/NPC/Grandpa/BP_Grandpa')
grandpa = ensure('NPC_OldMan_001', cls, unreal.Vector(9812,13750,2068.25), unreal.Rotator(pitch=0,yaw=-90,roll=0))
grandpa.set_editor_property('front_door', door)
task = unreal.load_asset('/Game/Task/Definitions/DA_Task_001')
grandpa.delivery_target.set_editor_property('expected_task', task)

# 原房屋只保留目的地导航，唯一交付点改为屋内爷爷
for location in house.get_components_by_class(unreal.DeliveryLocationComponent):
    if str(location.get_editor_property('location_id')) == 'Location_OldManHouse_001':
        location.modify()
        location.set_world_location(unreal.Vector(9812,13360,2010.25), False, False)
for target in house.get_components_by_class(unreal.DeliveryTargetComponent):
    target.modify()
    target.destroy_component(house)
for interaction in house.get_components_by_class(unreal.DeliveryInteractableComponent):
    interaction.modify()
    interaction.set_editor_property('interact_enabled', False)
for actor in [door, grandpa]:
    rotation = actor.get_actor_rotation()
    assert abs(rotation.pitch)<.1 and abs(rotation.roll)<.1, (actor.get_actor_label(),rotation)
if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
    raise RuntimeError('Could not save first-task map')
print('GRANDPA_PLACED',grandpa.get_actor_location(),door.get_actor_location())
print('GRANDPA_DOOR_BOUNDS',door.get_component_by_class(unreal.StaticMeshComponent).get_local_bounds())
