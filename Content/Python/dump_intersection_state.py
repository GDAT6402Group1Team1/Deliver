# -*- coding: utf-8 -*-
"""完整 dump 指定路口的当前状态，并和脚本本来会分配的灯组并排对比。

用途：人在编辑器里手改过某个路口之后，把"实际是什么"和"脚本会怎么做"
并排打出来，差异就是手改内容——这样才能把它编码进脚本，而不是靠口头记。

**按坐标认路口，名字只作参考。** `gen_intersections.py` 每次按簇顺序重新
编号（`Intersection_%02d`），名字会变；坐标不会。

报告内容：
  · 盒子：中心、缩放、实际半尺寸（半尺寸 = 默认 extent × 缩放）
  · 罩住的段：路名、轴角、当前 LightNumber
  · 位置聚类：段分几团、团心最远距离（判"盒子是不是跨了两个路口"）
  · **脚本本来会分的组**（先按路、再按轴角，和 fix_light_number_by_axis 同一套）
    和当前值逐段对比，不一致的点名

只读。结果写到 Saved/dump_intersection_state.txt
用法：py dump_intersection_state.py
"""

import math
import re
import traceback

import unreal

# 按坐标找（半径内最近的那个路口）。名字只用来显示。
TARGETS = [
    (-3422.0, -19387.0, u"原 Intersection_35：三轴 + 8段2团"),
    (25907.0, -11995.0, u"原 Intersection_33：6段2团 + 一处出不去"),
]
SEARCH_RADIUS = 4000.0
SAME_AXIS_TOL = 30.0
CLUSTER_DIST = 2000.0     # 和 diag_junction_span.py 一致
INTER_PREFIX = "BP_Intersection"
LABEL_RE = re.compile(r'^Inter_(.+)_([+-]\d+)_I(\d+)$')

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "dump_intersection_state.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[dumpinter] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def prop(a, name):
    for n in (name, name.lower()):
        try:
            return a.get_editor_property(n)
        except Exception:
            continue
    return None


def box_parts(a):
    """返回 (中心, 半尺寸, 未缩放extent, 缩放)。没有 Box 返回 None。"""
    for c in a.get_components_by_class(unreal.BoxComponent):
        e = c.get_unscaled_box_extent()
        sc = a.get_actor_scale3d()
        hx, hy, hz = e.x * sc.x, e.y * sc.y, e.z * sc.z
        try:
            center, rot = c.get_world_location(), c.get_world_rotation()
        except Exception:
            center, rot = a.get_actor_location(), a.get_actor_rotation()
        f = unreal.MathLibrary.get_forward_vector(rot)
        r = unreal.MathLibrary.get_right_vector(rot)
        u = unreal.MathLibrary.get_up_vector(rot)
        half = unreal.Vector(
            abs(f.x) * hx + abs(r.x) * hy + abs(u.x) * hz,
            abs(f.y) * hx + abs(r.y) * hy + abs(u.y) * hz,
            abs(f.z) * hx + abs(r.z) * hy + abs(u.z) * hz)
        return center, half, e, sc
    return None


def overlaps(a_box, b_box):
    (ac, ah), (bc, bh) = a_box, b_box
    for axis in ("x", "y", "z"):
        if abs(getattr(ac, axis) - getattr(bc, axis)) > getattr(ah, axis) + getattr(bh, axis):
            return False
    return True


def main_spline(a):
    best = None
    for c in a.get_components_by_class(unreal.SplineComponent):
        nm = c.get_name()
        if "Left" in nm or "Right" in nm:
            continue
        if best is None or nm in ("Spline", "SplineComponent"):
            best = c
    return best


def axis_angle(sp):
    n = sp.get_number_of_spline_points()
    if n < 2:
        return None
    p = sp.get_location_at_spline_point(0, WS)
    q = sp.get_location_at_spline_point(n - 1, WS)
    dx, dy = q.x - p.x, q.y - p.y
    if abs(dx) < 1e-6 and abs(dy) < 1e-6:
        return None
    return math.degrees(math.atan2(dy, dx)) % 180.0


def axis_diff(a, b):
    d = abs(a - b) % 180.0
    return min(d, 180.0 - d)


def road_of(label):
    m = LABEL_RE.match(str(label))
    return m.group(1) if m else str(label)


