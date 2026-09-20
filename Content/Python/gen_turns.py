# -*- coding: utf-8 -*-
"""在路口生成左右转通道。

规则：
    外道 -> 左转，内道 -> 右转
    起点 = 来向路口段的**第一个点**，终点 = 去向路口段的**最后一个点**
    中间用三次贝塞尔连成平滑曲线，两端与来路/去路相切

几个不能想当然的地方：

左右不能看偏移的正负号。每条道路样条的走向是作者随手画的，
形状39 的"正方向"和形状13 没有任何固定关系。用叉乘做几何判断：
UE 里 Cross(Forward, Right) = +Z，所以 cross(进入方向, 驶出方向).z > 0 是右转。

内外道不能写死偏移值。主路是 +-675/+-225、次路只有 +-270，
写死过一次（375/125），车道宽度一改就全失效。改成按每条路当场算：
同向车道里 |偏移| 最大的是外道，最小的是内道。次路同向只有一条车道，
内外道是同一条，那条既左转也右转（没有别的车道可分工）。

高度不在路口里打射线，直接在首尾之间过渡。路口内部两条路的路面是叠着的
（实测差约 200cm），打射线取到哪一层全看运气，转弯道会忽上忽下。
首尾两点来自已经桥接好的直行段，本身就是连续的。

依赖：先跑 gen_traffic_lanes.py。
用法：py gen_turns.py
"""

import traceback

import unreal

CHILD_BP = "/Game/PS2DEM/BP_TrafficLine1_IntersectionChild"
LANE_TAG_PREFIX = "ClaudeGenLane"
TAG = "ClaudeGenTurn"

ONLY_INTERSECTION = "Intersection_04"   # 只处理这个路口；"" = 全部。先拿一个试。
CLUSTER_DIST = 2500.0    # 世界坐标相距小于此值的路口段归为同一个物理路口
BEZIER_K = 0.45          # 控制点伸出长度占首尾直线距离的比例
SAMPLES = 14             # 每条转弯线采样点数
MIN_TURN_LEN = 200.0     # 首尾太近的不生成（同一条线自己接自己）
BOX_SCALE = 2.0          # 和车道保持一致（蓝图默认半尺寸 32 -> 64）

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "gen_turns.txt"
lines = []
_box_default = None


