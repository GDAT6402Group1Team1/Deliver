# -*- coding: utf-8 -*-
"""在 testfortraffic 的南侧空地摆一条小孩单列直线，重复运行只替换本路线。"""

import math

import unreal


LEVEL = "/Game/Level/testfortraffic"
TAG = "DeliveryChildQueueV1"
LABEL = "PedestrianRoute_ChildQueue"
FOLDER = "Pedestrians/ChildQueue"
START = (500.0, -5000.0, 5.0)
END = (2900.0, -5000.0, 5.0)
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
    leaders = []
    for name in ("SK_NpcMan", "SK_NpcWoman"):
        mesh = unreal.load_asset("/Game/Characters/NPC/%s.%s" % (name, name))
        if not isinstance(mesh, unreal.SkeletalMesh):
            raise RuntimeError("Missing leader mesh: " + name)
        kind = unreal.DeliveryPedestrianKind()
        kind.set_editor_property("mesh", mesh)
        kind.set_editor_property("weight", 1.0)
        kind.set_editor_property("height_scale", 1.0)
        leaders.append(kind)
    route.set_editor_property("leader_kinds", leaders)
    route.set_editor_property("roam_in_area", False)
    route.set_editor_property("use_boids", True)
    route.set_editor_property("both_directions", False)
    route.set_editor_property("allow_handoff", False)
    route.set_editor_property("prewarm_on_begin_play", False)
    route.set_editor_property("pause_while_walking", False)
    # 人数要够铺满整条线，否则队伍走远之后入口会空一截。
    # 走完全程 2400 / 130 = 18.5 秒，间距约 195 cm，所以 2400 / 195 = 12 个才连续；
    # 给 6 个时队伍只有 975 cm（占四成），6 个排满用 9 秒、第一个 18.5 秒才退场，
    # 中间约 9 秒入口没人进场。
    route.set_editor_property("max_alive", 12)
    # 间距 = 生成间隔 x 速度，所以"排得紧"是在这里调、不是调速度。
    # **下限由让路探测决定**：IsBlockedAhead 从原点往前扫 YieldProbeDistance(95)
    # 加球半径 26，前面那个小孩的挨打胶囊半径 24 x 身高 0.60 约 14，
    # 于是间距小到约 135 cm 他们就互相判定"前面有人"而停下，再被
    # MaxYieldSeconds(2.5 秒) 超时硬挤过去——表现是整队走走停停、挤成一团。
    # 1.4~1.6 秒 x 125~135 cm/s = 175~216 cm，离那个下限还有 40 cm 以上余量。
    route.set_editor_property("spawn_interval_min", 1.4)
    route.set_editor_property("spawn_interval_max", 1.6)
    route.set_editor_property("walk_speed_min", 125.0)
    route.set_editor_property("walk_speed_max", 135.0)
    route.set_editor_property("lateral_jitter", 0.0)
    # 线短了，250 的出入场窗口会占掉两成多的行程（两头各一个），缩到 150
    route.set_editor_property("appear_distance", 150.0)
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

    # 期望长度从端点现算：写死一个数的话改端点忘了改它，断言就会假报失败
    expected = math.dist(START[:2], END[:2])
    if abs(spline.get_spline_length() - expected) > 1.0:
        raise RuntimeError("Child route is not the expected straight %.0f cm line" % expected)
    if not level.save_current_level():
        raise RuntimeError("Cannot save testfortraffic")
    # 报告里的数字一律从 actor 现读，不写字面量：写死的话改了属性日志照样打旧值，
    # 看着像"改了没生效"（这个脚本的 "max 6" 原来就是字面量）
    unreal.log("[ChildQueue] Saved %s: %d appearances, max_alive %d, length %.0f cm, "
               "interval %.2f~%.2f s, speed %.0f~%.0f cm/s, spacing %.0f~%.0f cm" %
               (LABEL, len(kinds),
                route.get_editor_property("max_alive"),
                spline.get_spline_length(),
                route.get_editor_property("spawn_interval_min"),
                route.get_editor_property("spawn_interval_max"),
                route.get_editor_property("walk_speed_min"),
                route.get_editor_property("walk_speed_max"),
                route.get_editor_property("spawn_interval_min") * route.get_editor_property("walk_speed_min"),
                route.get_editor_property("spawn_interval_max") * route.get_editor_property("walk_speed_max")))


if __name__ == "__main__":
    main()
