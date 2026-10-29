# -*- coding: utf-8 -*-
"""查指定路口的段「进得来/ 出得去」和三条样条的状态——定位车为什么在那里裸奔。

裸奔的机制：车沿样条开到最后一点，靠 Overlap 扫到下一段的 Box 才能接力。
下一段不存在（或者朝向反了、不算同向），车就脱离样条按最后朝向直行，
满速 3 秒后被 `RouteLostTimeout` 缩小消失、瞬移回出生点，再来一次。
**表现是"这辆车永远走不出这个路口"。**

所以每段要查三件事：
  1. 主样条终点附近有没有**同向**的下家（`Lane_` 或 `Inter_` 的起点）
  2. 主样条起点附近有没有同向的上家（没有则这段根本没车进来，是条死段）
  3. `SplineLeft`/`SplineRight` 各是什么：转弯曲线 / 直行副本 / 蓝图默认残桩 /
     零长。**残桩最危险**——车探到一个 Box 就拿到三条候选、随机选一条，
     选中 100cm 的残桩就等于直接开进虚空。

同向判据 dot > 0.5：只看位置的话，**对向车道的起点也在附近**（实测 360cm），
会把逆行当成合法下家（`fix_t_junctions.py` 当初就漏过两条）。

只读。结果写到 Saved/diag_intersection_exits.txt
用法：py diag_intersection_exits.py
"""

import math
import traceback

import unreal

TARGETS = ["Intersection_18",
           "Intersection_33", "Intersection_38", "Intersection_39",
           "Intersection_40", "Intersection_41"]
INTER_PREFIX = "BP_Intersection"
LINK_DIST = 600.0        # 和 fix_t_junctions.py 一致
LINK_DOT = 0.5
STUB_LEN = 300.0         # 点数 <=3 且短于此 = 蓝图默认残桩
SAME_SPLINE_TOL = 50.0   # 和主样条长度差在此以内、点数相同 = 直行副本

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "diag_intersection_exits.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[exits] %s" % s)


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


def box_aabb(a):
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
        return center, unreal.Vector(
            abs(f.x) * hx + abs(r.x) * hy + abs(u.x) * hz,
            abs(f.y) * hx + abs(r.y) * hy + abs(u.y) * hz,
            abs(f.z) * hx + abs(r.z) * hy + abs(u.z) * hz)
    return None


def overlaps(a_box, b_box):
    (ac, ah), (bc, bh) = a_box, b_box
    for axis in ("x", "y", "z"):
        if abs(getattr(ac, axis) - getattr(bc, axis)) > getattr(ah, axis) + getattr(bh, axis):
            return False
    return True


def splines(a):
    out = {}
    for c in a.get_components_by_class(unreal.SplineComponent):
        out[c.get_name()] = c
    return out


def main_spline(a):
    sp = splines(a)
    for k in ("Spline", "SplineComponent"):
        if k in sp:
            return sp[k]
    for n, c in sp.items():
        if "Left" not in n and "Right" not in n:
            return c
    return next(iter(sp.values())) if sp else None


def ends(sp):
    n = sp.get_number_of_spline_points()
    return (sp.get_location_at_spline_point(0, WS),
            sp.get_location_at_spline_point(n - 1, WS))


def dir_at(sp, at_start):
    """用两个采样点作差。端点切线可能被 CURVE_CLAMPED 归零，不能用 get_direction。"""
    n = sp.get_number_of_spline_points()
    if n < 2:
        return None
    i, j = (0, 1) if at_start else (n - 1, n - 2)
    p, q = sp.get_location_at_spline_point(i, WS), sp.get_location_at_spline_point(j, WS)
    dx, dy = (q.x - p.x, q.y - p.y) if at_start else (p.x - q.x, p.y - q.y)
    L = math.sqrt(dx * dx + dy * dy)
    return None if L < 1e-3 else (dx / L, dy / L)


