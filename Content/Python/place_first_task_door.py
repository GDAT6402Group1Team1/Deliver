"""Place the first task's production axe and breakable front door at house2.

Run from the Unreal Editor after compiling DeliveryEditor:
    py "D:/Udev/Delivery/Content/Python/place_first_task_door.py"
"""
import unreal

MAP = '/Game/Level/TestForCharacter'
HOUSE_LABEL = 'house2'
DOOR_LABEL = 'OldManHouse_FrontDoor'
AXE_LABEL = 'OldManHouse_Axe'
DOOR_LOCATION = unreal.Vector(9610, 13455, 2334)
DOOR_ROTATION = unreal.Rotator(0, 90, 0)
DOOR_SCALE = unreal.Vector(0.3, 1.55, 1.75)
AXE_LOCATION = unreal.Vector(9800, 13000, 2005)
AXE_ROTATION = unreal.Rotator(0, 90, 0)
AXE_SCALE = unreal.Vector(0.2, 0.2, 0.2)

world = unreal.EditorLoadingAndSavingUtils.load_map(MAP)
if not world:
    raise RuntimeError('Cannot load first-task map: ' + MAP)
subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
actors = subsystem.get_all_level_actors()
houses = [actor for actor in actors if actor.get_actor_label() == HOUSE_LABEL]
if len(houses) != 1 or (houses[0].get_actor_location() - unreal.Vector(9501, 13979, 2762)).length() > 10:
    raise RuntimeError('house2 is missing or has moved; remeasure the door opening before placement')

door_class = unreal.load_class(None, '/Script/Delivery.DeliveryBreakableDoor')
axe_class = unreal.load_class(None, '/Game/Blueprint/Item/BP_Axe.BP_Axe_C')
if not door_class or not axe_class:
    raise RuntimeError('Breakable door or BP_Axe missing; compile and save assets first')

def ensure_actor(label, old_label, actor_class, location, rotation, scale):
    found = [actor for actor in actors if actor.get_actor_label() in (label, old_label)]
    if len(found) > 1:
        raise RuntimeError('Duplicate actors for ' + label)
    actor = found[0] if found else subsystem.spawn_actor_from_class(actor_class, location, rotation)
    if not actor or actor.get_class() != actor_class:
        raise RuntimeError('Missing or wrong actor class for ' + label)
    actor.modify(True)
    actor.set_actor_label(label)
    actor.set_actor_location(location, False, False)
    actor.set_actor_rotation(rotation, False)
    actor.set_actor_scale3d(scale)
    return actor

door = ensure_actor(DOOR_LABEL, 'Task001_BreakableDoor', door_class,
                    DOOR_LOCATION, DOOR_ROTATION, DOOR_SCALE)
axe = ensure_actor(AXE_LABEL, 'Task001_TestAxe', axe_class,
                   AXE_LOCATION, AXE_ROTATION, AXE_SCALE)

# Hide old native test labels if this script runs before the C++ class is rebuilt.
for component in door.get_components_by_class(unreal.TextRenderComponent):
    component.set_visibility(False)
door_mesh = door.get_component_by_class(unreal.StaticMeshComponent)
door_mesh.set_material(0, unreal.load_asset('/Game/materials/wood_dark'))

if not unreal.EditorLevelLibrary.save_current_level():
    raise RuntimeError('Failed to save first-task map')
unreal.log('TASK001_FORMAL_DOOR %s %s' % (door.get_actor_label(), door.get_actor_location()))
unreal.log('TASK001_AXE %s %s' % (axe.get_actor_label(), axe.get_actor_location()))
