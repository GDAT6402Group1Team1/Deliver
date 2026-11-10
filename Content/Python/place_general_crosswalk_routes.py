# -*- coding: utf-8 -*-
"""给其余有明确车灯归属的斑马线布置随机 NPC 过街路线"""
import math
import re
import unreal

LEVEL = '/Game/Level/TestForCharacter'
TAG = 'DeliveryGeneralCrosswalkRouteV1'
EXCLUDE = {'Crosswalk_Intersection_45_02', 'Crosswalk_MainRoad_R003_9'}
WS = unreal.SplineCoordinateSpace.WORLD
KINDS = (('SK_NpcBoy', .60), ('SK_NpcGirl', .60),
         ('SK_NpcMan', 1.0), ('SK_NpcWoman', 1.0),
         ('SK_NpcGrandpa', .80), ('SK_NpcGrandma', .80))


def span(mark, lat):
    values = []
    surface_heights = []
    for mesh in mark.get_components_by_class(unreal.StaticMeshComponent):
        if not mesh.static_mesh:
            continue
        box = mesh.static_mesh.get_bounding_box()
        xf = mesh.get_world_transform()
        top = xf.transform_location(unreal.Vector(box.max.x, box.max.y, box.max.z))
        surface_heights.append(top.z)
        for x in (box.min.x, box.max.x):
            for y in (box.min.y, box.max.y):
                for z in (box.min.z, box.max.z):
                    p = xf.transform_location(unreal.Vector(x, y, z))
                    values.append(p.x * lat[0] + p.y * lat[1])
    if not values:
        raise RuntimeError('No mesh bounds: ' + mark.get_actor_label())
    surface_heights.sort()
    return min(values), max(values), surface_heights[len(surface_heights)//2]


def signal_for(mark, segments):
    loc = mark.get_actor_location()
    matches = []
    for actor, p0, fx, fy in segments:
        rx, ry = loc.x - p0.x, loc.y - p0.y
        behind = -(rx * fx + ry * fy)
        lateral = abs(-rx * fy + ry * fx)
        if 150 <= behind <= 800 and lateral <= 1100:
            matches.append(actor)
    groups = {int(a.get_editor_property('LightNumber')) for a in matches}
    if not matches or len(groups) != 1:
        return None
    return matches[0]


def main():
    level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not level.load_level(LEVEL):
        raise RuntimeError('Cannot load ' + LEVEL)
    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = editor.get_all_level_actors()
    route_class = getattr(unreal, 'DeliveryPedestrianRoute', None)
    if route_class is None:
        raise RuntimeError('Build DeliveryEditor first')
    kinds = []
    for name, height in KINDS:
        mesh = unreal.load_asset('/Game/Characters/NPC/%s.%s' % (name, name))
        if not isinstance(mesh, unreal.SkeletalMesh):
            raise RuntimeError('Missing NPC mesh: ' + name)
        kind = unreal.DeliveryPedestrianKind()
        kind.set_editor_property('mesh', mesh)
        kind.set_editor_property('weight', 1.0)
        kind.set_editor_property('height_scale', height)
        kinds.append(kind)
    segments = []
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
        if length > .01:
            segments.append((actor, p0, dx/length, dy/length))
    marks = sorted((a for a in actors if a.get_actor_label().startswith('Crosswalk_')
                    and a.get_actor_label() not in EXCLUDE), key=lambda a: a.get_actor_label())
    plans = []
    skipped = []
    for mark in marks:
        signal = signal_for(mark, segments)
        if signal is None:
            skipped.append(mark.get_actor_label())
            continue
        loc = mark.get_actor_location()
        fv = mark.get_actor_forward_vector()
        lat = (-fv.y, fv.x)
        norm = math.hypot(*lat)
        if norm < .5:
            skipped.append(mark.get_actor_label())
            continue
        lat = (lat[0]/norm, lat[1]/norm)
        lo, hi, surface_z = span(mark, lat)
        base = loc.x*lat[0] + loc.y*lat[1]
        # 每三条中一条双道，其余单道，布置可重复
        lanes = ('A', 'B') if len(plans) % 3 == 0 else ('A',)
        plans.append((mark, signal, loc, fv, lat, lo-base-250, hi-base+250,
                      surface_z+2, lanes))
    if not plans:
        raise RuntimeError('No crosswalk with an unambiguous traffic signal')
    old = [a for a in actors if TAG in [str(t) for t in a.tags]]
    created = []
    try:
        for mark, signal, loc, fv, lat, start, end, route_z, lanes in plans:
            owner = None
            for lane in lanes:
                side = (-70 if lane == 'A' else 70) if len(lanes) == 2 else 0
                points = [unreal.Vector(loc.x+lat[0]*d+fv.x*side,
                                        loc.y+lat[1]*d+fv.y*side, route_z)
                          for d in (start, end)]
                route = editor.spawn_actor_from_class(route_class, points[0])
                if route is None:
                    raise RuntimeError('Cannot spawn route for ' + mark.get_actor_label())
                created.append(route)
                route.set_actor_label('PedestrianRoute_' + mark.get_actor_label() + '_' + lane)
                route.set_folder_path('Pedestrians/Crosswalk')
                route.tags = [unreal.Name(TAG)]
                route.set_editor_property('kinds', kinds)
                route.set_editor_property('roam_in_area', False)
                route.set_editor_property('both_directions', False)
                route.set_editor_property('allow_handoff', False)
                route.set_editor_property('prewarm_on_begin_play', False)
                route.set_editor_property('pause_while_walking', False)
                route.set_editor_property('max_alive', 6)
                route.set_editor_property('spawn_interval_min', .7)
                route.set_editor_property('spawn_interval_max', 1.1)
                route.set_editor_property('initial_spawn_delay', .4 if lane == 'B' else 0)
                route.set_editor_property('green_phase_spawn_min', 2)
                route.set_editor_property('green_phase_spawn_max', 3)
                if owner:
                    route.set_editor_property('shared_green_phase_budget_owner', owner)
                else:
                    owner = route
                route.set_editor_property('walk_speed_min', 130.)
                route.set_editor_property('walk_speed_max', 130.)
                route.set_editor_property('lateral_jitter', 15. if len(lanes) == 1 else 0.)
                route.set_editor_property('appear_distance', 100.)
                route.set_editor_property('red_light_walk_speed_multiplier', 1.5)
                route.set_editor_property('crosswalk_actor', mark)
                route.set_editor_property('signal_segment', signal)
                sp = route.get_component_by_class(unreal.SplineComponent)
                sp.clear_spline_points(False)
                for point in points:
                    sp.add_spline_point(point, WS, False)
                for index in range(2):
                    sp.set_spline_point_type(index, unreal.SplinePointType.LINEAR, False)
                sp.set_closed_loop(False, False)
                sp.update_spline()
            unreal.log('[GeneralCrosswalk] %s lanes=%d signal=%s' %
                       (mark.get_actor_label(), len(lanes), signal.get_actor_label()))
    except Exception:
        for route in created:
            editor.destroy_actor(route)
        raise
    for actor in old:
        editor.destroy_actor(actor)
    if not level.save_current_level():
        raise RuntimeError('Cannot save level')
    unreal.log('[GeneralCrosswalk] placed=%d routes=%d skipped=%s' %
               (len(plans), len(created), ','.join(skipped)))


main()
