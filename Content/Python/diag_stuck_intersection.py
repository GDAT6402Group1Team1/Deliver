# -*- coding: utf-8 -*-
"""查"车卡在某个路口之前"的原因。只读，默认查 MainRoad_R010 的 I08 路口。

为什么从"路口盒罩不罩得住"查起
------------------------------
车停在路口**之前**不走，最像的是等一个永远不变绿的红灯：
`BP_car_base` 刹停后起 `PollForGreen`（0.3 秒轮询）盯着
`StoppedAtIntersection` 的 `StopAtInter`，变 false 才重新武装加速。
如果那段 `Inter_` 没有被任何 `BP_Intersection` 的盒子罩住，
就没人去翻它的 `StopAtInter`，车会原地等到关卡结束。
这个失败模式在本项目造成过两次"红绿灯没了"（车道一重新生成，
`Inter_` 的 Box 全挪位，旧盒子罩不住新位置），而且表现必然是**局部的**
——只有那个路口卡，其他路口正常，和现在的症状一致。

顺带查的其他几条，都是已知会让车停在路口前的原因：
  · 同一个物理路口有**两个** BP_Intersection：两套独立定时器抢着写同一批子节点，
    可能长期按住红灯。
  · 两个相位的 `LightNumber` 分组不对（全是同一组 → 永远只有一组能走，
    另一组恒红）。
  · 端点切线为零（`CURVE_CLAMPED` 把端点切线归零）→ `GetRotationAtDistanceAlongSpline`
    内部 `MakeFromXZ` 退化、前向掉回世界 +X，车的路口探测甩向别处、扫不到路口盒。
  · 进不来 / 出不去（上游 `Lane_` 终点离本段起点太远、或本段终点没有下家）。
  · 转弯样条是蓝图默认的 100cm 残桩。

只读：不改任何属性、不建不删 actor。结果写到 Saved/diag_stuck_intersection.txt
用法：py diag_stuck_intersection.py
"""

import math
import traceback

import unreal

# 要查的路口段。按标签子串匹配，留空 TARGET_LABELS 则查 TARGET_SUBSTR 命中的全部。
TARGET_LABELS = [
    "Inter_MainRoad_R010_-0540_I08",
    "Inter_MainRoad_R010_+0540_I08",
]
TARGET_SUBSTR = "I08"          # 找同一路口的兄弟段用
INTERSECTION_PREFIX = "BP_Intersection"
CHILD_PREFIX = "Inter_"
LANE_PREFIX = "Lane_"

CLUSTER_DIST = 2500.0          # 和 gen_intersections.py 保持一致：这么近算同一个路口
LINK_DIST = 600.0              # 和 fix_t_junctions.py 一致：接得上的距离阈值
LINK_DOT = 0.5                 # 同向判据，防止把对向车道当成合法下家
ZERO_TANGENT = 1.0             # 切线长小于此值视为被钳成 0
STUB_LEN = 300.0               # 短于此且点数 <=3 视为蓝图默认残桩

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "diag_stuck_intersection.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[stuck] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def prop(a, name):
    """读蓝图变量。**先试原名**——蓝图变量要用原名，snake_case 只适用于原生
    UPROPERTY，按 snake_case 猜会全部读成"读不到"。"""
    for n in (name, name.lower()):
        try:
            return a.get_editor_property(n)
        except Exception:
            continue
    return u"<读不到>"


def box_aabb(a):
    """actor 上 Box 的世界 AABB：(中心, 逐轴半长)。没有 Box 返回 None。

    Box 随行驶方向转过、不是轴对齐，所以用 |前|*hx+|右|*hy+|上|*hz 逐轴外接
    （保守偏大）。用 actor 原点判会出错：原点在盒内而盒体探出去、
    或原点在盒外而盒体已经压上来，两种都遇到过。
    """
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
        return center, half
    return None