def d2(p, q):
    return math.sqrt((p.x - q.x) ** 2 + (p.y - q.y) ** 2)


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    w(u"=== 路口出入口与转弯状态（只读）===")
    w(u"关卡：%s" % (world.get_name() if world else u"?"))

    inters, pieces = {}, []
    for a in eas.get_all_level_actors():
        cls = a.get_class().get_name()
        lbl = a.get_actor_label()
        if cls.startswith(INTER_PREFIX) and "Child" not in cls:
            if lbl in TARGETS:
                inters[lbl] = a
        elif lbl.startswith("Lane_") or lbl.startswith("Inter_"):
            sp = main_spline(a)
            if sp and sp.get_number_of_spline_points() >= 2:
                s, e = ends(sp)
                pieces.append({"a": a, "label": lbl, "sp": sp, "s": s, "e": e})
    w(u"目标路口找到 %d/%d 个；全图可用段 %d 个"
      % (len(inters), len(TARGETS), len(pieces)))
    missing = [t for t in TARGETS if t not in inters]
    if missing:
        w(u"！没找到：%s（名字变了？或者已被重建成别的序号）" % u"、".join(missing))

    for name in TARGETS:
        inter = inters.get(name)
        if inter is None:
            continue
        bb = box_aabb(inter)
        w(u"")
        w(u"=" * 74)
        w(u"%s @ (%.0f, %.0f)" % (name, inter.get_actor_location().x,
                                  inter.get_actor_location().y))
        w(u"=" * 74)
        if bb is None:
            w(u"  ！没有 Box")
            continue
        mine = [p for p in pieces
                if p["label"].startswith("Inter_") and overlaps(bb, box_aabb(p["a"]) or (p["s"], unreal.Vector(1, 1, 1)))]
        w(u"  盒子罩住 %d 段" % len(mine))

        for p in sorted(mine, key=lambda x: x["label"]):
            sp = p["sp"]
            n = sp.get_number_of_spline_points()
            main_len = sp.get_spline_length()
            din, dout = dir_at(sp, True), dir_at(sp, False)

            ins, outs = [], []
            for q in pieces:
                if q is p:
                    continue
                if din and d2(q["e"], p["s"]) < LINK_DIST:
                    qd = dir_at(q["sp"], False)
                    dot = (qd[0] * din[0] + qd[1] * din[1]) if qd else -1.0
                    ins.append((dot, q["label"], d2(q["e"], p["s"])))
                if dout and d2(q["s"], p["e"]) < LINK_DIST:
                    qd = dir_at(q["sp"], True)
                    dot = (qd[0] * dout[0] + qd[1] * dout[1]) if qd else -1.0
                    outs.append((dot, q["label"], d2(q["s"], p["e"])))
            good_in = [x for x in ins if x[0] > LINK_DOT]
            good_out = [x for x in outs if x[0] > LINK_DOT]

            flag = u""
            if not good_out:
                flag = u"   ←←← **出不去，车开到这里就裸奔**"
            elif not good_in:
                flag = u"   ← 进不来（死段，没车会走到它）"
            w(u"")
            w(u"  %s  灯组 %s  主样条 %d 点 / %.0f cm%s"
              % (p["label"], prop(p["a"], "LightNumber"), n, main_len, flag))
            w(u"      进得来：%d 条（同向 %d）    出得去：%d 条（同向 %d）"
              % (len(ins), len(good_in), len(outs), len(good_out)))
            for dot, lbl, dist in sorted(good_out, reverse=True)[:3]:
                w(u"        下家 %-40s 间距 %4.0f 同向 %+.2f" % (lbl[:40], dist, dot))
            for dot, lbl, dist in sorted(outs, reverse=True)[:3]:
                if dot <= LINK_DOT:
                    w(u"        （逆向，不算）%-30s 间距 %4.0f 同向 %+.2f"
                      % (lbl[:30], dist, dot))

            for cn, c in sorted(splines(p["a"]).items()):
                if "Left" not in cn and "Right" not in cn:
                    continue
                k = c.get_number_of_spline_points()
                L = c.get_spline_length()
                if k <= 3 and L < STUB_LEN:
                    kind = u"**蓝图默认残桩** ← 车选到它就开进虚空"
                elif L < 1.0:
                    kind = u"零长"
                elif k == n and abs(L - main_len) < SAME_SPLINE_TOL:
                    kind = u"直行副本（选中=直行，安全）"
                else:
                    kind = u"转弯曲线"
                    ce = ends(c)[1]
                    has = any(d2(q["s"], ce) < LINK_DIST for q in pieces if q is not p)
                    if not has:
                        kind += u"  ←←← **终点没有下家，选到它就裸奔**"
                w(u"      %-14s %d 点 / %5.0f cm — %s" % (cn, k, L, kind))

    w(u"")
    w(u"=" * 74)
    w(u"怎么读")
    w(u"=" * 74)
    w(u"  · 「出不去」或「转弯终点没有下家」就是裸奔的直接原因。")
    w(u"  · 残桩是最隐蔽的一种：三条候选里只有一条坏，所以**只有部分车**裸奔，")
    w(u"    看着像随机。跑 fill_turn_splines.py 能把残桩写成直行副本或曲线。")
    w(u"  · 要把某个路口简化成「全直行」，前提是**每段的主样条都出得去**；")
    w(u"    主样条出不去的段（T 形进入段）必须保留它那条转弯，否则直行就是虚空。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[exits] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[exits] 写入 %s" % OUT)
