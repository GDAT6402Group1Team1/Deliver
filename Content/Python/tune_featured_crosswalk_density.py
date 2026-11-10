# -*- coding: utf-8 -*-
"""只调整两条重点斑马线上四条现有路线的出发节奏"""
import unreal

LEVEL = '/Game/Level/TestForCharacter'
ROUTES = (
    ('PedestrianRoute_ChildCrosswalk4502_A', 2, 0.0),
    ('PedestrianRoute_ChildCrosswalk4502_B', 6, 0.45),
    ('PedestrianRoute_FeaturedCrosswalkR0039_A', 2, 0.0),
    ('PedestrianRoute_FeaturedCrosswalkR0039_B', 6, 0.45),
)
INTERVAL = 0.90

level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if not level.load_level(LEVEL):
    raise RuntimeError('Cannot load ' + LEVEL)
actors = {a.get_actor_label(): a for a in
          unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()}
for label, kinds, _ in ROUTES:
    route = actors.get(label)
    if not isinstance(route, unreal.DeliveryPedestrianRoute):
        raise RuntimeError('Missing route ' + label)
    if len(route.get_editor_property('kinds')) != kinds or \
            route.get_editor_property('max_alive') != 12:
        raise RuntimeError('Unexpected route settings: ' + label)

for label, _, delay in ROUTES:
    route = actors[label]
    route.set_editor_property('spawn_interval_min', INTERVAL)
    route.set_editor_property('spawn_interval_max', INTERVAL)
    route.set_editor_property('initial_spawn_delay', delay)
if not level.save_current_level():
    raise RuntimeError('Could not save level')
unreal.log('[FeaturedDensity] saved 4 routes: interval %.2f, B delay %.2f' %
           (INTERVAL, ROUTES[1][2]))