def contains(outer, inner):
    """outer 的 AABB 是否完整罩住 inner。返回 (是否罩住, 各轴缺多少 cm)。"""
    (oc, oh), (ic, ih) = outer, inner
    gaps = []
    for axis in ("x", "y", "z"):
        o_lo, o_hi = getattr(oc, axis) - getattr(oh, axis), getattr(oc, axis) + getattr(oh, axis)
        i_lo, i_hi = getattr(ic, axis) - getattr(ih, axis), getattr(ic, axis) + getattr(ih, axis)
        gaps.append(max(0.0, o_lo - i_lo) + max(0.0, i_hi - o_hi))
    return (sum(gaps) <= 0.0), gaps


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
    for key in ("Spline", "SplineComponent"):
        if key in sp:
            return sp[key]
    # 名字对不上就退回"不是左右转的那条"，再退回第一条
    for name, c in sp.items():
        if "Left" not in name and "Right" not in name:
            return c
    return next(iter(sp.values())) if sp else None


def spline_dir(sp, at_start):
    """用两个采样点作差求方向。

    不用 get_direction_at_distance_along_spline：它**在端点**返回零向量
    （端点切线被 CURVE_CLAMPED 归零），而我们恰好要端点的方向。
    """
    n = sp.get_number_of_spline_points()
    if n < 2:
        return None
    i, j = (0, 1) if at_start else (n - 1, n - 2)
    p, q = sp.get_location_at_spline_point(i, WS), sp.get_location_at_spline_point(j, WS)
    d = unreal.Vector(q.x - p.x, q.y - p.y, 0.0) if at_start else \
        unreal.Vector(p.x - q.x, p.y - q.y, 0.0)
    L = math.sqrt(d.x ** 2 + d.y ** 2)
    return None if L < 1e-3 else unreal.Vector(d.x / L, d.y / L, 0.0)


def tangent_len(sp, idx):
    t = sp.get_tangent_at_spline_point(idx, WS)
    return math.sqrt(t.x ** 2 + t.y ** 2 + t.z ** 2)


