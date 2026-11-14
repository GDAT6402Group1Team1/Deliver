"""只读检查正式地图的同伴路线，不启动 PIE、动作、模型或导航重建

检查当前 NavMesh 路径及距离，不把行人导航通过当作整车能通过或实际交接成功
输出到 Saved/AIResearch/CompanionRoutes，不保存地图或资产
"""
from pathlib import Path
import json
import math
import unreal


def xyz(value):
    return [value.x, value.y, value.z]


def main():
    if unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world():
        raise RuntimeError('只允许编辑器静态世界')
    world = unreal.EditorLoadingAndSavingUtils.load_map('/Game/Level/TestForCharacter')
    # Commandlet 刚载入时静态网格碰撞可能仍在异步编译，先完成编译再查询，不保存资产
    unreal.SystemLibrary.execute_console_command(world, 'Editor.AsyncAssetCompilationFinishAll')
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    npc = next(a for a in actors if isinstance(a, unreal.DeliveryCompanionCharacter))
    bike = npc.get_component_by_class(unreal.DeliveryCompanionComponent).get_editor_property('errand_vehicle')
    home = next(a for a in actors if isinstance(a, unreal.DeliveryHome))
    task = unreal.load_asset('/Game/Task/Definitions/DA_Task_001')
    pickups = [c for a in actors for c in a.get_components_by_class(unreal.DeliveryPickupPointComponent)
               if c.get_location_id() == task.get_editor_property('pickup_location_id')]
    assert len(pickups) == 1 and bike
    pickup = pickups[0]
    interaction = pickup.get_owner().get_component_by_class(unreal.DeliveryInteractableComponent)
    capsule = npc.get_component_by_class(unreal.CapsuleComponent)
    npc_feet = npc.get_actor_location() - unreal.Vector(0, 0, capsule.get_scaled_capsule_half_height())
    points = {'ergou': npc_feet, 'bike_boarding': bike.get_boarding_location(),
              'home_return': home.get_editor_property('return_anchor').get_world_location()}
    # 取件点在 BeginPlay 才自动创建交互组件，静态检查按同一地点锚点计算
    location = pickup.get_owner().get_component_by_class(unreal.DeliveryLocationComponent)
    goal = (interaction.get_interaction_point(npc.get_actor_location()) if interaction
            else location.get_world_location() if location else pickup.get_owner().get_actor_location())
    radius = pickup.get_editor_property('interact_radius')
    result = {'map': world.get_path_name(), 'pickup_goal': xyz(goal), 'pickup_radius': radius,
              'walk_speed': npc.get_editor_property('walk_speed'), 'vehicle_travel_speed': bike.get_editor_property('travel_speed'),
              'points': {}, 'routes': {}, 'limitation': '静态行人导航查询，不检查驾驶转弯、车宽、移动障碍或运行时交接'}

    def project(point, extent=unreal.Vector(100, 100, 300)):
        return unreal.NavigationSystemV1.project_point_to_navigation(world, point, None, None, extent)

    def line_clear(start, end, ignored):
        hit = unreal.SystemLibrary.line_trace_single_for_objects(
            world, start, end, [getattr(unreal.ObjectTypeQuery, 'OBJECT_TYPE_QUERY' + str(i))
                               for i in range(1, 6)], False, ignored, unreal.DrawDebugTrace.NONE, True)
        data = hit.to_dict() if hit else {}
        point = data.get('impact_point')
        blocker = data.get('hit_actor')
        component = data.get('hit_component')
        return {'clear': not data.get('blocking_hit', False) or (point - end).length() <= 2,
                'blocker': blocker.get_actor_label() if blocker else None,
                'component': component.get_path_name() if component else None,
                'blocking_hit': data.get('blocking_hit', False),
                'initial_overlap': data.get('initial_overlap', False),
                'impact': xyz(point) if point else None}

    half_height = capsule.get_scaled_capsule_half_height()
    result['pickup_line_of_sight_from_spawn'] = line_clear(npc.get_actor_location(), goal, [npc])

    pickup_points = []
    for distance in [.75 * radius, .45 * radius, min(125, .2 * radius)]:
        for angle in range(0, 360, 45):
            radians = math.radians(angle)
            proposed = goal + unreal.Vector(math.cos(radians) * distance, math.sin(radians) * distance, 0)
            projected = project(proposed)
            if (projected is not None and (projected - goal).length() <= radius
                    and line_clear(projected + unreal.Vector(0, 0, half_height), goal, [npc])['clear']):
                pickup_points.append(projected)
    if pickup_points:
        points['pickup_approach'] = min(pickup_points, key=lambda p: (p - npc_feet).length())
    result['pickup_navigation_candidates'] = len(pickup_points)
    grandpa = home.get_editor_property('former_resident')
    door = grandpa.get_editor_property('front_door')
    grandpa_capsule = grandpa.get_component_by_class(unreal.CapsuleComponent)
    grandpa_feet = grandpa.get_actor_location() - unreal.Vector(0, 0, grandpa_capsule.get_scaled_capsule_half_height())
    points['grandpa_inside'] = grandpa_feet
    house = home.get_editor_property('existing_house')
    house_origin, house_extent = house.get_actor_bounds(False)
    result['home_object_candidates'] = []
    for actor in actors:
        position = actor.get_actor_location()
        offset = position - house_origin
        if (abs(offset.x) > house_extent.x or abs(offset.y) > house_extent.y or
                abs(offset.z) > house_extent.z):
            continue
        meshes = [component.get_editor_property('static_mesh')
                  for component in actor.get_components_by_class(unreal.StaticMeshComponent)]
        paths = [mesh.get_path_name() for mesh in meshes if mesh]
        if any('/model/indoor/' in path for path in paths):
            result['home_object_candidates'].append({'actor': actor.get_path_name(),
                'label': actor.get_actor_label(), 'location': xyz(position), 'meshes': paths})
    result['home_object_limitation'] = '包围盒与模型目录只找编辑器候选，不证明在房间内或有互动能力；运行时必须显式接线和校验'
    result['house_floor_control'] = line_clear(grandpa_feet + unreal.Vector(0, 0, 75),
        grandpa_feet - unreal.Vector(0, 0, 100), [npc, grandpa])
    if (result['house_floor_control']['clear'] or
            result['house_floor_control']['blocker'] != house.get_actor_label()):
        raise RuntimeError('未命中现有房屋室内地板，静态网格碰撞未就绪或配置错误')
    # 只比较完整门和排除门的静态几何，不调用破门、搬离或修改组件碰撞
    doorway_start = points['home_return'] + unreal.Vector(0, 0, half_height)
    doorway_end = grandpa_feet + unreal.Vector(0, 0, half_height)
    result['doorway_line_closed'] = line_clear(doorway_start, doorway_end, [npc, grandpa])
    result['doorway_line_excluding_door'] = line_clear(doorway_start, doorway_end, [npc, grandpa, door])
    if (result['doorway_line_closed']['clear'] or
            result['doorway_line_closed']['blocker'] != door.get_actor_label()):
        raise RuntimeError('完整门未挡住门洞射线，不能把空查询当作无遮挡证据')
    result['physics_query_control'] = line_clear(points['home_return'] + unreal.Vector(0, 0, 150),
        points['home_return'] - unreal.Vector(0, 0, 150), [npc, grandpa])
    if result['physics_query_control']['clear']:
        raise RuntimeError('已知门外地面未命中，不能将空射线当作无遮挡证据')
    result['door_components'] = [{'name': c.get_name(), 'collision': str(c.get_collision_enabled()),
        'object_type': str(c.get_collision_object_type()), 'bounds': str(c.get_local_bounds())}
        for c in door.get_components_by_class(unreal.StaticMeshComponent)]
    result['nav_runtime_generation'] = [str(a.get_editor_property('runtime_generation'))
        for a in actors if isinstance(a, unreal.RecastNavMesh)]
    result['departure_floor_samples'] = []
    previous = grandpa_feet
    for point in home.get_editor_property('departure_route'):
        endpoint = unreal.MathLibrary.transform_location(home.get_actor_transform(), point)
        count = max(1, math.ceil((endpoint - previous).length() / 20))
        for index in range(1, count + 1):
            sample = previous + (endpoint - previous) * (index / count)
            hit = unreal.SystemLibrary.line_trace_single_for_objects(world, sample + unreal.Vector(0, 0, 75),
                sample - unreal.Vector(0, 0, 100), [unreal.ObjectTypeQuery.OBJECT_TYPE_QUERY1,
                unreal.ObjectTypeQuery.OBJECT_TYPE_QUERY2], False, [npc, grandpa], unreal.DrawDebugTrace.NONE, True)
            data = hit.to_dict() if hit else {}
            floor = data.get('impact_point')
            normal = data.get('impact_normal')
            hit_actor = data.get('hit_actor')
            component = data.get('hit_component')
            result['departure_floor_samples'].append({'route_point': xyz(sample), 'floor': xyz(floor) if floor else None,
                'normal_z': normal.z if normal else None, 'height_error_cm': sample.z - floor.z if floor else None,
                'blocking_hit': data.get('blocking_hit', False), 'initial_overlap': data.get('initial_overlap', False),
                'actor': hit_actor.get_actor_label() if hit_actor else None,
                'component': component.get_path_name() if component else None})
        previous = endpoint
    result['indoor_candidates'] = []
    for dx in (-250, 0, 250):
        for dy in (200, 400, 600):
            proposed = home.get_editor_property('return_anchor').get_world_location() + unreal.Vector(dx, dy, 0)
            projected = project(proposed, unreal.Vector(70, 70, 100))
            result['indoor_candidates'].append({'requested': xyz(proposed),
                'projected': xyz(projected) if projected else None})
    projected_points = {}
    for name, point in points.items():
        projected = project(point)
        projected_points[name] = projected
        result['points'][name] = {'requested': xyz(point), 'projected': xyz(projected) if projected is not None else None}
    for start_name, end_name in [('ergou', 'bike_boarding'), ('ergou', 'pickup_approach'),
                                ('bike_boarding', 'home_return'), ('pickup_approach', 'home_return'),
                                ('home_return', 'bike_boarding'), ('home_return', 'grandpa_inside')]:
        start, end = projected_points.get(start_name), projected_points.get(end_name)
        key = start_name + '_to_' + end_name
        if start is None or end is None:
            result['routes'][key] = {'complete': False, 'reason': 'endpoint_not_on_navigation'}
            continue
        path = unreal.NavigationSystemV1.find_path_to_location_synchronously(world, start, end, npc)
        valid = path is not None and path.is_valid()
        partial = path.is_partial() if path else True
        path_points = list(path.path_points) if valid else []
        length = sum((path_points[i] - path_points[i-1]).length() for i in range(1, len(path_points)))
        result['routes'][key] = {'complete': valid and not partial, 'partial': partial,
                                'length_cm': length, 'points': [xyz(p) for p in path_points]}
    out = Path(unreal.Paths.project_dir()) / 'Saved/AIResearch/CompanionRoutes'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'editor_navigation.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    summary = {key: {k: v for k, v in value.items() if k != 'points'} for key, value in result['routes'].items()}
    print('COMPANION_ROUTE_INSPECTION ' + json.dumps(summary, ensure_ascii=False))


if __name__ == '__main__':
    main()
