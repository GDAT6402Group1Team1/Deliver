# -*- coding: utf-8 -*-
"""将 R035 Merged 8 的普通过街路线替换成两条男孩/女孩排队路线

在已打开的 TestForCharacter 编辑器内运行：py place_child_crosswalk_r035_8.py
脚本不切图、不自动保存关卡；检查路线后由用户保存
"""

import math
import unreal

MARK = 'Crosswalk_BranchRoad_R035_Merged_8'
SIGNAL = 'Inter_BranchRoad_R035_Merged_+0180_I02'
LABEL = 'PedestrianRoute_ChildCrosswalkR0358'
TAG = 'DeliveryChildCrosswalkR0358'
GENERAL_TAG = 'DeliveryGeneralCrosswalkRouteV1'
LANE_OFFSET = 70.0
SECOND_LANE_DELAY = 0.45
WS = unreal.SplineCoordinateSpace.WORLD


def route_points(mark):
    origin = mark.get_actor_location()
    forward = mark.get_actor_forward_vector()
    lateral = (-forward.y, forward.x)
    length = math.hypot(*lateral)
    if length < .5:
        raise RuntimeError('Invalid crosswalk direction')
    lateral = (lateral[0] / length, lateral[1] / length)
    projections, surface = [], []
    for mesh in mark.get_components_by_class(unreal.StaticMeshComponent):
        if not mesh.static_mesh:
            continue
        bounds = mesh.static_mesh.get_bounding_box()
        transform = mesh.get_world_transform()
        surface.append(transform.transform_location(
            unreal.Vector(bounds.max.x, bounds.max.y, bounds.max.z)).z)
        for x in (bounds.min.x, bounds.max.x):
            for y in (bounds.min.y, bounds.max.y):
                for z in (bounds.min.z, bounds.max.z):
                    p = transform.transform_location(unreal.Vector(x, y, z))
                    projections.append(p.x * lateral[0] + p.y * lateral[1])
    if not projections:
        raise RuntimeError('Crosswalk has no mesh bounds')
    surface.sort()
    base = origin.x * lateral[0] + origin.y * lateral[1]
    z = surface[len(surface) // 2] + 2.0
    return [unreal.Vector(origin.x + lateral[0] * (edge - base),
                          origin.y + lateral[1] * (edge - base), z)
            for edge in (min(projections) - 250.0, max(projections) + 250.0)]


def main():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if world is None or world.get_name() != 'TestForCharacter':
        raise RuntimeError('Open TestForCharacter before running this script')
    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = editor.get_all_level_actors()
    by_label = {actor.get_actor_label(): actor for actor in actors}
    mark, signal = by_label.get(MARK), by_label.get(SIGNAL)
    if mark is None or signal is None:
        raise RuntimeError('Missing target crosswalk or signal segment')
    if int(signal.get_editor_property('LightNumber')) != 0:
        raise RuntimeError('Signal group changed; check pedestrian phase before replacing')
    route_class = getattr(unreal, 'DeliveryPedestrianRoute', None)
    if route_class is None:
        raise RuntimeError('Build DeliveryEditor first')
    kinds, fades, leaders, leader_fades = [], [], [], []
    for mesh_name, fade_name, is_leader in (
            ('SK_NpcBoy', 'M_ChildBoy_Fade', False),
            ('SK_NpcGirl', 'M_ChildGirl_Fade', False),
            ('SK_NpcMan', 'M_Man_Fade', True),
            ('SK_NpcWoman', 'M_Woman_Fade', True)):
        mesh = unreal.load_asset('/Game/Characters/NPC/' + mesh_name)
        fade = unreal.load_asset('/Game/Characters/NPC/' + fade_name)
        if not isinstance(mesh, unreal.SkeletalMesh) or not isinstance(fade, unreal.Material):
            raise RuntimeError('Missing child mesh or fade material: ' + mesh_name)
        kind = unreal.DeliveryPedestrianKind()
        kind.set_editor_property('mesh', mesh)
        kind.set_editor_property('weight', 1.0)
        kind.set_editor_property('height_scale', .60)
        (leaders if is_leader else kinds).append(kind)
        (leader_fades if is_leader else fades).append(fade)
    points = route_points(mark)
    # 只替换本斑马线的普通生成路线及本脚本旧路线，不触碰其他手摆 NPC
    old = [a for a in actors if isinstance(a, route_class) and (
        TAG in [str(t) for t in a.tags] or
        (GENERAL_TAG in [str(t) for t in a.tags] and
         a.get_editor_property('crosswalk_actor') == mark))]
    created = []
    try:
        forward = mark.get_actor_forward_vector()
        for lane, side, delay in (('A', -1, 0.0), ('B', 1, SECOND_LANE_DELAY)):
            shifted = [unreal.Vector(p.x + side * forward.x * LANE_OFFSET,
                                     p.y + side * forward.y * LANE_OFFSET, p.z)
                       for p in points]
            route = editor.spawn_actor_from_class(route_class, shifted[0])
            if route is None:
                raise RuntimeError('Cannot spawn child crosswalk route ' + lane)
            created.append((lane, route))
            route.set_folder_path('Pedestrians/Crosswalk')
            route.tags = [unreal.Name(TAG), unreal.Name(TAG + lane)]
            route.set_editor_property('kinds', kinds)
            route.set_editor_property('leader_kinds', leaders)
            route.set_editor_property('fade_materials', fades)
            route.set_editor_property('leader_fade_materials', leader_fades)
            route.set_editor_property('fade_instead_of_scale', True)
            route.set_editor_property('roam_in_area', False)
            route.set_editor_property('both_directions', False)
            route.set_editor_property('allow_handoff', False)
            route.set_editor_property('prewarm_on_begin_play', False)
            route.set_editor_property('pause_while_walking', False)
            route.set_editor_property('max_alive', 16)
            route.set_editor_property('spawn_interval_min', .70)
            route.set_editor_property('spawn_interval_max', .70)
            route.set_editor_property('initial_spawn_delay', delay)
            route.set_editor_property('yield_probe_distance_override', 35.)
            route.set_editor_property('walk_speed_min', 130.)
            route.set_editor_property('walk_speed_max', 130.)
            route.set_editor_property('lateral_jitter', 0.)
            route.set_editor_property('appear_distance', 100.)
            route.set_editor_property('red_light_walk_speed_multiplier', 2.0)
            route.set_editor_property('crosswalk_actor', mark)
            route.set_editor_property('signal_segment', signal)
            spline = route.get_component_by_class(unreal.SplineComponent)
            if spline is None:
                raise RuntimeError('New route has no spline')
            spline.clear_spline_points(False)
            for point in shifted:
                spline.add_spline_point(point, WS, False)
            for index in range(2):
                spline.set_spline_point_type(index, unreal.SplinePointType.LINEAR, False)
            spline.set_closed_loop(False, False)
            spline.update_spline()
            if abs(spline.get_spline_length() - math.dist(
                    (shifted[0].x, shifted[0].y),
                    (shifted[1].x, shifted[1].y))) > 1.0:
                raise RuntimeError('New route length is invalid: ' + lane)
    except Exception:
        for _lane, route in created:
            editor.destroy_actor(route)
        raise
    for actor in old:
        if not editor.destroy_actor(actor):
            raise RuntimeError('Failed to remove old route: ' + actor.get_actor_label())
    for lane, route in created:
        route.set_actor_label(LABEL + '_' + lane)
    unreal.log('[ChildCrosswalkR0358] Created two child lanes, replaced %d general/old routes; '
               'length %.0f cm, signal %s. Check and save level.' %
               (len(old), spline.get_spline_length(), SIGNAL))


if __name__ == '__main__':
    main()