def dist2d(p, q):
    return math.sqrt((p.x - q.x) ** 2 + (p.y - q.y) ** 2)


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()
    w(u"=== 路口卡车诊断（只读）===")
    w(u"关卡：%s     actor 总数 %d" % (world.get_name() if world else u"?", len(actors)))

    inters, children, lanes = [], [], []
    for a in actors:
        cls = a.get_class().get_name()
        label = a.get_actor_label()
        if cls.startswith(INTERSECTION_PREFIX) and "Child" not in cls:
            inters.append(a)
        elif label.startswith(CHILD_PREFIX):
            children.append(a)
        elif label.startswith(LANE_PREFIX):
            lanes.append(a)
    w(u"路口 actor %d 个、Inter_ 段 %d 个、Lane_ 段 %d 个"
      % (len(inters), len(children), len(lanes)))

    targets = [a for a in children if a.get_actor_label() in TARGET_LABELS]
    if not targets:
        w(u"")
        w(u"！按完整标签没找到目标。含「%s」的 Inter_ 段有：" % TARGET_SUBSTR)
        for a in children:
            if TARGET_SUBSTR in a.get_actor_label():
                w(u"    %s" % a.get_actor_label())
        targets = [a for a in children if TARGET_SUBSTR in a.get_actor_label()]
        if not targets:
            w(u"  一个都没有——标签变了？把 TARGET_SUBSTR 改成实际的名字片段再跑。")
            return

    # ---- 1) 这个路口的全部兄弟段 ----
    anchor = targets[0].get_actor_location()
    siblings = [a for a in children
                if dist2d(a.get_actor_location(), anchor) < CLUSTER_DIST]
    w(u"")
    w(u"=" * 72)
    w(u"① 这个物理路口的全部 Inter_ 段（%.0f cm 以内共 %d 段）" % (CLUSTER_DIST, len(siblings)))
    w(u"=" * 72)

    near_inters = [(dist2d(i.get_actor_location(), anchor), i) for i in inters
                   if dist2d(i.get_actor_location(), anchor) < CLUSTER_DIST * 2]
    near_inters.sort(key=lambda t: t[0])
    w(u"附近的 %s actor %d 个：" % (INTERSECTION_PREFIX, len(near_inters)))
    for d, i in near_inters:
        ib = box_aabb(i)
        w(u"    %-34s 距锚点 %6.0f  LightDuration=%s  StopAtInter=%s  盒%s"
          % (i.get_actor_label(), d, prop(i, "LightDuration"),
             prop(i, "StopAtInter"),
             u"无！" if ib is None else u"半长(%.0f,%.0f,%.0f)" % (ib[1].x, ib[1].y, ib[1].z)))
    if len(near_inters) > 1:
        w(u"  ！附近有多于一个路口 actor——两套独立定时器可能抢着写同一批子节点，")
        w(u"    表现就是某一组长期红灯。这条要和下面的覆盖结果一起看。")

    w(u"")
    w(u"%-34s %-6s %-9s %-22s %s"
      % (u"段", u"灯组", u"StopAtInter", u"被哪个路口盒罩住", u"备注"))
    groups = {}
    for a in sorted(siblings, key=lambda x: x.get_actor_label()):
        label = a.get_actor_label()
        ln = prop(a, "LightNumber")
        stop = prop(a, "StopAtInter")
        bb = box_aabb(a)
        groups.setdefault(str(ln), []).append(label)

        cover, note = u"—", u""
        if bb is None:
            note = u"这段没有 Box！车探不到它"
        else:
            full, partial = [], []
            for _d, i in near_inters:
                ib = box_aabb(i)
                if ib is None:
                    continue
                ok, gaps = contains(ib, bb)
                if ok:
                    full.append(i.get_actor_label())
                elif overlaps(ib, bb):
                    partial.append((i.get_actor_label(), gaps))
            if full:
                cover = full[0] if len(full) == 1 else u"%d 个" % len(full)
            elif partial:
                nm, gaps = partial[0]
                cover = u"只压住一部分"
                note = u"%s 差 X%.0f/Y%.0f/Z%.0f cm" % (nm, gaps[0], gaps[1], gaps[2])
            else:
                cover = u"**没有**"
                note = u"没人翻它的 StopAtInter → 车会在这段前面等到底"
        mark = u" <<< 目标" if label in TARGET_LABELS else u""
        w(u"%-34s %-6s %-9s %-22s %s%s" % (label, ln, stop, cover, note, mark))

    w(u"")
    w(u"灯组分布：%s" % u"  ".join(u"组%s=%d段" % (k, len(v)) for k, v in sorted(groups.items())))
    if len(groups) < 2:
        w(u"  ！只有一个灯组——两个相位里有一个永远不会放行，")
        w(u"    另一条路的车会一直等。正常应该是两组交替（gen_traffic_lanes.py 的")
        w(u"    LIGHT_NUMBER_BY_AXIS 按走向给 0/1）。")

    # ---- 2) 目标段自己的细节 ----
    for a in targets:
        label = a.get_actor_label()
        w(u"")
        w(u"=" * 72)
        w(u"② %s" % label)
        w(u"=" * 72)
        loc = a.get_actor_location()
        w(u"位置 (%.0f, %.0f, %.0f)   灯组 %s   StopAtInter %s"
          % (loc.x, loc.y, loc.z, prop(a, "LightNumber"), prop(a, "StopAtInter")))

        sp = main_spline(a)
        if sp is None:
            w(u"！没有样条组件——车跟不了这段。")
            continue
        n = sp.get_number_of_spline_points()
        p0 = sp.get_location_at_spline_point(0, WS)
        p1 = sp.get_location_at_spline_point(n - 1, WS)
        w(u"主样条 %d 点，长 %.0f cm，起点 (%.0f,%.0f) 终点 (%.0f,%.0f)"
          % (n, sp.get_spline_length(), p0.x, p0.y, p1.x, p1.y))
        t0, t1 = tangent_len(sp, 0), tangent_len(sp, n - 1)
        if t0 < ZERO_TANGENT or t1 < ZERO_TANGENT:
            w(u"！端点切线被钳成 0（首 %.2f / 末 %.2f）。" % (t0, t1))
            w(u"  后果：GetRotationAtDistanceAlongSpline 内部 MakeFromXZ 退化、")
            w(u"  前向掉回世界 +X，车的路口探测会甩向别处、扫不到路口盒。")
            w(u"  修法：首尾两点改成 CURVE（跑 fix_endpoint_tangents.py），中间点保持 CLAMPED。")
        else:
            w(u"端点切线正常（首 %.0f / 末 %.0f）" % (t0, t1))

        # 转弯样条
        for nm, c in sorted(splines(a).items()):
            if "Left" not in nm and "Right" not in nm:
                continue
            cn = c.get_number_of_spline_points()
            cl = c.get_spline_length()
            kind = u"残桩（蓝图默认，车选到它就开进虚空）" if (cn <= 3 and cl < STUB_LEN) \
                else (u"零长" if cl < 1.0 else u"有曲线")
            w(u"  %-16s %d 点 长 %.0f cm — %s" % (nm, cn, cl, kind))

        # 进得来 / 出得去
        d_in = spline_dir(sp, True)
        d_out = spline_dir(sp, False)
        feeders, exits = [], []
        for ln_actor in lanes:
            lsp = main_spline(ln_actor)
            if lsp is None or lsp.get_number_of_spline_points() < 2:
                continue
            ln_n = lsp.get_number_of_spline_points()
            ls = lsp.get_location_at_spline_point(0, WS)
            le = lsp.get_location_at_spline_point(ln_n - 1, WS)
            if d_in is not None and dist2d(le, p0) < LINK_DIST:
                ld = spline_dir(lsp, False)
                dot = (ld.x * d_in.x + ld.y * d_in.y) if ld else -1.0
                feeders.append((dist2d(le, p0), dot, ln_actor.get_actor_label()))
            if d_out is not None and dist2d(ls, p1) < LINK_DIST:
                ld = spline_dir(lsp, True)
                dot = (ld.x * d_out.x + ld.y * d_out.y) if ld else -1.0
                exits.append((dist2d(ls, p1), dot, ln_actor.get_actor_label()))

        for title, rows in ((u"进得来（上游车道终点落在本段起点附近）", feeders),
                            (u"出得去（下游车道起点落在本段终点附近）", exits)):
            good = [r for r in rows if r[1] > LINK_DOT]
            w(u"  %s：%d 条，其中同向 %d 条" % (title, len(rows), len(good)))
            for d, dot, nm in sorted(rows)[:4]:
                w(u"      %-34s 间距 %5.0f cm  同向性 %+.2f%s"
                  % (nm, d, dot, u"" if dot > LINK_DOT else u"  ← 逆向，不算下家"))
            if not good:
                w(u"      ！一条同向的都没有。")

    w(u"")
    w(u"=" * 72)
    w(u"怎么读这份报告")
    w(u"=" * 72)
    w(u"  · 目标段的「被哪个路口盒罩住」= **没有** → 根因就是它。")
    w(u"    修法：跑 py gen_intersections.py 重算路口盒（车道重新生成过之后必须跑它），")
    w(u"    跑完 Ctrl+S。不用重跑车道。")
    w(u"  · 罩住了、StopAtInter 恒为 true → 去看那个路口 actor 的灯循环，")
    w(u"    以及附近是不是有两个路口 actor 在抢。")
    w(u"  · 罩住了、灯也在变，但只有一个灯组 → 另一相位永远不放行。")
    w(u"  · 以上都正常 → 是「进不来/出不去」或端点切线，看上面第 ② 段。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[stuck] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[stuck] 写入 %s" % OUT)
