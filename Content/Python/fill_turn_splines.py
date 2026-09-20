# -*- coding: utf-8 -*-
"""把左右转曲线写进路口段 actor 自带的 SplineLeft / SplineRight。

和之前那版独立 Turn actor 的区别：转弯路径就挂在原来那个路口段 actor 上，
和直行的 Spline 共用同一个 Box。车探测到一个 Box 就拿到三条候选路径。

分工按"外道左拐、内道右拐"：
    外道（同向车道里 |偏移| 最大的）-> 只填 SplineLeft
    内道（|偏移| 最小的）          -> 只填 SplineRight
    次路同向只有一条车道时内外道是同一条，两条都填
用不上的那条会被**压成零长**（两个点都在 actor 原点），
这是个明确的"这里没有转弯"标记，蓝图里判 GetSplineLength() < 1 就能跳过。
不压的话它会保留蓝图默认的 2 点 100cm，车可能抓到这根残桩开出去。

起点 = 本段直行样条的第一个点，终点 = 去向路口段直行样条的最后一个点，
中间三次贝塞尔、两端相切。高度在首尾之间线性过渡，不在路口里打射线——
路口内部两条路的路面叠着（实测差约 200cm），取到哪层看运气。

左右用叉乘判，不看偏移正负号：每条道路样条的走向是作者随手画的，
形状39 的"正方向"和形状13 没有固定关系。
UE 里 Cross(Forward, Right) = +Z，所以 cross(进入方向, 驶出方向).z > 0 是右转。

用法：py fill_turn_splines.py
"""

import math
import traceback

import unreal

LANE_TAG_PREFIX = "ClaudeGenLane"
ONLY_INTERSECTION = "Intersection_04"   # 只处理这个路口；"" = 全部
COLLAPSE_ALL_FIRST = True
# 开工前先把**全图**所有路口段的 SplineLeft/SplineRight 压成零长。
# 不这么做的话，没被处理到的路口上这两条组件一直停在蓝图默认值——
# 2 个点、长 100、方向一律朝 +X，和路的走向无关。152 个路口段就是
# 300 根散在全图的小棍子（视口里看着一团乱），而且车有可能抓到一根开出去。
# 只处理单个路口做试验时，这一步尤其必要。
CLUSTER_DIST = 2500.0
# BEZIER_K 已弃用：那种「切线各伸出 k 倍距离」的画法要猜 k，
# 猜大了曲线会在反面鼓出个包变成 S 形（实测就是这么坏的）。
# 现在用两条切线的交点当控制点，不需要任何可调参数。
SAMPLES = 14
TIGHT_K = 0.35           # 角点退化时三次贝塞尔的控制点伸出比例
DIR_SAMPLE = 100.0       # 求首尾朝向时，沿样条取多长的一段作差
MIN_TURN_LEN = 100.0     # 首尾太近的不生成。200 太严：紧角的转弯首尾只差 187cm
                         # （实测 形状16-0270 -> 形状13-0675），那是条合法的短弯

LEFT_COMP, RIGHT_COMP, MAIN_COMP = "SplineLeft", "SplineRight", "Spline"

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "fill_turn_splines.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[turnfill] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def comp_by_name(a, name):
    for c in a.get_components_by_class(unreal.SplineComponent):
        if c.get_name() == name:
            return c
    return None


def mark_edited(sp):
    for n in ("spline_has_been_edited", "b_spline_has_been_edited"):
        try:
            sp.set_editor_property(n, True)
            return True
        except Exception:
            continue
    return False


def write_spline(sp, pts):
    sp.clear_spline_points(False)
    for p in pts:
        sp.add_spline_point(p, WS, False)
    for i in range(sp.get_number_of_spline_points()):
        sp.set_spline_point_type(i, unreal.SplinePointType.CURVE_CLAMPED, False)
    sp.update_spline()
    mark_edited(sp)


def collapse(a, sp):
    """压成零长，明确表示"这条没有转弯"。"""
    o = a.get_actor_location()
    write_spline(sp, [o, o])


def parse_label(lbl):
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
        return None          # 不要兜底成 (1,0,0)：那会把"取不到方向"
                             # 伪装成"方向朝 +X"，后面所有判断都建立在假数据上
    return unreal.Vector(v.x / h, v.y / h, 0.0)


