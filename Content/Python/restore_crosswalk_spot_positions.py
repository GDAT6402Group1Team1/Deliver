# -*- coding: utf-8 -*-
"""把已替换人行灯的三个 SpotLight 恢复为蓝图默认相对位置和朝向。"""
import unreal

LEVEL = '/Game/Level/TestForCharacter'
TAG = 'DeliveryCrosswalkReplaced'
DEFAULTS = {'green': 238.0, 'yellow': 203.0, 'red': 168.0}

levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if not levels.load_level(LEVEL):
    raise RuntimeError('Cannot load ' + LEVEL)
e = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
lights = [a for a in e.get_all_level_actors() if TAG in [str(t) for t in a.tags]]
if len(lights) != 48:
    raise RuntimeError('Expected 48 replaced pedestrian lights, found %d; nothing changed' % len(lights))
for actor in lights:
    spots = actor.get_components_by_class(unreal.SpotLightComponent)
    names = {s.get_name().lower(): s for s in spots}
    if set(names) != set(DEFAULTS):
        raise RuntimeError('%s has unexpected spots %s; nothing changed' %
                           (actor.get_actor_label(), sorted(names)))
for actor in lights:
    for name, spot in {s.get_name().lower(): s for s in
                       actor.get_components_by_class(unreal.SpotLightComponent)}.items():
        spot.set_editor_property('relative_location',
                                 unreal.Vector(0.0, 42.0, DEFAULTS[name]))
        spot.set_editor_property('relative_rotation',
                                 unreal.Rotator(0.0, 0.0, -90.0))
if not levels.save_current_level():
    raise RuntimeError('Could not save level')
unreal.log('[CrosswalkSpots] Restored original positions and rotations on %d lights' % len(lights))
