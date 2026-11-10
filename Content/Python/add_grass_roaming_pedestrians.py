# -*- coding: utf-8 -*-
"""在 TestForCharacter 的草坪上增添随机游走 NPC，不替换现有活动区"""
import json
import math
import random
import unreal

LEVEL = '/Game/Level/TestForCharacter'
TAG = 'DeliveryGrassRoamingV1'
ROAD_MESH = '/Game/model/road_0.road_0'
RADIUS = 250.0
CLEARANCE = 100.0
COUNT = 7
SEED = 20261110
DRY_RUN = False
ANCHORS = ((-250.0, -1900.0), (1300.0, 5500.0),
           (4500.0, 2400.0), (-700.0, -6000.0))
KINDS = (('SK_NpcBoy', .60), ('SK_NpcGirl', .60),
         ('SK_NpcMan', 1.0), ('SK_NpcWoman', 1.0),
         ('SK_NpcGrandpa', .80), ('SK_NpcGrandma', .80))
REPORT = '/private/tmp/delivery_grass_roaming.json'


def distance_to_bounds(x, y, origin, extent):
    return math.hypot(max(abs(x-origin.x)-extent.x, 0.0),
                      max(abs(y-origin.y)-extent.y, 0.0))


def grass_height(world, x, y):
    hits = unreal.SystemLibrary.line_trace_multi(
        world, unreal.Vector(x, y, 7000), unreal.Vector(x, y, -1000),
        unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, False, [],
        unreal.DrawDebugTrace.NONE, True)
    if not hits:
        return None
    first = hits[0].to_dict()
    actor = first.get('hit_actor')
    component = first.get('hit_component')
    if not actor or actor.get_actor_label() != 'Landscape' or not component or \
            not component.get_name().startswith('LandscapeHeightfieldCollisionComponent'):
        return None
    return float(first['impact_point'].z)


def main():
    level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not level.load_level(LEVEL):
        raise RuntimeError('Cannot load ' + LEVEL)
    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    actors = editor.get_all_level_actors()
    route_class = getattr(unreal, 'DeliveryPedestrianRoute', None)
    if route_class is None:
        raise RuntimeError('Build DeliveryEditor first')

    road_bounds = []
    building_bounds = []
    other_routes = []
    for actor in actors:
        label = actor.get_actor_label().lower()
        if isinstance(actor, route_class) and TAG not in [str(t) for t in actor.tags]:
            p = actor.get_actor_location()
            if actor.get_editor_property('roam_in_area'):
                other_routes.append((p.x, p.y, actor.get_editor_property('roam_radius')))
        is_building = any(word in label for word in
                          ('house', 'shop', 'building', 'wall', 'fence', 'park'))
        for component in actor.get_components_by_class(unreal.StaticMeshComponent):
            mesh = component.get_editor_property('static_mesh')
            is_building |= bool(mesh and '/buildings/' in mesh.get_path_name().lower())
            if mesh and mesh.get_path_name() == ROAD_MESH:
                origin, extent, _ = unreal.SystemLibrary.get_component_bounds(component)
                road_bounds.append((origin, extent))
        if is_building:
            building_bounds.append(actor.get_actor_bounds(False))
    if len(road_bounds) < 100 or len(building_bounds) < 15:
        raise RuntimeError('Road or building geometry missing')

    rng = random.Random(SEED)
    chosen = []
    for index in range(COUNT):
        anchor = ANCHORS[index % len(ANCHORS)]
        rejected = {'road': 0, 'building': 0, 'route': 0, 'grass': 0}
        for _ in range(1500):
            angle = rng.uniform(0, 2*math.pi)
            distance = rng.uniform(500, 3500)
            x = round(anchor[0] + math.cos(angle)*distance, 1)
            y = round(anchor[1] + math.sin(angle)*distance, 1)
            if min(distance_to_bounds(x, y, o, e) for o, e in road_bounds) < RADIUS+CLEARANCE:
                rejected['road'] += 1
                continue
            if min(distance_to_bounds(x, y, o, e) for o, e in building_bounds) < RADIUS+CLEARANCE:
                rejected['building'] += 1
                continue
            if any(math.hypot(x-ox, y-oy) < RADIUS+orr+200
                   for ox, oy, orr in other_routes):
                rejected['route'] += 1
                continue
            if any(math.hypot(x-px, y-py) < 2*RADIUS+200
                   for px, py, _, _ in chosen):
                rejected['route'] += 1
                continue
            heights = [grass_height(world, x, y)]
            for step in range(8):
                a = step*math.pi/4
                heights.append(grass_height(world,
                    x+RADIUS*math.cos(a), y+RADIUS*math.sin(a)))
            if any(z is None for z in heights) or max(heights)-min(heights) > 150:
                rejected['grass'] += 1
                continue
            chosen.append((x, y, heights[0], rng.randint(1, 2)))
            break
        else:
            raise RuntimeError('Could not find safe grass area %d: %s' % (index+1, rejected))

    unreal.log('[GrassRoaming] planned %s' % chosen)
    if DRY_RUN:
        return
    kinds = []
    for name, height in KINDS:
        mesh = unreal.load_asset('/Game/Characters/NPC/%s.%s' % (name, name))
        if not isinstance(mesh, unreal.SkeletalMesh):
            raise RuntimeError('Missing NPC mesh ' + name)
        kind = unreal.DeliveryPedestrianKind()
        kind.set_editor_property('mesh', mesh)
        kind.set_editor_property('weight', 1.0)
        kind.set_editor_property('height_scale', height)
        kinds.append(kind)
    old = [a for a in actors if TAG in [str(t) for t in a.tags]]
    created = []
    try:
        for index, (x, y, z, count) in enumerate(chosen, 1):
            route = editor.spawn_actor_from_class(route_class, unreal.Vector(x, y, z+2))
            if route is None:
                raise RuntimeError('Cannot spawn grass route %d' % index)
            created.append(route)
            route.set_actor_label('PedestrianRoute_Grass_%02d' % index)
            route.set_folder_path('Pedestrians/GrassRoaming')
            route.tags = [unreal.Name(TAG)]
            route.set_editor_property('kinds', kinds)
            route.set_editor_property('roam_in_area', True)
            route.set_editor_property('roam_radius', RADIUS)
            route.set_editor_property('allow_handoff', False)
            route.set_editor_property('max_alive', count)
            route.set_editor_property('prewarm_on_begin_play', True)
            route.set_editor_property('roam_spawn_spacing', 100.0)
    except Exception:
        for route in created:
            editor.destroy_actor(route)
        raise
    for actor in old:
        editor.destroy_actor(actor)
    if not level.save_current_level():
        raise RuntimeError('Could not save level')
    with open(REPORT, 'w', encoding='utf-8') as handle:
        json.dump(chosen, handle, ensure_ascii=False, indent=2)
    unreal.log('[GrassRoaming] saved %d areas; max %d NPC' %
               (len(chosen), sum(row[3] for row in chosen)))


main()
