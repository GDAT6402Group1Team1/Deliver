# -*- coding: utf-8 -*-
"""按用户确认的方向侧移人行灯；高度由 fix_crosswalk_light_heights.py 单独校正。"""
import math
import unreal

LEVEL = '/Game/Level/TestForCharacter'
TAG = 'ClaudeGenCrosswalkLight'
LEFT_OFFSET = 150.0  # 侧向偏移，厘米

levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if not levels.load_level(LEVEL):
    raise RuntimeError('Cannot load ' + LEVEL)
e = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
all_actors = e.get_all_level_actors()
marks = {a.get_actor_label(): a for a in all_actors
         if a.get_actor_label().startswith('Crosswalk_')}
lights = [a for a in all_actors if TAG in [str(t) for t in a.tags]]
if len(lights) != 48:
    raise RuntimeError('Expected 48 pedestrian lights, found %d; nothing changed' % len(lights))
prepared = []
for light in lights:
    label = light.get_actor_label()
    if not label.startswith('PedLight_') or not label.endswith(('_A', '_B')):
        raise RuntimeError('Unexpected pedestrian light name: ' + label)
    mark_label = 'Crosswalk_' + label[len('PedLight_'):-2]
    mark = marks.get(mark_label)
    if not mark:
        raise RuntimeError('Missing crosswalk for ' + label + ': ' + mark_label)
    loc = light.get_actor_location()
    cross = mark.get_actor_location()
    fv = mark.get_actor_forward_vector()
    n = math.hypot(fv.x, fv.y)
    if n < 0.5:
        raise RuntimeError('Invalid crosswalk direction: ' + mark_label)
    fx, fy = fv.x/n, fv.y/n
    # 按视口确认的方向：A 沿路轴正向，B 沿路轴反向。
    side = 1.0 if label.endswith('_A') else -1.0
    desired_along = cross.x*fx + cross.y*fy + side*LEFT_OFFSET
    current_along = loc.x*fx + loc.y*fy
    delta_along = desired_along - current_along
    new_loc = unreal.Vector(loc.x + fx*delta_along,
                            loc.y + fy*delta_along,
                            loc.z)
    prepared.append((light, new_loc))
for light, new_loc in prepared:
    light.set_actor_location(new_loc, False, False)
    unreal.log('[CrosswalkAlign] %s -> Z %.1f' %
               (light.get_actor_label(), new_loc.z))
if not levels.save_current_level():
    raise RuntimeError('Could not save level')
unreal.log('[CrosswalkAlign] aligned %d pedestrian lights' % len(prepared))
