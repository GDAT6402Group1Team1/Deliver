# -*- coding: utf-8 -*-
"""在每个路口生成左右转通道。

规则（用户指定）：
    外道(|偏移|=375) -> 左转，接目标路左转方向的 2 条车道
    内道(|偏移|=125) -> 右转，接目标路右转方向的 2 条车道
    起点 = 来车道路口段的第一个点；终点 = 目标路口段的最后一个点

左右不能用偏移符号判断——每条道路样条的走向是作者随手画的，
形状39 的"正方向"和形状13 没有固定关系。所以用叉乘做几何判断：
UE 里 Cross(Forward, Right) = +Z，故 cross(进入方向, 驶出方向).z > 0 为右转。

曲线用三次贝塞尔，两端控制点沿各自方向伸出，保证与来路/去路相切平滑。
高度和车道一样逐点打射线贴真实路面。

依赖：先跑 gen_traffic_lanes.py。
"""

import traceback

import unreal

CHILD_BP = "/Game/PS2DEM/BP_TrafficLine1_IntersectionChild"
LANE_TAG_PREFIX = "ClaudeGenLane"
TAG = "ClaudeGenTurn"

CLUSTER_DIST = 2500.0    # 世界坐标相距小于此值的路口段归为同一个物理路口
OUTER_ABS = 375          # 外道偏移绝对值 -> 左转
INNER_ABS = 125          # 内道偏移绝对值 -> 右转
BEZIER_K = 0.45          # 控制点伸出长度占首尾直线距离的比例
SAMPLES = 14             # 每条转弯线采样点数
Z_OFFSET = 15.0
MIN_TURN_LEN = 200.0     # 首尾太近的不生成（同一条线自己接自己）

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "gen_turns.txt"

lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[gen_turns] %s" % s)


def surface_z(world, x, y, z_hint):
    start = unreal.Vector(x, y, z_hint + 60000.0)
    end = unreal.Vector(x, y, z_hint - 60000.0)
    try:
        hit = unreal.SystemLibrary.line_trace_single(
            world, start, end, unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
            False, [], unreal.DrawDebugTrace.NONE, True)
        if hit is None:
            return None
        d = hit.to_dict()
        if not d.get("blocking_hit"):
            return None
        return d["impact_point"].z
    except Exception:
        return None


def mark_edited(sp):
    """给样条打"已被编辑"标记，否则构造脚本一重跑就把点冲回蓝图默认的 2 个点。

    bSplineHasBeenEdited 在编辑器里显示为 "Override Construction Script"。
    手动拖点时 UE 自动打，从脚本写点必须自己设。
    """
    for name in ("spline_has_been_edited", "b_spline_has_been_edited"):
        try:
            sp.set_editor_property(name, True)
            return True
        except Exception:
            continue
    return False


def parse_label(lbl):
    """Inter_形状13_-0375_I05 -> ("形状13", -375, "I05")"""
    parts = lbl.split("_")
    if len(parts) < 4 or parts[0] != "Inter":
        return None
    try:
        return parts[1], int(parts[2]), parts[3]
    except ValueError:
        return None


