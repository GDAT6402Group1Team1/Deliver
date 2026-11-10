# -*- coding: utf-8 -*-
"""两条重点斑马线各摆一条小孩路线和一条六种 NPC 混合路线"""
import math
import re
import unreal

LEVEL = '/Game/Level/TestForCharacter'
MARKS = (
    ('Crosswalk_Intersection_45_02', 'DeliveryChildCrosswalk4502',
     'PedestrianRoute_ChildCrosswalk4502'),
    ('Crosswalk_MainRoad_R003_9', 'DeliveryFeaturedCrosswalkR0039',
     'PedestrianRoute_FeaturedCrosswalkR0039'),
)
LANE_OFFSET = 70.0
SPAWN_INTERVAL = 0.90
SECOND_LANE_DELAY = 0.45
EXTEND = 250.0
WS = unreal.SplineCoordinateSpace.WORLD
NPCS = (
    ('SK_NpcBoy', .60, 'M_ChildBoy_Fade'),
    ('SK_NpcGirl', .60, 'M_ChildGirl_Fade'),
    ('SK_NpcMan', 1.0, 'M_Man_Fade'),
    ('SK_NpcWoman', 1.0, 'M_Woman_Fade'),
    ('SK_NpcGrandpa', .80, 'M_Grandpa_Fade'),
    ('SK_NpcGrandma', .80, 'M_Grandma_Fade'),
)


def signal_for(mark, actors):
    loc = mark.get_actor_location()
    matches = []
    for actor in actors:
        if not re.match(r'^Inter_(.+)_([+-]\d+)_I\d+$', actor.get_actor_label()):
            continue
        splines = actor.get_components_by_class(unreal.SplineComponent)
        sp = next((s for s in splines if s.get_name() == 'Spline'),
                  splines[0] if splines else None)
        if not sp or sp.get_spline_length() < 1:
            continue
        p0 = sp.get_location_at_distance_along_spline(0, WS)
        p1 = sp.get_location_at_distance_along_spline(min(200, sp.get_spline_length()), WS)
        dx, dy = p1.x-p0.x, p1.y-p0.y
        length = math.hypot(dx, dy)
        if length < .01:
            continue
        fx, fy = dx/length, dy/length
        rx, ry = loc.x-p0.x, loc.y-p0.y
        behind = -(rx*fx+ry*fy)
        lateral = abs(-rx*fy+ry*fx)
        if 150 <= behind <= 800 and lateral <= 1100:
            matches.append(actor)
    groups = {int(a.get_editor_property('LightNumber')) for a in matches}
    if not matches or len(groups) != 1:
        raise RuntimeError('Missing or ambiguous signal for ' + mark.get_actor_label())
    return matches[0]


