# -*- coding: utf-8 -*-
"""检测有多少路面被地形埋住。

判据：射线在第一个 blocking 命中处停止，所以只看**最上面那层是谁**。
    SplineMeshComponent  -> 路面露在外面
    Landscape*Component  -> 地形盖在路面上方 = 被埋

横向采样按**每条路实测半宽的比例**取，不是固定厘米数——
路宽实测 1080~1800 不等（形状13 是主路 1800），固定 ±450 对宽路根本测不到边缘，
会给出虚高的好看数字。0.95 那一档才是真正的路缘。

跑 deform_terrain.py 前后各跑一次对比。
用法：py diag_road_buried.py
"""

import traceback

import unreal

ROAD_LABELS = []                 # 空 = 全部；可填 ["形状 13"]
SKIP_KEYWORDS = ["River"]        # 河流样条不是路
LONG_STEP = 1000.0
FRACTIONS = [-0.95, -0.6, 0.0, 0.6, 0.95]   # 相对半宽的比例
WIDTH_PROBE_MAX = 1400.0
WIDTH_PROBE_STEP = 50.0
WIDTH_STATIONS = 9
UP = 60000.0
DOWN = 60000.0
MAX_STEPS = 8

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "road_buried.txt"

lines = []


def w(s=""):
    lines.append(str(s))


def top_surface(world, x, y, z_hint):
    """最上面那层是什么。返回 (road / terrain / other / none, Z)"""
    start = unreal.Vector(x, y, z_hint + UP)
    end = unreal.Vector(x, y, z_hint - DOWN)
    try:
        hit = unreal.SystemLibrary.line_trace_single(
            world, start, end, unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
            False, [], unreal.DrawDebugTrace.NONE, True)
        if hit is None:
            return "none", None
        d = hit.to_dict()
        if not d.get("blocking_hit"):
            return "none", None
        comp = d.get("hit_component")
        z = d["impact_point"].z
        if comp is None:
            return "other", z
        cn = comp.get_class().get_name()
        if "SplineMesh" in cn:
            return "road", z
        if "Landscape" in cn:
            return "terrain", z
        return "other", z
    except Exception:
        return "none", None


def road_below(world, x, y, z_hint):
    """穿过地形找下面的路面（用于测宽度：被埋处也要算作路面存在）。"""
    top = z_hint + UP
    for _ in range(MAX_STEPS):
        start = unreal.Vector(x, y, top)
        end = unreal.Vector(x, y, z_hint - DOWN)
        try:
            hit = unreal.SystemLibrary.line_trace_single(
                world, start, end, unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
                False, [], unreal.DrawDebugTrace.NONE, True)
        except Exception:
            return None
        if hit is None:
            return None
        d = hit.to_dict()
        if not d.get("blocking_hit"):
            return None
        comp = d.get("hit_component")
        z = d["impact_point"].z
        cn = comp.get_class().get_name() if comp is not None else "?"
        if "SplineMesh" in cn:
            return z
        top = z - 1.0
        if top <= z_hint - DOWN:
            return None
    return None


def horiz_right(sp, d):
    r = sp.get_right_vector_at_distance_along_spline(d, WS)
    h = (r.x * r.x + r.y * r.y) ** 0.5
    if h < 1e-4:
        return 1.0, 0.0
    return r.x / h, r.y / h