def norm2d(v):
    h = (v.x * v.x + v.y * v.y) ** 0.5
    if h < 1e-6:
        return unreal.Vector(1, 0, 0)
    return unreal.Vector(v.x / h, v.y / h, 0.0)


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    actors = eas.get_all_level_actors()

    removed = 0
    for a in actors:
        if TAG in [str(t) for t in a.tags]:
            eas.destroy_actor(a)
            removed += 1
    w("清除旧转弯道 %d 个" % removed)

    # ---- 收集路口段，取首尾点和方向 ----
    segs = []
    for a in actors:
        if not any(str(t).startswith(LANE_TAG_PREFIX) for t in a.tags):
            continue
        info = parse_label(a.get_actor_label())
        if info is None:
            continue
        sp = a.get_component_by_class(unreal.SplineComponent)
        if sp is None or sp.get_number_of_spline_points() < 2:
            continue
        L = sp.get_spline_length()
        segs.append({
            "road": info[0], "offset": info[1], "idx": info[2],
            "label": a.get_actor_label(),
            "p_in": sp.get_location_at_distance_along_spline(0.0, WS),
            "d_in": norm2d(sp.get_direction_at_distance_along_spline(0.0, WS)),
            "p_out": sp.get_location_at_distance_along_spline(L, WS),
            "d_out": norm2d(sp.get_direction_at_distance_along_spline(L, WS)),
        })
    w("路口段 %d 个" % len(segs))
    if not segs:
        w("!! 没找到 Inter_* actor，请先跑 gen_traffic_lanes.py")
        return

    # ---- 按世界坐标聚成物理路口 ----
    clusters = []
    for s in segs:
        c = unreal.Vector((s["p_in"].x + s["p_out"].x) / 2.0,
                          (s["p_in"].y + s["p_out"].y) / 2.0,
                          (s["p_in"].z + s["p_out"].z) / 2.0)
        got = None
        for cl in clusters:
            if (cl["center"] - c).length() < CLUSTER_DIST:
                got = cl
                break
        if got is None:
            clusters.append({"center": c, "segs": [s]})
        else:
            got["segs"].append(s)
    w("聚类出 %d 个物理路口" % len(clusters))
    w("")

    child_cls = unreal.EditorAssetLibrary.load_asset(CHILD_BP).generated_class()
    made = skipped = 0

    for ci, cl in enumerate(clusters):
        roads = {}
        for s in cl["segs"]:
            roads.setdefault(s["road"], []).append(s)
        if len(roads) < 2:
            w("路口 %02d 只有 %d 条路（%s），跳过"
              % (ci, len(roads), ",".join(roads.keys())))
            continue
        w("路口 %02d  中心(%.0f, %.0f)  道路: %s"
          % (ci, cl["center"].x, cl["center"].y, " x ".join(sorted(roads.keys()))))

        names = sorted(roads.keys())
        for a_name in names:
            for b_name in names:
                if a_name == b_name:
                    continue
                for src in roads[a_name]:
                    ao = abs(src["offset"])
                    if ao == OUTER_ABS:
                        want_right = False       # 外道左转
                        kind = "L"
                    elif ao == INNER_ABS:
                        want_right = True        # 内道右转
                        kind = "R"
                    else:
                        continue

                    picked = []
                    for dst in roads[b_name]:
                        # 叉乘判左右：cross(进入方向, 驶出方向).z > 0 为右转
                        cz = src["d_in"].x * dst["d_out"].y - src["d_in"].y * dst["d_out"].x
                        if (cz > 0) == want_right:
                            picked.append(dst)

                    for dst in picked:
                        p0, p3 = src["p_in"], dst["p_out"]
                        span = ((p3.x - p0.x) ** 2 + (p3.y - p0.y) ** 2) ** 0.5
                        if span < MIN_TURN_LEN:
                            skipped += 1
                            continue
                        k = span * BEZIER_K
                        p1 = unreal.Vector(p0.x + src["d_in"].x * k,
                                           p0.y + src["d_in"].y * k, p0.z)
                        p2 = unreal.Vector(p3.x - dst["d_out"].x * k,
                                           p3.y - dst["d_out"].y * k, p3.z)

                        pts = []
                        for i in range(SAMPLES + 1):
                            t = float(i) / SAMPLES
                            u = 1.0 - t
                            bx = (u * u * u * p0.x + 3 * u * u * t * p1.x
                                  + 3 * u * t * t * p2.x + t * t * t * p3.x)
                            by = (u * u * u * p0.y + 3 * u * u * t * p1.y
                                  + 3 * u * t * t * p2.y + t * t * t * p3.y)
                            bz = u * p0.z + t * p3.z
                            sz = surface_z(world, bx, by, bz)
                            pts.append(unreal.Vector(bx, by,
                                                     (sz + Z_OFFSET) if sz is not None else bz))

                        act = eas.spawn_actor_from_class(child_cls, pts[0])
                        act.set_actor_label(
                            "Turn%s_%s%+05d_to_%s%+05d_%s"
                            % (kind, src["road"], src["offset"],
                               dst["road"], dst["offset"], src["idx"]))
                        act.tags = [TAG]
                        tsp = act.get_component_by_class(unreal.SplineComponent)
                        tsp.clear_spline_points(False)
                        for p in pts:
                            tsp.add_spline_point(p, WS, False)
                        for i in range(tsp.get_number_of_spline_points()):
                            tsp.set_spline_point_type(
                                i, unreal.SplinePointType.CURVE_CLAMPED, False)
                        tsp.update_spline()
                        if not mark_edited(tsp):
                            w("!! spline_has_been_edited 设置失败: %s"
                              % act.get_actor_label())
                        made += 1

                    if len(picked) != 2:
                        w("    注意: %s 的%s转候选是 %d 条（预期 2）"
                          % (src["label"], "右" if want_right else "左", len(picked)))

    w("")
    w("共生成转弯道 %d 条，跳过 %d 条（首尾过近）" % (made, skipped))
    w("命名: TurnL_/TurnR_ + 起点车道 + to + 终点车道")
    w("")
    w("!! 提醒: BP_car_base 目前没有'进路口走哪条'的选择逻辑，")
    w("   车会同时看到直行/左转/右转三种候选，抓到哪条取决于 TraceForNewPath 先扫到谁。")
    w("完成。关卡尚未保存。")

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[gen_turns] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
