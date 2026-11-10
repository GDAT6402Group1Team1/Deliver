# -*- coding: utf-8 -*-
"""按现有灯罩的世界朝向重摆人行灯；新灯齐备后才删除旧灯。"""
import unreal

LEVEL = '/Game/Level/TestForCharacter'
PLACEMENT_TAG = 'ClaudeGenCrosswalkLight'
REPLACED_TAG = 'DeliveryCrosswalkReplaced'
EXPECTED_COUNT = 48

levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if not levels.load_level(LEVEL):
    raise RuntimeError('Cannot load ' + LEVEL)
e = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
old = [a for a in e.get_all_level_actors()
       if PLACEMENT_TAG in [str(t) for t in a.tags]]
if len(old) != EXPECTED_COUNT:
    raise RuntimeError('Expected %d pedestrian lights, found %d; nothing changed' %
                       (EXPECTED_COUNT, len(old)))
if all(REPLACED_TAG in [str(t) for t in a.tags] for a in old):
    unreal.log('[CrosswalkReplace] Already replaced; nothing changed')
else:
    if any(REPLACED_TAG in [str(t) for t in a.tags] for a in old):
        raise RuntimeError('Mixed old and replaced lights; nothing changed')
    prepared = []
    for a in old:
        meshes = a.get_components_by_class(unreal.StaticMeshComponent)
        spots = a.get_components_by_class(unreal.SpotLightComponent)
        if len(meshes) != 1 or len(spots) != 3:
            raise RuntimeError('%s has %d meshes and %d spots; nothing changed' %
                               (a.get_actor_label(), len(meshes), len(spots)))
        # 原模型自身相对中心高 130 cm；新实例保留原 Actor 位置，只取模型世界朝向
        prepared.append((a, a.get_class(), a.get_actor_location(),
                         meshes[0].get_world_rotation(), a.get_actor_label(),
                         [str(t) for t in a.tags if t not in
                          ('DeliveryCrosswalkMeshFacing180',
                           'DeliveryCrosswalkSpotsFacing180')]))
    spawned = []
    try:
        for old_actor, cls, location, model_rotation, label, tags in prepared:
            new = e.spawn_actor_from_class(cls, location, model_rotation)
            if not new:
                raise RuntimeError('Could not spawn replacement for ' + label)
            spawned.append(new)
            new_meshes = new.get_components_by_class(unreal.StaticMeshComponent)
            new_spots = new.get_components_by_class(unreal.SpotLightComponent)
            if len(new_meshes) != 1 or len(new_spots) != 3:
                raise RuntimeError('Unexpected components on replacement ' + label)
            # 灯罩保持蓝图默认 0°，Actor 朝向取旧灯罩的世界朝向。
            # SpotLight 保持蓝图原始位置和旋转：三盏只在 Z 高度上不同。
            new.set_actor_label(label + '_NEW')
            new.tags = [unreal.Name(t) for t in tags + [REPLACED_TAG]]
        for old_actor, *_ in prepared:
            e.destroy_actor(old_actor)
        for new, (_, _, _, _, label, _) in zip(spawned, prepared):
            new.set_actor_label(label)
        if not levels.save_current_level():
            raise RuntimeError('Replacement spawned but level save failed')
        unreal.log('[CrosswalkReplace] Replaced %d pedestrian lights; saved level' %
                   len(spawned))
    except Exception:
        if len(spawned) < len(prepared):
            for new in spawned:
                e.destroy_actor(new)
        raise
