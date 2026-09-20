# -*- coding: utf-8 -*-
"""交汇处只保留一层路面：把次要道路在路口范围内的路面段隐藏掉。

问题：两条路在交汇处各铺各的，两块 14cm 厚的板子互相穿插，看起来在打架。
做法：每个路口按实测半宽选出"主路"（宽的赢，同宽比长度），
      把次要道路在路口半径内的 SplineMeshComponent 隐藏，主路完整穿过去。

归属判断：一段路面属于离它最近的那条道路样条。交汇正中心会有小范围模糊，
但那里本来就被主路盖住，影响不大。

RESTORE=True 可以把所有隐藏过的段恢复显示。
用法：py fix_intersection_overlap.py
"""

import traceback

import unreal

RESTORE = True                 # True = 恢复所有被隐藏的路面段
SKIP_KEYWORDS = ["River"]

XY_CROSS = 200.0               # 两条样条最近距离小于此值算相交
Z_CROSS = 600.0                # Z 差大于此值是立交，不处理
DETECT_STEP = 250.0
MERGE_DIST = 2500.0
HIDE_RADIUS = 1300.0           # 路口中心多大范围内隐藏次路（主路半宽 900 + 余量）

WIDTH_PROBE_MAX = 1400.0
WIDTH_PROBE_STEP = 50.0
WIDTH_STATIONS = 7
UP = 60000.0
DOWN = 60000.0
MAX_STEPS = 8

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "fix_intersection.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[fixinter] %s" % s)


def road_top_z(world, x, y, z_hint):
    top = z_hint + UP
    for _ in range(MAX_STEPS):
        try:
            hit = unreal.SystemLibrary.line_trace_single(
                world, unreal.Vector(x, y, top), unreal.Vector(x, y, z_hint - DOWN),
                unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, False, [],
                unreal.DrawDebugTrace.NONE, True)
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
                if road_top_z(world, loc.x + hx * off * sign,
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


def comp_world_pos(c):
    try:
        return c.k2_get_component_location()
    except Exception:
        pass
    try:
        return c.get_world_location()
    except Exception:
        return None


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    actors = eas.get_all_level_actors()

    land = None
    for a in actors:
        if a.get_class().get_name() == "Landscape":
            land = a
            break
    if land is None:
        w("!! 找不到 Landscape")
        return
    sms = list(land.get_components_by_class(unreal.SplineMeshComponent))
    w("路面段共 %d 个" % len(sms))

    if RESTORE:
        n = 0
        for c in sms:
            try:
                if not c.is_visible():
                    c.set_visibility(True, False)
                    n += 1
            except Exception:
                pass
        w("已恢复显示 %d 段。关卡尚未保存。" % n)
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
        return

    roads = []
    for a in actors:
        if a.get_class().get_name() != "BP_PS2DEMSplineActor_v3_C":
            continue
        lbl = a.get_actor_label().strip()
        if any(k.lower() in lbl.lower() for k in SKIP_KEYWORDS):
            continue
        sp = a.get_component_by_class(unreal.SplineComponent)
        if sp:
            roads.append([lbl, sp, None])
    w("道路 %d 条，测量各自半宽…" % len(roads))
    for r in roads:
        r[2] = measure_half_width(world, r[1], r[1].get_spline_length()) or 0.0
    w("")

    # ---- 找所有交汇点 ----
    crossings = []
    for i in range(len(roads)):
        for j in range(i + 1, len(roads)):
            na, spa, wa = roads[i]
            nb, spb, wb = roads[j]
            raw = []
            d = 0.0
            tot = spa.get_spline_length()
            while d <= tot:
                p = spa.get_location_at_distance_along_spline(d, WS)
                q = spb.find_location_closest_to_world_location(p, WS)
                dxy = ((q.x - p.x) ** 2 + (q.y - p.y) ** 2) ** 0.5
                if dxy < XY_CROSS and abs(q.z - p.z) < Z_CROSS:
                    raw.append(p)
                d += DETECT_STEP
            # 聚类
            for p in raw:
                merged = False
                for c in crossings:
                    if c["roads"] == (na, nb) and (c["pos"] - p).length() < MERGE_DIST:
                        merged = True
                        break
                if not merged:
                    # 宽的当主路；同宽比长度
                    if wa > wb or (abs(wa - wb) < 1.0 and
                                   spa.get_spline_length() >= spb.get_spline_length()):
                        major, minor = na, nb
                    else:
                        major, minor = nb, na
                    crossings.append({"roads": (na, nb), "pos": p,
                                      "major": major, "minor": minor})
    w("检测到交汇 %d 处" % len(crossings))
    for c in crossings:
        w("   (%.0f, %.0f)  %s x %s  -> 保留 %s"
          % (c["pos"].x, c["pos"].y, c["roads"][0], c["roads"][1], c["major"]))
    w("")

    # ---- 隐藏次路在路口范围内的路面段 ----
    by_name = dict((r[0], r[1]) for r in roads)
    hidden = 0
    for c in crossings:
        minor_sp = by_name.get(c["minor"])
        major_sp = by_name.get(c["major"])
        if minor_sp is None or major_sp is None:
            continue
        for sm in sms:
            p = comp_world_pos(sm)
            if p is None:
                continue
            if (p - c["pos"]).length() > HIDE_RADIUS:
                continue
            # 归属：离哪条样条更近就属于谁
            qa = minor_sp.find_location_closest_to_world_location(p, WS)
            qb = major_sp.find_location_closest_to_world_location(p, WS)
            if (qa - p).length() < (qb - p).length():
                try:
                    if sm.is_visible():
                        sm.set_visibility(False, False)
                        hidden += 1
                except Exception:
                    pass

    w("已隐藏次路路面段 %d 个" % hidden)
    w("隐藏半径 %.0f cm。太多/太少就改 HIDE_RADIUS 重跑。" % HIDE_RADIUS)
    w("要全部恢复：把 RESTORE 改成 True 再跑一次。")
    w("注意：只隐藏了显示，碰撞仍在——射线和车辆检测不受影响。")
    w("关卡尚未保存。")

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[fixinter] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