def route_points(mark):
    loc = mark.get_actor_location()
    forward = mark.get_actor_forward_vector()
    lat = (-forward.y, forward.x)
    length = math.hypot(*lat)
    if length < .5:
        raise RuntimeError('Invalid crosswalk direction: ' + mark.get_actor_label())
    lat = (lat[0]/length, lat[1]/length)
    projection = []
    surface = []
    for mesh in mark.get_components_by_class(unreal.StaticMeshComponent):
        if not mesh.static_mesh:
            continue
        box = mesh.static_mesh.get_bounding_box()
        xf = mesh.get_world_transform()
        surface.append(xf.transform_location(
            unreal.Vector(box.max.x, box.max.y, box.max.z)).z)
        for x in (box.min.x, box.max.x):
            for y in (box.min.y, box.max.y):
                for z in (box.min.z, box.max.z):
                    p = xf.transform_location(unreal.Vector(x, y, z))
                    projection.append(p.x*lat[0] + p.y*lat[1])
    if not projection:
        raise RuntimeError('Crosswalk has no mesh bounds: ' + mark.get_actor_label())
    surface.sort()
    base = loc.x*lat[0] + loc.y*lat[1]
    points = []
    for edge in (min(projection)-EXTEND, max(projection)+EXTEND):
        d = edge-base
        points.append(unreal.Vector(loc.x+lat[0]*d, loc.y+lat[1]*d,
                                    surface[len(surface)//2]+2))
    return points, forward


def main():
    level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not level.load_level(LEVEL):
        raise RuntimeError('Cannot load ' + LEVEL)
    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = editor.get_all_level_actors()
    by_label = {a.get_actor_label(): a for a in actors}
    route_class = getattr(unreal, 'DeliveryPedestrianRoute', None)
    if route_class is None:
        raise RuntimeError('Build DeliveryEditor first')
    kinds = []
    fades = []
    for name, height, fade_name in NPCS:
        mesh = unreal.load_asset('/Game/Characters/NPC/%s.%s' % (name, name))
        fade = unreal.load_asset('/Game/Characters/NPC/' + fade_name)
        if not isinstance(mesh, unreal.SkeletalMesh) or not isinstance(fade, unreal.Material):
            raise RuntimeError('Missing NPC mesh or fade material: ' + name)
        kind = unreal.DeliveryPedestrianKind()
        kind.set_editor_property('mesh', mesh)
        kind.set_editor_property('weight', 1.0)
        kind.set_editor_property('height_scale', height)
        kinds.append(kind)
        fades.append(fade)
    plans = []
    for label, tag, prefix in MARKS:
        mark = by_label.get(label)
        if mark is None:
            raise RuntimeError('Missing crosswalk ' + label)
        points, forward = route_points(mark)
        plans.append((mark, signal_for(mark, actors), points, forward, tag, prefix))
    own_tags = {item[1] for item in MARKS}
    old = [a for a in actors if any(str(t) in own_tags for t in a.tags)]
    created = []
    try:
        for mark, signal, points, forward, tag, prefix in plans:
            for lane, side, delay, choices, materials in (
                    ('A', -1, 0., kinds[:2], fades[:2]),
                    ('B', 1, SECOND_LANE_DELAY, kinds, fades)):
                lane_points = [unreal.Vector(p.x+side*forward.x*LANE_OFFSET,
                                             p.y+side*forward.y*LANE_OFFSET, p.z)
                               for p in points]
                route = editor.spawn_actor_from_class(route_class, lane_points[0])
                if route is None:
                    raise RuntimeError('Cannot spawn ' + prefix + lane)
                created.append(route)
                route.set_actor_label(prefix + '_' + lane)
                route.set_folder_path('Pedestrians/Crosswalk')
                route.tags = [unreal.Name(tag), unreal.Name(tag+lane)]
                route.set_editor_property('kinds', choices)
                route.set_editor_property('fade_materials', materials)
                route.set_editor_property('fade_instead_of_scale', True)
                route.set_editor_property('roam_in_area', False)
                route.set_editor_property('both_directions', False)
                route.set_editor_property('allow_handoff', False)
                route.set_editor_property('prewarm_on_begin_play', False)
                route.set_editor_property('pause_while_walking', False)
                route.set_editor_property('max_alive', 12)
                route.set_editor_property('spawn_interval_min', SPAWN_INTERVAL)
                route.set_editor_property('spawn_interval_max', SPAWN_INTERVAL)
                route.set_editor_property('initial_spawn_delay', delay)
                route.set_editor_property('walk_speed_min', 130.)
                route.set_editor_property('walk_speed_max', 130.)
                route.set_editor_property('lateral_jitter', 0.)
                route.set_editor_property('appear_distance', 100.)
                route.set_editor_property('red_light_walk_speed_multiplier', 1.5)
                route.set_editor_property('crosswalk_actor', mark)
                route.set_editor_property('signal_segment', signal)
                sp = route.get_component_by_class(unreal.SplineComponent)
                sp.clear_spline_points(False)
                for point in lane_points:
                    sp.add_spline_point(point, WS, False)
                for index in range(2):
                    sp.set_spline_point_type(index, unreal.SplinePointType.LINEAR, False)
                sp.set_closed_loop(False, False)
                sp.update_spline()
            unreal.log('[FeaturedCrosswalk] %s: child lane + six-kind lane; signal=%s' %
                       (mark.get_actor_label(), signal.get_actor_label()))
    except Exception:
        for route in created:
            editor.destroy_actor(route)
        raise
    for actor in old:
        editor.destroy_actor(actor)
    if not level.save_current_level():
        raise RuntimeError('Could not save level')
    unreal.log('[FeaturedCrosswalk] replaced %d routes with %d routes' %
               (len(old), len(created)))


main()