def d2(p, q):
    return math.sqrt((p.x - q.x) ** 2 + (p.y - q.y) ** 2)


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    w(u"=== 路口状态 dump（只读）===")
    w(u"关卡：%s" % (world.get_name() if world else u"?"))

    inters, segs = [], []
    for a in eas.get_all_level_actors():
        cls = a.get_class().get_name()
        lbl = a.get_actor_label()
        if cls.startswith(INTER_PREFIX) and "Child" not in cls:
            bp = box_parts(a)
            if bp:
                inters.append((a, bp))
        elif lbl.startswith("Inter_"):
            sp = main_spline(a)
            bb = box_parts(a)
            if sp is None or bb is None:
                continue
            segs.append({"a": a, "label": lbl, "sp": sp,
                         "box": (bb[0], bb[1]), "loc": a.get_actor_location(),
                         "ang": axis_angle(sp), "road": road_of(lbl),
                         "ln": prop(a, "LightNumber")})
    w(u"路口 %d 个、Inter_ 段 %d 个" % (len(inters), len(segs)))

    for tx, ty, note in TARGETS:
        anchor = unreal.Vector(tx, ty, 0.0)
        best, bd = None, 1e18
        for a, bp in inters:
            d = d2(a.get_actor_location(), anchor)
            if d < bd:
                best, bd = (a, bp), d
        w(u"")
        w(u"=" * 76)
        w(u"目标 (%.0f, %.0f) —— %s" % (tx, ty, note))
        w(u"=" * 76)
        if best is None or bd > SEARCH_RADIUS:
            w(u"  ！%.0f cm 内没有路口（最近的在 %.0f cm 外）。"
              % (SEARCH_RADIUS, bd))
            w(u"    这个路口可能已经被删掉了——如果是你手动删的，那就是预期。")
            continue
        a, (center, half, ext, sc) = best
        w(u"  当前名字：%s   距锚点 %.0f cm" % (a.get_actor_label(), bd))
        w(u"  盒子中心 (%.0f, %.0f, %.0f)" % (center.x, center.y, center.z))
        w(u"  缩放 (%.3f, %.3f, %.3f)   未缩放 extent (%.0f, %.0f, %.0f)"
          % (sc.x, sc.y, sc.z, ext.x, ext.y, ext.z))
        w(u"  实际半尺寸 (%.0f, %.0f, %.0f)  ← 盒子罩多大就看这个"
          % (half.x, half.y, half.z))
        tags = [str(t) for t in a.tags]
        w(u"  tags = %s%s" % (u",".join(tags) or u"（无）",
                              u"" if tags else u"   ← 没有 tag！gen_intersections 不会删它、"
                                               u"重建时会在旁边再加一个"))
        w(u"  LightDuration = %s" % prop(a, "LightDuration"))

        mine = [s for s in segs if overlaps((center, half), s["box"])]
        w(u"")
        w(u"  罩住 %d 段：" % len(mine))
        w(u"  %-40s %-22s %-7s %s" % (u"段", u"路", u"轴角", u"当前灯组"))
        for s in sorted(mine, key=lambda x: x["label"]):
            w(u"  %-40s %-22s %-7s %s"
              % (s["label"][:40], s["road"][:22],
                 u"?" if s["ang"] is None else u"%.0f°" % s["ang"], s["ln"]))

        # 位置聚类：盒子是不是跨了两个路口
        clusters = []
        for s in mine:
            hit = None
            for c in clusters:
                if any(d2(s["loc"], o["loc"]) < CLUSTER_DIST for o in c):
                    hit = c
                    break
            (hit if hit is not None else clusters.append([]) or clusters[-1]).append(s)
        worst = 0.0
        for i in range(len(clusters)):
            for j in range(i + 1, len(clusters)):
                for p in clusters[i]:
                    for q in clusters[j]:
                        worst = max(worst, d2(p["loc"], q["loc"]))
        w(u"")
        w(u"  位置聚类：%d 团%s"
          % (len(clusters),
             u"，团间最远 %.0f cm  ← 盒子跨了两个路口" % worst if len(clusters) > 1 else u""))

        # 脚本本来会怎么分：先按路、再按轴角
        by_road = {}
        for s in mine:
            if s["ang"] is not None:
                by_road.setdefault(s["road"], []).append(s)
        axes = []
        for rd, members in sorted(by_road.items()):
            ang = sum(x["ang"] for x in members) / len(members)
            hit = None
            for ax in axes:
                if axis_diff(ax["ang"], ang) < SAME_AXIS_TOL:
                    hit = ax
                    break
            if hit is None:
                axes.append({"ang": ang, "segs": list(members), "roads": [rd]})
            else:
                hit["segs"].extend(members)
                hit["roads"].append(rd)
                hit["ang"] = sum(x["ang"] for x in hit["segs"]) / len(hit["segs"])
        axes.sort(key=lambda ax: -len(ax["segs"]))
        w(u"")
        w(u"  脚本会分成 %d 个轴向组（先按路、再按轴角，容差 %.0f°）："
          % (len(axes), SAME_AXIS_TOL))
        mismatch = []
        for gi, ax in enumerate(axes):
            w(u"      组%d  %.0f°  %d 段  路：%s"
              % (gi, ax["ang"], len(ax["segs"]), u"+".join(ax["roads"])))
            vals = set(str(x["ln"]) for x in ax["segs"])
            if len(vals) > 1:
                mismatch.append(u"组%d 内部灯组不一致：%s" % (gi, u"/".join(sorted(vals))))
        seen = {}
        for gi, ax in enumerate(axes):
            for x in ax["segs"]:
                seen.setdefault(str(x["ln"]), set()).add(gi)
        for ln, gset in seen.items():
            if len(gset) > 1:
                mismatch.append(u"灯组 %s 横跨 %d 个轴向组（交叉方向同时放行）"
                                % (ln, len(gset)))
        w(u"")
        if mismatch:
            w(u"  **当前值和脚本判据不一致**（这些差异就是手改内容）：")
            for m in mismatch:
                w(u"      · %s" % m)
            w(u"  若这是刻意的，要把它编码进脚本才不会被下次重跑冲掉。")
        else:
            w(u"  当前灯组和脚本判据一致，没有手改痕迹。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[dumpinter] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[dumpinter] 写入 %s" % OUT)