def dir_from_points(sp, d_a, d_b):
    """用样条上两个采样点作差求方向。

    不用 get_direction_at_distance_along_spline——实测它在这些样条上
    返回零向量（摊开的几何里所有朝向都打成了 +0.0°）。
    而 get_location_at_distance_along_spline 的结果是对的（首尾坐标都正常），
    所以从坐标反推方向是可靠的那条路。
    """
    a = sp.get_location_at_distance_along_spline(d_a, WS)
    b = sp.get_location_at_distance_along_spline(d_b, WS)
    return norm2d(unreal.Vector(b.x - a.x, b.y - a.y, 0.0))


def corner_point(p0, d0, p3, d3):
    """两条切线的交点：从 p0 沿进入方向的射线，和从 p3 沿驶出方向倒推的射线。

    这就是这个弯的"角点"。用它当控制点画二次贝塞尔，曲线天然与两端相切，
    而且**只会朝角点鼓**——不可能像"切线各伸出 k 长度"那种画法一样，
    k 猜大了就在反面鼓出个包、整条线变成 S 形。

    解 p0 + t*d0 + s*d3 = p3：两边同时叉乘 d3 得 t，叉乘 d0 得 s。
    t<=0 说明交点在来向后方、s<=0 说明在去向前方——两种都意味着
    切线方向取错了，返回 None 让调用方报出来，而不是硬画一条怪线。
    """
    den = d0.x * d3.y - d0.y * d3.x
    if abs(den) < 1e-6:
        return None, "两端切线平行"
    ex, ey = p3.x - p0.x, p3.y - p0.y
    t = (ex * d3.y - ey * d3.x) / den
    sgn = (d0.x * ey - d0.y * ex) / den
    if t <= 0.0:
        return None, "交点在来向后方(t=%.0f)" % t
    if sgn <= 0.0:
        return None, "交点在去向前方(s=%.0f)" % sgn
    return unreal.Vector(p0.x + d0.x * t, p0.y + d0.y * t, 0.0), ""


