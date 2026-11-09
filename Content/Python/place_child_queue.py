# -*- coding: utf-8 -*-
"""在 testfortraffic 的南侧空地摆一条小孩单列直线，重复运行只替换本路线。"""

import unreal


LEVEL = "/Game/Level/testfortraffic"
TAG = "DeliveryChildQueueV1"
LABEL = "PedestrianRoute_ChildQueue"
FOLDER = "Pedestrians/ChildQueue"
START = (500.0, -5000.0, 5.0)
END = (4500.0, -5000.0, 5.0)
KINDS = (("SK_NpcBoy", 0.60), ("SK_NpcGirl", 0.60))


def main():
    level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not level.load_level(LEVEL):
        raise RuntimeError("Cannot load " + LEVEL)

    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()
    route_class = getattr(unreal, "DeliveryPedestrianRoute", None)
    if route_class is None:
        raise RuntimeError("DeliveryEditor is not built: DeliveryPedestrianRoute is missing")

    # 关卡加载缺 C++ 模块时会丢失已有路人；这种情况下绝不能保存地图
    existing_routes = [actor for actor in actors
                       if isinstance(actor, route_class) and TAG not in [str(t) for t in actor.tags]]
    if len(existing_routes) < 4:
        raise RuntimeError("Existing pedestrian routes are missing; refusing to save incomplete level")

    floor = next((actor for actor in actors if actor.get_actor_label() == "Floor"), None)
    if floor is None:
        raise RuntimeError("Floor is missing")
    origin, extent = floor.get_actor_bounds(False)
    for x, y, _ in (START, END):
        if not (origin.x - extent.x + 100 < x < origin.x + extent.x - 100 and
                origin.y - extent.y + 100 < y < origin.y + extent.y - 100):
            raise RuntimeError("Child route endpoint is outside Floor")

    kinds = []
    for name, scale in KINDS:
        mesh = unreal.load_asset("/Game/Characters/NPC/%s.%s" % (name, name))
        if not isinstance(mesh, unreal.SkeletalMesh):
            raise RuntimeError("Missing child mesh: " + name)
        kind = unreal.DeliveryPedestrianKind()
        kind.set_editor_property("mesh", mesh)
        kind.set_editor_property("weight", 1.0)
        kind.set_editor_property("height_scale", scale)
        kinds.append(kind)

    # 只处理本脚本的标签，保留用户手摆的其他路人
    for actor in actors:
        if TAG in [str(t) for t in actor.tags]:
            if not eas.destroy_actor(actor):
                raise RuntimeError("Cannot replace old child queue")

    route = eas.spawn_actor_from_class(route_class, unreal.Vector(*START))
    if route is None:
        raise RuntimeError("Cannot spawn child queue")
    route.set_actor_label(LABEL)
    route.set_folder_path(FOLDER)
    route.set_editor_property("tags", [TAG])
    route.set_editor_property("kinds", kinds)
    route.set_editor_property("roam_in_area", False)
    route.set_editor_property("both_directions", False)
    route.set_editor_property("allow_handoff", False)
    route.set_editor_property("prewarm_on_begin_play", False)
    route.set_editor_property("pause_while_walking", False)
    route.set_editor_property("max_alive", 6)
    route.set_editor_property("spawn_interval_min", 1.8)
    route.set_editor_property("spawn_interval_max", 2.4)
    route.set_editor_property("walk_speed_min", 125.0)
    route.set_editor_property("walk_speed_max", 135.0)
    route.set_editor_property("lateral_jitter", 0.0)
    route.set_editor_property("appear_distance", 250.0)
    route.set_editor_property("min_visible_scale", 0.02)

    spline = route.get_component_by_class(unreal.SplineComponent)
    if spline is None:
        raise RuntimeError("Child route has no spline")
    spline.clear_spline_points(False)
    for point in (START, END):
        spline.add_spline_point(unreal.Vector(*point), unreal.SplineCoordinateSpace.WORLD, False)
    for index in range(2):
        spline.set_spline_point_type(index, unreal.SplinePointType.LINEAR, False)
    spline.set_closed_loop(False, False)
    spline.update_spline()

    if abs(spline.get_spline_length() - 4000.0) > 1.0:
        raise RuntimeError("Child route is not the expected straight 4000 cm line")
    if not level.save_current_level():
        raise RuntimeError("Cannot save testfortraffic")
    unreal.log("[ChildQueue] Saved %s: 2 child appearances, max 6, length %.0f cm" %
               (LABEL, spline.get_spline_length()))


if __name__ == "__main__":
    main()
