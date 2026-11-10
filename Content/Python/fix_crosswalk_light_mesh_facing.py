# -*- coding: utf-8 -*-
"""修正已摆放人行灯的灯罩和聚光灯朝向；可重复运行。"""
import unreal

LEVEL = '/Game/Level/TestForCharacter'
PLACEMENT_TAG = 'ClaudeGenCrosswalkLight'
MESH_TAG = 'DeliveryCrosswalkMeshFacing180'
SPOT_TAG = 'DeliveryCrosswalkSpotsFacing180'

levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if not levels.load_level(LEVEL):
    raise RuntimeError('Cannot load ' + LEVEL)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
changed_meshes = changed_spots = 0
for actor in actors.get_all_level_actors():
    tags = [str(t) for t in actor.tags]
    if PLACEMENT_TAG not in tags:
        continue
    meshes = actor.get_components_by_class(unreal.StaticMeshComponent)
    spots = actor.get_components_by_class(unreal.SpotLightComponent)
    if len(meshes) != 1 or len(spots) != 3:
        raise RuntimeError('%s: expected 1 mesh and 3 spotlights, got %d and %d' %
                           (actor.get_actor_label(), len(meshes), len(spots)))
    if MESH_TAG not in tags:
        mesh = meshes[0]
        rot = mesh.get_editor_property('relative_rotation')
        mesh.set_editor_property('relative_rotation',
                                 unreal.Rotator(rot.roll, rot.pitch, rot.yaw + 180.0))
        tags.append(MESH_TAG)
        changed_meshes += 1
    if SPOT_TAG not in tags:
        # 蓝图默认灯泡在局部 +Y 42cm，朝 -Y；灯罩转半圈后应到 -Y，朝 +Y
        for spot in spots:
            loc = spot.get_editor_property('relative_location')
            rot = spot.get_editor_property('relative_rotation')
            spot.set_editor_property('relative_location',
                                     unreal.Vector(-loc.x, -loc.y, loc.z))
            spot.set_editor_property('relative_rotation',
                                     unreal.Rotator(rot.roll, rot.pitch, rot.yaw + 180.0))
        tags.append(SPOT_TAG)
        changed_spots += 1
    actor.tags = [unreal.Name(t) for t in tags]
    unreal.log('[CrosswalkFacing] %s mesh=%s spots=%s' %
               (actor.get_actor_label(), MESH_TAG in tags, SPOT_TAG in tags))
if (changed_meshes or changed_spots) and not levels.save_current_level():
    raise RuntimeError('Could not save level')
unreal.log('[CrosswalkFacing] changed %d meshes and %d spotlight sets' %
           (changed_meshes, changed_spots))
