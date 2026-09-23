# -*- coding: utf-8 -*-
"""核对（并可选修正）每辆车相对它那条车道 Box 的位置。

为什么会错位：车是按**当时**的车道起点摆的，而 Box 挂在车道段的第一个
控制点上。重铺交通线之后所有 Lane_ 都是全新 actor、起点挪了位，车却还在
原地——可能已经跑到 Box 前面（一出生就开过了自己的盒子，前向探测扫不到，
TraceForNewPath 永远接不上路）、甚至落到别的车道上。

量两个数，都沿**车道方向**分解：
    纵向  车相对 Box 的位置。**负数 = 在 Box 后方 = 对的**，
          车往前开会扫到盒子。正数 = 已经开过了，接不上路。
    横向  车偏离车道中心多远。大了说明它离最近的这条车道其实不是一路的。

FIX=True 时把每辆车重新摆到"沿车道方向、Box 后方 BEHIND_BOX 处"，
朝向对齐车道。类、缩放、MaxSpeed、标签全部保留——只动位置和朝向。

用法：py reposition_cars.py
"""

import math
import traceback

import unreal

CAR_NAME_PREFIX = "BP_car_base"
LANE_TAG_PREFIX = "ClaudeGenLane"
BEHIND_BOX = 300.0       # 修正后车落在 Box 后方多远
SEARCH_RADIUS = 6000.0   # 找最近车道段起点的范围
LATERAL_WARN = 400.0     # 横向偏离超过这个值就点名
FIX = True               # False = 只报告不动

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "reposition_cars.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    # 车道段起点 = Box 的位置 = 样条第一个控制点
    lanes = []
    for a in actors:
        try:
            if not unreal.SystemLibrary.is_valid(a) or a.is_actor_being_destroyed():
                continue
        except Exception:
            pass
        if not any(str(t).startswith(LANE_TAG_PREFIX) for t in a.tags):
            continue
        lbl = a.get_actor_label()
        if not lbl.startswith("Lane_"):
            continue
        sp = a.get_component_by_class(unreal.SplineComponent)
        if sp is None or sp.get_number_of_spline_points() < 2:
            continue
        L = sp.get_spline_length()
        p = sp.get_location_at_distance_along_spline(0.0, WS)
        q = sp.get_location_at_distance_along_spline(min(100.0, L), WS)
        dx, dy = q.x - p.x, q.y - p.y
        h = (dx * dx + dy * dy) ** 0.5
        if h < 1e-3:
            continue
        lanes.append((lbl, p, dx / h, dy / h))

    cars = [a for a in actors
            if str(a.get_class().get_name()).startswith(CAR_NAME_PREFIX)]
    w("车道段 %d 条，车 %d 辆" % (len(lanes), len(cars)))
    w("纵向为负 = 车在 Box 后方（对的）；为正 = 已经开过了自己的盒子")
    w("")
    if not lanes or not cars:
        w("!! 缺一边，没法核对。")
        flush()
        return

    w("%-14s %-30s %9s %9s %9s  %s"
      % ("车", "最近的车道段", "离Box", "纵向", "横向", "判定"))
    w("-" * 104)

    ahead = far = ok = moved = 0
    for car in cars:
        loc = car.get_actor_location()
        best = None
        for lbl, p, ux, uy in lanes:
            d = ((p.x - loc.x) ** 2 + (p.y - loc.y) ** 2) ** 0.5
            if d > SEARCH_RADIUS:
                continue
            if best is None or d < best[0]:
                best = (d, lbl, p, ux, uy)
        if best is None:
            w("%-14s %-30s 附近 %.0f 内没有车道段"
              % (car.get_actor_label()[:14], "-", SEARCH_RADIUS))
            far += 1
            continue

        d, lbl, p, ux, uy = best
        vx, vy = loc.x - p.x, loc.y - p.y
        along = vx * ux + vy * uy          # 沿车道方向：负 = 在 Box 后方
        lat = abs(-vx * uy + vy * ux)      # 垂直方向

        if along > 0:
            verdict = "!! 在 Box 前方，接不上路"
            ahead += 1
        elif lat > LATERAL_WARN:
            verdict = "!! 横向偏 %.0f，多半不在这条车道上" % lat
            far += 1
        else:
            verdict = "正常"
            ok += 1

        note = ""
        if FIX:
            nx = p.x - ux * BEHIND_BOX
            ny = p.y - uy * BEHIND_BOX
            yaw = math.degrees(math.atan2(uy, ux))
            try:
                car.modify(True)
                car.set_actor_location_and_rotation(
                    unreal.Vector(nx, ny, p.z),
                    unreal.Rotator(0.0, 0.0, yaw),   # (roll, pitch, yaw)
                    False, True)
                moved += 1
                note = "-> 已摆到 Box 后方 %.0f" % BEHIND_BOX
            except Exception as exc:
                note = "!! 移动失败：%s" % str(exc)[:40]

        w("%-14s %-30s %9.0f %+9.0f %9.0f  %s %s"
          % (car.get_actor_label()[:14], lbl[:30], d, along, lat,
             verdict, note))

    w("")
    w("=" * 104)
    w("正常 %d，在 Box 前方 %d，横向偏太远/找不到车道 %d" % (ok, ahead, far))
    if FIX:
        w("已重新摆放 %d 辆（只改位置和朝向，类/缩放/MaxSpeed/标签都没动）" % moved)
    else:
        w("FIX=False，只报告没动。")
    w("")
    w("Box 挂在车道段的第一个控制点上，车必须在它**后方**：")
    w("车出生时 CurrentFollowSpline 是 None，靠 TraceForNewPath 往前扫，")
    w("扫到 Box 才拿得到样条。摆在 Box 前面等于一出生就错过了它。")
    w("关卡尚未保存。")
    flush()
    unreal.log("[repos] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[repos] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
