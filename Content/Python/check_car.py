# -*- coding: utf-8 -*-
"""检查车辆是否正确跟随生成的车道样条。

在 Simulate 运行中执行才有意义（静止时 CurrentFollowSpline 是空的）：
    py check_car.py
结果写入 Saved/check_car.txt，同时打印到日志。
"""

import traceback

import unreal

CAR_HINT = "car"
LANE_TAG = "ClaudeGenLane"
OUT = unreal.Paths.project_saved_dir() + "check_car.txt"
WS = unreal.SplineCoordinateSpace.WORLD

lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[check_car] %s" % s)


def get_world():
    """Simulate 时车在 PIE 世界里，编辑器世界拿不到运行状态。"""
    ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    try:
        gw = ues.get_game_world()
        if gw:
            return gw, "GameWorld(运行中)"
    except Exception:
        pass
    return ues.get_editor_world(), "EditorWorld(未运行)"


def prop(obj, name):
    try:
        return obj.get_editor_property(name)
    except Exception:
        return "<读不到>"


def run():
    world, wname = get_world()
    w("世界: %s" % wname)

    allac = unreal.GameplayStatics.get_all_actors_of_class(world, unreal.Actor)
    w("该世界 actor 总数 %d" % len(allac))

    # ---- 车道 ----
    lanes = []
    for a in allac:
        try:
            if LANE_TAG in [str(t) for t in a.tags]:
                sp = a.get_component_by_class(unreal.SplineComponent)
                if sp:
                    lanes.append((a, sp))
        except Exception:
            pass
    w("找到生成的车道 %d 条: %s" % (len(lanes), [a.get_actor_label() for a, _ in lanes]))

    # ---- 车 ----
    cars = [a for a in allac
            if CAR_HINT in a.get_actor_label().lower() or CAR_HINT in a.get_class().get_name().lower()]
    w("找到车辆 %d 台" % len(cars))

    for car in cars:
        loc = car.get_actor_location()
        w("")
        w("=" * 64)
        w("车: %s   类 %s" % (car.get_actor_label(), car.get_class().get_name()))
        w("  位置 (%.0f, %.0f, %.0f)  朝向 yaw %.1f" % (loc.x, loc.y, loc.z, car.get_actor_rotation().yaw))
        for v in ("CurrentFollowSpline", "CurrentSpeed", "MaxSpeed", "StopatInter",
                  "StoppedAtIntersection", "SpawnLocation"):
            w("  %-22s = %s" % (v, prop(car, v)))

        # 组件状态
        for c in car.get_components_by_class(unreal.ActorComponent):
            if "TrafficCar" in type(c).__name__:
                for v in ("debug_speed_multiplier", "debug_distance_ahead",
                          "debug_blocked_elapsed", "trace_channel"):
                    w("  [组件] %-20s = %s" % (v, prop(c, v)))

        # 到每条车道的距离 —— 判断它实际贴着哪条
        w("  —— 到各车道的最近距离 ——")
        best = None
        for a, sp in lanes:
            closest = sp.find_location_closest_to_world_location(loc, WS)
            d = (closest - loc).length()
            w("    %-26s %8.1f cm" % (a.get_actor_label(), d))
            if best is None or d < best[1]:
                best = (a.get_actor_label(), d)
        if best:
            w("  >>> 最近的是 %s（%.1f cm）" % best)

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    w("已写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[check_car] 失败:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("失败:\n" + err)
