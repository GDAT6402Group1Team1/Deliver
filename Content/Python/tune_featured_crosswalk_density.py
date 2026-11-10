# -*- coding: utf-8 -*-
"""加密三条重点斑马线的四条小孩列"""
import unreal

LEVEL = '/Game/Level/TestForCharacter'
ROUTES = (
    'PedestrianRoute_ChildCrosswalk4502_A',
    'PedestrianRoute_FeaturedCrosswalkR0039_A',
    'PedestrianRoute_ChildCrosswalkR0358_A',
    'PedestrianRoute_ChildCrosswalkR0358_B',
)
INTERVAL = 0.70
MAX_ALIVE = 16
YIELD_PROBE_DISTANCE = 35.0

level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if not level.load_level(LEVEL):
    raise RuntimeError('Cannot load ' + LEVEL)
actors = {a.get_actor_label(): a for a in
          unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()}
for label in ROUTES:
    route = actors.get(label)
    if not isinstance(route, unreal.DeliveryPedestrianRoute):
        raise RuntimeError('Missing route ' + label)
    if len(route.get_editor_property('kinds')) != 2 or \
            route.get_editor_property('max_alive') not in (12, MAX_ALIVE):
        raise RuntimeError('Unexpected route settings: ' + label)

for label in ROUTES:
    route = actors[label]
    route.set_editor_property('spawn_interval_min', INTERVAL)
    route.set_editor_property('spawn_interval_max', INTERVAL)
    route.set_editor_property('max_alive', MAX_ALIVE)
    route.set_editor_property('yield_probe_distance_override', YIELD_PROBE_DISTANCE)
if not level.save_current_level():
    raise RuntimeError('Could not save level')
unreal.log('[FeaturedDensity] saved %d child routes: interval %.2f, max %d, probe %.1f' %
           (len(ROUTES), INTERVAL, MAX_ALIVE, YIELD_PROBE_DISTANCE))