def bezier(src, dst):
    """返回 (点列, 首尾直线距离, 说明)。画不出来时点列为 None。"""
    p0, p3 = src["p_in"], dst["p_out"]
    span = ((p3.x - p0.x) ** 2 + (p3.y - p0.y) ** 2) ** 0.5
    if span < MIN_TURN_LEN:
        return None, span, "首尾过近"

    c, why = corner_point(p0, src["d_in"], p3, dst["d_out"])
    if c is not None:
        pts = []
        for i in range(SAMPLES + 1):
            t = float(i) / SAMPLES
            u = 1.0 - t
            pts.append(unreal.Vector(
                u * u * p0.x + 2 * u * t * c.x + t * t * p3.x,
                u * u * p0.y + 2 * u * t * c.y + t * t * p3.y,
                u * p0.z + t * p3.z))
        return pts, span, ""

    # 角点退化（首尾几乎重合的紧角，实测 t=-14）。不能就这么丢掉——
    # 那是路口最里侧一个合法的急弯。退回三次贝塞尔，控制点只伸出
    # span 的一小段：这里 span 本来就短，不会像当初 k 猜大时那样鼓成 S 形。
    k = span * TIGHT_K
    p1x, p1y = p0.x + src["d_in"].x * k, p0.y + src["d_in"].y * k
    p2x, p2y = p3.x - dst["d_out"].x * k, p3.y - dst["d_out"].y * k
    pts = []
    for i in range(SAMPLES + 1):
        t = float(i) / SAMPLES
        u = 1.0 - t
        pts.append(unreal.Vector(
            u * u * u * p0.x + 3 * u * u * t * p1x + 3 * u * t * t * p2x + t * t * t * p3.x,
            u * u * u * p0.y + 3 * u * u * t * p1y + 3 * u * t * t * p2y + t * t * t * p3.y,
            u * p0.z + t * p3.z))
    return pts, span, "紧角(%s)，用三次贝塞尔" % why


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

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
    unparsed = []
    missing = bad_dir = 0
    for a in actors:
        if not any(str(t).startswith(LANE_TAG_PREFIX) for t in a.tags):
            continue
        lbl = a.get_actor_label()
        info = parse_label(lbl)
        if info is None:
            # 带着车道 tag 却认不出名字——多半是被改过名。
            # 静默跳过的话它就从所有统计里消失了，而少一条车道会连带
            # 改变内外道的判定（实测少了 -0225，南向就没有内道做右转了）。
            if lbl.startswith("Inter"):
                unparsed.append(lbl)
            continue
        main = comp_by_name(a, MAIN_COMP)
        left = comp_by_name(a, LEFT_COMP)
        right = comp_by_name(a, RIGHT_COMP)
        if main is None or main.get_number_of_spline_points() < 2:
            continue
        if left is None or right is None:
            missing += 1
            continue
        L = main.get_spline_length()
        step = min(DIR_SAMPLE, L * 0.4)
        d_in = dir_from_points(main, 0.0, step)
        d_out = dir_from_points(main, L - step, L)
        if d_in is None or d_out is None:
            bad_dir += 1
            continue
        segs.append({
            "road": info[0], "offset": info[1], "actor": a,
            "label": a.get_actor_label(), "left": left, "right": right,
            "p_in": main.get_location_at_distance_along_spline(0.0, WS),
            "d_in": d_in,
            "p_out": main.get_location_at_distance_along_spline(L, WS),
            "d_out": d_out,
        })
    w("路口段 %d 个" % len(segs))
    if COLLAPSE_ALL_FIRST:
        n = 0
        for s_ in segs:
            collapse(s_["actor"], s_["left"])
            collapse(s_["actor"], s_["right"])
            n += 2
        w("先把全图 %d 条转弯样条压成零长（清掉蓝图默认的 100cm 残桩）" % n)
    if unparsed:
        w("!! 有 %d 个 actor 带着车道 tag 但名字对不上 Inter_<路>_<偏移>_<编号> 格式，"
          % len(unparsed))
        w("   已跳过——它们不会参与内外道判定，可能导致某个方向少一条转弯：")
        for lbl in unparsed[:10]:
            w("     %s" % lbl)
    if bad_dir:
        w("!! 有 %d 个段算不出方向（样条退化），已跳过" % bad_dir)
    if missing:
        w("!! 有 %d 个 actor 上找不到 %s/%s —— 那些是加组件之前生成的旧实例，"
          % (missing, LEFT_COMP, RIGHT_COMP))
        w("   重跑 gen_traffic_lanes.py 重建一遍就有了。")
    if not segs:
        w("!! 没有可处理的路口段。")
        flush()
        return

    def mid(s_):
        return unreal.Vector((s_["p_in"].x + s_["p_out"].x) / 2.0,
                             (s_["p_in"].y + s_["p_out"].y) / 2.0,
                             (s_["p_in"].z + s_["p_out"].z) / 2.0)

    if focus is not None:
        # 指定了路口就直接按到它的距离圈一遍，不做贪心聚类。
        # 贪心 + 中心不更新会把同一个物理路口劈成两簇：实测 Intersection_04
        # 少了 Inter_形状13_-0225_I04，四车道主路只进来 3 条，
        # 南向就没有内道可以做右转了。
        clusters = [{"center": focus,
                     "segs": [s for s in segs
                              if (mid(s) - focus).length() < CLUSTER_DIST]}]
    else:
        clusters = []
        for s in segs:
            c = mid(s)
            got = None
            for cl in clusters:
                if (cl["center"] - c).length() < CLUSTER_DIST:
                    got = cl
                    break
            if got is None:
                clusters.append({"center": c, "segs": [s]})
            else:
                got["segs"].append(s)
                n = len(got["segs"])
                # 中心要跟着更新，否则先来的那条段的位置会一直当基准，
                # 路口边缘的段就圈不进来
                got["center"] = unreal.Vector(
                    sum(mid(x).x for x in got["segs"]) / n,
                    sum(mid(x).y for x in got["segs"]) / n,
                    sum(mid(x).z for x in got["segs"]) / n)
    w("要处理的物理路口 %d 个" % len(clusters))
    w("")
    flush()

    n_left = n_right = n_empty = 0
    for ci, cl in enumerate(clusters):
        roads = {}
        for s in cl["segs"]:
            roads.setdefault(s["road"], []).append(s)
        w("路口 %02d  中心(%.0f, %.0f)  道路: %s"
          % (ci, cl["center"].x, cl["center"].y, " x ".join(sorted(roads))))
        # 先把每条段的实际几何摊开——方向一旦取错，后面所有判断都是错的，
        # 而"切线平行"这种报错本身分不出是数据问题还是取值问题。
        w("  %-30s %-22s %-22s %7s %7s %7s" %
          ("段", "起点", "终点", "进°", "出°", "长度"))
        for s_ in sorted(cl["segs"], key=lambda x: x["label"]):
            ai = math.degrees(math.atan2(s_["d_in"].y, s_["d_in"].x))
            ao = math.degrees(math.atan2(s_["d_out"].y, s_["d_out"].x))
            seglen = ((s_["p_out"].x - s_["p_in"].x) ** 2
                      + (s_["p_out"].y - s_["p_in"].y) ** 2) ** 0.5
            w("  %-30s (%7.0f,%7.0f) (%7.0f,%7.0f) %+7.1f %+7.1f %7.0f" %
              (s_["label"][:30], s_["p_in"].x, s_["p_in"].y,
               s_["p_out"].x, s_["p_out"].y, ai, ao, seglen))
        w("")
        if len(roads) < 2:
            w("  只有一条路，全部压成零长")
            for s in cl["segs"]:
                collapse(s["actor"], s["left"])
                collapse(s["actor"], s["right"])
                n_empty += 2
            continue

        names = sorted(roads)
        for a_name in names:
            offs = [abs(x["offset"]) for x in roads[a_name]]
            outer_abs, inner_abs = max(offs), min(offs)
            if outer_abs == inner_abs:
                w("  %s 同向只有一种 |偏移|=%d，同一条车道既左转也右转"
                  % (a_name, outer_abs))

            for src in roads[a_name]:
                ao = abs(src["offset"])
                do_left = (ao == outer_abs)
                do_right = (ao == inner_abs)

                for want_right, comp, do_it in (
                        (False, src["left"], do_left),
                        (True, src["right"], do_right)):
                    if not do_it:
                        collapse(src["actor"], comp)
                        n_empty += 1
                        continue
                    cands, rejected = [], []
                    for b_name in names:
                        if b_name == a_name:
                            continue
                        for dst in roads[b_name]:
                            cz = (src["d_in"].x * dst["d_out"].y
                                  - src["d_in"].y * dst["d_out"].x)
                            if (cz > 0) == want_right:
                                pts, span, why = bezier(src, dst)
                                if pts is None:
                                    rejected.append((dst["label"], why))
                                else:
                                    cands.append((span, dst, pts, why))
                    if not cands:
                        w("  %-30s %s转 无候选，压零长"
                          % (src["label"][:30], "右" if want_right else "左"))
                        for lbl, why in rejected[:3]:
                            w("        排除 %-28s %s" % (lbl[:28], why))
                        collapse(src["actor"], comp)
                        n_empty += 1
                        continue
                    # 多个候选时取最短的那条——转弯自然走最近的目标车道
                    cands.sort(key=lambda t: t[0])
                    span, dst, pts, note = cands[0]
                    write_spline(comp, pts)
                    if want_right:
                        n_right += 1
                    else:
                        n_left += 1
                    others = ("，另有 %d 个候选" % (len(cands) - 1)) if len(cands) > 1 else ""
                    a_in = math.degrees(math.atan2(src["d_in"].y, src["d_in"].x))
                    a_out = math.degrees(math.atan2(dst["d_out"].y, dst["d_out"].x))
                    turn = (a_out - a_in + 540.0) % 360.0 - 180.0
                    w("  %-30s %s转 -> %-28s 跨度 %.0f  进%+.0f° 出%+.0f° 转%+.0f°%s"
                      % (src["label"][:30], "右" if want_right else "左",
                         dst["label"][:28], span, a_in, a_out, turn,
                         others + ("  " + note if note else "")))
        flush()

    w("")
    w("写入左转 %d 条，右转 %d 条，压成零长 %d 条" % (n_left, n_right, n_empty))
    w("零长的那些表示「这条车道没有这个方向的转弯」，")
    w("蓝图里判 GetSplineLength() < 1 就能跳过。")
    w("")
    w("!! 蓝图那边还没有「三条里挑一条」的逻辑：")
    w("   TraceForNewPath 现在多半是 get_component_by_class 抓第一个 SplineComponent，")
    w("   也就是永远只走直行那条。要让转弯真的生效，得在蓝图里加选择逻辑。")
    w("   先跑 Simulate 看现在的实际行为，再决定怎么改。")
    w("关卡尚未保存。")
    flush()


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[turnfill] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