def w(s=""):
    lines.append(str(s))
    unreal.log("[gen_turns] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def mark_edited(sp):
    """打"已被编辑"标记，否则构造脚本一重跑就把点冲回蓝图默认的 2 个点。

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


def scale_box(a):
    global _box_default
    if BOX_SCALE == 1.0:
        return
    for c in a.get_components_by_class(unreal.BoxComponent):
        if _box_default is None:
            _box_default = c.get_unscaled_box_extent()
        b = _box_default
        try:
            c.modify(True)
            c.set_editor_property("box_extent", unreal.Vector(
                b.x * BOX_SCALE, b.y * BOX_SCALE, b.z * BOX_SCALE))
        except Exception:
            pass
        break


def parse_label(lbl):
    """Inter_形状13_-0675_I05 -> ("形状13", -675, "I05")"""
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


def bezier_points(src, dst):
    """首尾相切的三次贝塞尔，高度在两端之间线性过渡。首尾太近返回 None。"""
    p0, p3 = src["p_in"], dst["p_out"]
    span = ((p3.x - p0.x) ** 2 + (p3.y - p0.y) ** 2) ** 0.5
    if span < MIN_TURN_LEN:
        return None
    k = span * BEZIER_K
    p1 = unreal.Vector(p0.x + src["d_in"].x * k, p0.y + src["d_in"].y * k, 0)
    p2 = unreal.Vector(p3.x - dst["d_out"].x * k, p3.y - dst["d_out"].y * k, 0)
    pts = []
    for i in range(SAMPLES + 1):
        t = float(i) / SAMPLES
        u = 1.0 - t
        bx = (u * u * u * p0.x + 3 * u * u * t * p1.x
              + 3 * u * t * t * p2.x + t * t * t * p3.x)
        by = (u * u * u * p0.y + 3 * u * u * t * p1.y
              + 3 * u * t * t * p2.y + t * t * t * p3.y)
        pts.append(unreal.Vector(bx, by, u * p0.z + t * p3.z))
    return pts


def spawn_turn(eas, cls, pts, label, src_actor):
    a = eas.spawn_actor_from_class(cls, pts[0])
    a.set_actor_label(label)
    a.tags = [TAG]
    sp = a.get_component_by_class(unreal.SplineComponent)
    sp.clear_spline_points(False)
    for p in pts:
        sp.add_spline_point(p, WS, False)
    for i in range(sp.get_number_of_spline_points()):
        sp.set_spline_point_type(i, unreal.SplinePointType.CURVE_CLAMPED, False)
    sp.update_spline()
    if not mark_edited(sp):
        w("  !! %s 的 spline_has_been_edited 设置失败" % label)
    scale_box(a)
    # 灯组跟来向车道走：转弯和它的直行是同一个放行相位
    try:
        a.set_editor_property("LightNumber",
                              src_actor.get_editor_property("LightNumber"))
    except Exception:
        pass
    return a


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    removed = 0
    for a in actors:
        if TAG in [str(t) for t in a.tags]:
            eas.destroy_actor(a)
            removed += 1
    w("清除旧转弯道 %d 个" % removed)

    focus = None
    if ONLY_INTERSECTION:
        for a in actors:
            if a.get_actor_label() == ONLY_INTERSECTION:
                focus = a.get_actor_location()
                break
        if focus is None:
            w("!! 找不到 %s，改为处理全部路口" % ONLY_INTERSECTION)
        else:
            w("只处理 %s @ (%.0f, %.0f)" % (ONLY_INTERSECTION, focus.x, focus.y))

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
            "label": a.get_actor_label(), "actor": a,
            "p_in": sp.get_location_at_distance_along_spline(0.0, WS),
            "d_in": norm2d(sp.get_direction_at_distance_along_spline(0.0, WS)),
            "p_out": sp.get_location_at_distance_along_spline(L, WS),
            "d_out": norm2d(sp.get_direction_at_distance_along_spline(L, WS)),
        })
    w("路口段 %d 个" % len(segs))
    if not segs:
        w("!! 没找到 Inter_* actor，请先跑 gen_traffic_lanes.py")
        flush()
        return

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
    if focus is not None:
        clusters = [cl for cl in clusters
                    if (cl["center"] - focus).length() < CLUSTER_DIST]
    w("要处理的物理路口 %d 个" % len(clusters))
    w("")
    flush()

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
            offs = [abs(x["offset"]) for x in roads[a_name]]
            outer_abs, inner_abs = max(offs), min(offs)
            if outer_abs == inner_abs:
                w("  %s 只有一种 |偏移|=%d，同一条车道既左转也右转"
                  % (a_name, outer_abs))

            for src in roads[a_name]:
                ao = abs(src["offset"])
                jobs = []
                if ao == outer_abs:
                    jobs.append((False, "L"))     # 外道左转
                if ao == inner_abs:
                    jobs.append((True, "R"))      # 内道右转

                for want_right, kind in jobs:
                    picked = []
                    for b_name in names:
                        if b_name == a_name:
                            continue
                        for dst in roads[b_name]:
                            cz = (src["d_in"].x * dst["d_out"].y
                                  - src["d_in"].y * dst["d_out"].x)
                            if (cz > 0) == want_right:
                                picked.append(dst)
                    if not picked:
                        w("  %s 的%s转没有候选去向"
                          % (src["label"][:30], "右" if want_right else "左"))
                        continue
                    for dst in picked:
                        pts = bezier_points(src, dst)
                        if pts is None:
                            skipped += 1
                            continue
                        label = ("Turn%s_%s%+05d_to_%s%+05d_%s"
                                 % (kind, src["road"], src["offset"],
                                    dst["road"], dst["offset"], src["idx"]))
                        spawn_turn(eas, child_cls, pts, label, src["actor"])
                        made += 1
                    w("  %-32s %s转 -> %d 条" %
                      (src["label"][:32], "右" if want_right else "左", len(picked)))

    w("")
    w("共生成转弯道 %d 条，跳过 %d 条（首尾过近）" % (made, skipped))
    w("命名: TurnL_/TurnR_ + 起点车道 + to + 终点车道")
    w("灯组继承自来向车道（转弯和它的直行是同一个放行相位）")
    w("")
    w("!! 车目前没有'进路口走哪条'的选择逻辑：直行/左转/右转三种候选它都看得见，")
    w("   抓到哪条取决于 TraceForNewPath 先扫到谁。所以先只在一个路口试，")
    w("   跑 Simulate 看车的实际行为，再决定要不要铺开。")
    w("删除：clean_gen.py 把 TAGS 设成 [\"ClaudeGenTurn\"] 跑一次。")
    w("完成。关卡尚未保存。")
    flush()


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[gen_turns] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