def measure_half_width(world, sp, total):
    halves = []
    for k in range(1, WIDTH_STATIONS + 1):
        d = total * k / (WIDTH_STATIONS + 1.0)
        loc = sp.get_location_at_distance_along_spline(d, WS)
        hx, hy = horiz_right(sp, d)
        for sign in (-1.0, 1.0):
            off, last_ok = 0.0, 0.0
            while off <= WIDTH_PROBE_MAX:
                if road_below(world, loc.x + hx * off * sign,
                              loc.y + hy * off * sign, loc.z) is None:
                    break
                last_ok = off
                off += WIDTH_PROBE_STEP
            if last_ok > 0:
                halves.append(last_ok)
    if not halves:
        return None
    halves.sort()
    return halves[len(halves) // 2]


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()

    roads = []
    for a in eas.get_all_level_actors():
        if a.get_class().get_name() != "BP_PS2DEMSplineActor_v3_C":
            continue
        lbl = a.get_actor_label().strip()
        if any(k.lower() in lbl.lower() for k in SKIP_KEYWORDS):
            continue
        if ROAD_LABELS and lbl not in ROAD_LABELS:
            continue
        sp = a.get_component_by_class(unreal.SplineComponent)
        if sp:
            roads.append((lbl, sp))
    roads.sort()
    w("检测 %d 条道路，沿路间距 %.0f cm" % (len(roads), LONG_STEP))
    w("横向按实测半宽的比例采样: %s（0.95 = 真正的路缘）"
      % "/".join("%.2f" % f for f in FRACTIONS))
    w("")

    rows = []
    frac_tot = dict((f, 0) for f in FRACTIONS)
    frac_bur = dict((f, 0) for f in FRACTIONS)

    for label, sp in roads:
        total = sp.get_spline_length()
        half = measure_half_width(world, sp, total)
        if half is None:
            rows.append((label, total, None, 0, 0, 0, 0, 0))
            continue

        c_tot = c_bur = e_tot = e_bur = none_cnt = 0
        deepest = 0.0
        d = 0.0
        while d <= total:
            loc = sp.get_location_at_distance_along_spline(d, WS)
            hx, hy = horiz_right(sp, d)
            cross = []
            for f in FRACTIONS:
                off = half * f
                kind, z = top_surface(world, loc.x + hx * off,
                                      loc.y + hy * off, loc.z)
                cross.append((f, kind, z))
                frac_tot[f] += 1
                if kind == "terrain":
                    frac_bur[f] += 1
                elif kind == "none":
                    none_cnt += 1
                if f == 0.0:
                    c_tot += 1
                    if kind == "terrain":
                        c_bur += 1
                elif abs(f) > 0.9:
                    e_tot += 1
                    if kind == "terrain":
                        e_bur += 1
            road_zs = [z for _f, k, z in cross if k == "road" and z is not None]
            if road_zs:
                ref = sum(road_zs) / len(road_zs)
                for _f, k, z in cross:
                    if k == "terrain" and z is not None and z - ref > deepest:
                        deepest = z - ref
            d += LONG_STEP

        rows.append((label, total, half, c_tot, c_bur, e_tot, e_bur, deepest))

    w("%-12s %7s %7s %9s %9s %8s" %
      ("道路", "长度m", "半宽cm", "中心埋率", "路缘埋率", "最深cm"))
    w("-" * 60)
    for (label, total, half, c_tot, c_bur, e_tot, e_bur, dp) in rows:
        if half is None:
            w("%-12s %7.0f %7s  (探测不到路面)" % (label, total / 100.0, "-"))
            continue
        cp = 100.0 * c_bur / c_tot if c_tot else 0.0
        ep = 100.0 * e_bur / e_tot if e_tot else 0.0
        flag = "  <<<" if ep > 15 or cp > 10 else ""
        w("%-12s %7.0f %7.0f %8.1f%% %8.1f%% %8.0f%s"
          % (label, total / 100.0, half, cp, ep, dp, flag))

    w("")
    w("=" * 60)
    w("按横向比例的被埋率:")
    for f in FRACTIONS:
        t, b = frac_tot[f], frac_bur[f]
        w("   %+5.2f × 半宽 : %5d 点中 %5d 被埋 (%.1f%%)%s"
          % (f, t, b, 100.0 * b / t if t else 0.0,
             "   <- 路缘" if abs(f) > 0.9 else ""))

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    unreal.log("[road_buried] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[road_buried] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
