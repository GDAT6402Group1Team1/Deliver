# -*- coding: utf-8 -*-
"""全图体检：还有哪些地方会让车裸奔或卡死。只读，不用跑 PIE。

两类症状的成因**大部分是静态的**，所以不必开 Simulate 就能查：

【裸奔】车开到样条尽头、没有同向下家可接 → 脱离样条按最后朝向直行，
满速 `RouteLostTimeout`(3 秒) 后缩小消失、瞬移回出生点，再来一次。
  A1 主样条终点没有同向下家
  A2 转弯样条（SplineLeft/Right）终点没有同向下家——**只有选到那条的车才裸奔**，
     所以表现是"偶尔有车跑飞"，比 A1 难查
  A3 蓝图默认残桩（点数 ≤3 且 <300cm）——等于一条通往虚空的 100cm 小棍子

【卡死】车停住再也不走。
  B1 `Inter_` 段**没有被任何路口盒罩住**，而 `StopAtInter` 是 true
     → 没有 BP_Intersection 去刷它（蓝图靠 GetOverlappingActors 找子节点），
     车读到 true 就 StopCar，`PollForGreen` 永远等不到绿灯。**最致命的一种。**
  B2 一段被 **2 个以上**盒子罩住 → 两套定时器抢着写同一段的灯态
  B3 单组路口（`Length == 1`）→ 蓝图里那道 `Length > 1` 闸生效时恒绿；
     列出来是为了确认闸真的在（闸没了就是恒红）
  B4 段被罩住但所属路口**没有 LightDuration** → 定时器周期为 0

同向判据 dot > 0.5：只看位置的话对向车道的起点也在附近（实测 360cm），
会把逆行当成合法下家。

只读。结果写到 Saved/audit_traffic_health.txt
用法：py audit_traffic_health.py
"""

import math
import traceback

import unreal

LINK_DIST = 600.0
LINK_DOT = 0.5
STUB_LEN = 300.0
INTER_PREFIX = "BP_Intersection"
MAX_LIST = 9999        # 不截断：用户要拿完整清单自己去编辑器里查

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "audit_traffic_health.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[health] %s" % s)


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


def all_splines(a):
    return {c.get_name(): c for c in a.get_components_by_class(unreal.SplineComponent)}


def main_of(d):
    for k in ("Spline", "SplineComponent"):
        if k in d:
            return d[k]
    for n, c in d.items():
        if "Left" not in n and "Right" not in n:
            return c
    return next(iter(d.values())) if d else None


def ends(sp):
    n = sp.get_number_of_spline_points()
    return (sp.get_location_at_spline_point(0, WS),
            sp.get_location_at_spline_point(n - 1, WS))


def dir_at(sp, at_start):
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
    w(u"=== 交通线全图体检（只读，不用跑 PIE）===")
    w(u"关卡：%s" % (world.get_name() if world else u"?"))

    inters, pieces = [], []
    for a in eas.get_all_level_actors():
        cls = a.get_class().get_name()
        lbl = a.get_actor_label()
        if cls.startswith(INTER_PREFIX) and "Child" not in cls:
            bb = box_aabb(a)
            if bb:
                inters.append((a, bb))
        elif lbl.startswith("Lane_") or lbl.startswith("Inter_"):
            sd = all_splines(a)
            m = main_of(sd)
            if m is None or m.get_number_of_spline_points() < 2:
                continue
            s, e = ends(m)
            pieces.append({"a": a, "label": lbl, "sp": m, "sd": sd,
                           "s": s, "e": e, "box": box_aabb(a),
                           "is_inter": lbl.startswith("Inter_")})
    w(u"路口盒 %d 个、交通段 %d 个" % (len(inters), len(pieces)))
    if not pieces:
        w(u"！没有交通段。")
        return

    def has_downstream(point, direction):
        """point 附近有没有同向的下家起点。"""
        if direction is None:
            return False
        for q in pieces:
            if d2(q["s"], point) >= LINK_DIST:
                continue
            qd = dir_at(q["sp"], True)
            if qd and (qd[0] * direction[0] + qd[1] * direction[1]) > LINK_DOT:
                return True
        return False

    import re as _re
    _SEG = _re.compile(r'^(Lane|Inter)_(.+)_([+-])(\d+)_([SI])(\d+)$')

    def seg_key(label):
        m = _SEG.match(str(label))
        return None if m is None else (m.group(2), m.group(3), m.group(6))

    def anything_near(point, exclude):
        """point 附近有没有**任何**段的起点（不看方向）。返回 (标签, 是不是对向同段)。

        「对向同段」= 同一条路、同一段号、相反偏移。**这是断头路的正常几何**：
        双向路到尽头时，出向车道的终点正好挨着入向车道的起点（横向只差 360cm，
        在 600 的判定半径内）。第一版把它当成"接错了"，于是 31 处"真问题"里
        大半是正常的路到头——数字没法用。
        """
        me = seg_key(exclude["label"])
        for q in pieces:
            if q is exclude or d2(q["s"], point) >= LINK_DIST:
                continue
            k = seg_key(q["label"])
            twin = (me is not None and k is not None
                    and k[0] == me[0] and k[2] == me[2] and k[1] != me[1])
            return q["label"], twin
        return None, False

    # A1 要分三类，否则数字没法用：
    #   · Inter_ 段没下家      → **真问题**，车进了路口出不去
    #   · Lane_ 段附近有东西但方向不对 → **真问题**，接错了（逆向当成下家）
    #   · Lane_ 段附近什么都没有       → 这条路到头了，断头路/地图边缘，**正常**
    # 不分的话"每条支路各贡献 1 处末段"会把真问题淹掉（实测 39 处里大半是这个）。
    a1, a1_edge, a2, a3 = [], [], [], []
    for p in pieces:
        dout = dir_at(p["sp"], False)
        if not has_downstream(p["e"], dout):
            near, twin = anything_near(p["e"], p)
            if p["is_inter"]:
                a1.append(u"(%8.0f,%9.0f)  %s%s"
                          % (p["e"].x, p["e"].y, p["label"],
                             u"   附近有 %s" % near[:34] if near else u"   附近什么都没有"))
            elif twin:
                # 出向车道的终点挨着同段的入向车道起点 = 这条路到头了，正常
                a1_edge.append(u"%s（对面是 %s）" % (p["label"], near[:34]))
            elif near:
                a1.append(u"(%8.0f,%9.0f)  %s   附近有 %s 但不同向、也不是对向同段"
                          % (p["e"].x, p["e"].y, p["label"], near[:34]))
            else:
                a1_edge.append(p["label"])
        for nm, c in p["sd"].items():
            if "Left" not in nm and "Right" not in nm:
                continue
            k = c.get_number_of_spline_points()
            L = c.get_spline_length()
            cs, ce = ends(c)
            if k <= 3 and L < STUB_LEN:
                a3.append(u"(%8.0f,%9.0f)  %s · %s（%d 点 / %.0f cm）"
                          % (cs.x, cs.y, p["label"], nm, k, L))
                continue
            if not has_downstream(ce, dir_at(c, False)):
                a2.append(u"(%8.0f,%9.0f)  %s · %s（终点在这里）"
                          % (ce.x, ce.y, p["label"], nm))

    # ---- B 组：卡死 ----
    b1, b2, b3, b4 = [], [], [], []
    seg_owners = {}
    for p in pieces:
        if not p["is_inter"] or p["box"] is None:
            continue
        owners = [ia for ia, ib in inters if overlaps(ib, p["box"])]
        seg_owners[p["label"]] = owners
        stop = prop(p["a"], "StopAtInter")
        if not owners:
            b1.append(u"%s（StopAtInter=%s）%s"
                      % (p["label"], stop,
                         u"  ← **永久红灯**" if stop is True else u"  （=false，能过）"))
        elif len(owners) > 1:
            b2.append(u"%s ← %s"
                      % (p["label"], u"、".join(x.get_actor_label() for x in owners)))
        else:
            if not prop(owners[0], "LightDuration"):
                b4.append(u"%s ← %s 的 LightDuration 是 %s"
                          % (p["label"], owners[0].get_actor_label(),
                             prop(owners[0], "LightDuration")))

    # 单组路口：按盒子分组（和蓝图 GetOverlappingActors 一致）
    for ia, ib in inters:
        mine = [p for p in pieces
                if p["is_inter"] and p["box"] and overlaps(ib, p["box"])]
        if not mine:
            continue
        groups = set(str(prop(p["a"], "LightNumber")) for p in mine)
        if len(groups) == 1:
            b3.append(u"%s（%d 段，全是灯组 %s）"
                      % (ia.get_actor_label(), len(mine), list(groups)[0]))

    def block(title, items, why):
        w(u"")
        w(u"-" * 78)
        w(u"%s：%d 处" % (title, len(items)))
        w(u"-" * 78)
        if not items:
            w(u"  （无）")
            return
        w(u"  %s" % why)
        for x in items[:MAX_LIST]:
            w(u"    · %s" % x)
        if len(items) > MAX_LIST:
            w(u"    …… 还有 %d 处" % (len(items) - MAX_LIST))

    w(u"")
    w(u"=" * 78)
    w(u"【裸奔】车开到尽头没有同向下家 → 脱离样条直行，3 秒后瞬移回出生点")
    w(u"=" * 78)
    block(u"A1 主样条终点没有同向下家（真问题）", a1,
          u"这一类**每辆**走到这儿的车都会裸奔。路口段没下家跑 fix_t_junctions.py"
          u"（第 3 步把直行样条写成右转副本）；"
          u"「附近有但不同向」要去看那条路是不是手删/接错了。")
    block(u"A1-edge 路到头了（断头路 / 地图边缘）", a1_edge,
          u"**这些是正常的**：每条支路的末段天然没有下家。车开到尽头会裸奔 3 秒、"
          u"然后瞬移回出生点重新上路——这就是设计里的循环机制，不用修。"
          u"想减少的话只能给那些路加下家，或者接受它。")
    block(u"A2 转弯样条终点没有同向下家", a2,
          u"**只有随机选到那条转弯的车**才裸奔，所以表现是偶发。"
          u"跑 fix_t_junctions.py（第 2 步写成直行副本）。")
    block(u"A3 蓝图默认残桩（≤3 点且 <300cm）", a3,
          u"一条通往虚空的小棍子。跑 fill_turn_splines.py 会写成曲线或直行副本。")

    w(u"")
    w(u"=" * 78)
    w(u"【卡死】车停住再也不走")
    w(u"=" * 78)
    block(u"B1 没有任何路口盒罩住的 Inter_ 段", b1,
          u"**最致命**：蓝图靠 GetOverlappingActors 找子节点，没盒子罩着就没人刷它的"
          u"StopAtInter。标了「永久红灯」的那些，车到那儿会一直等。"
          u"跑 gen_intersections.py 重算盒子。")
    block(u"B2 被两个以上盒子同时罩住", b2,
          u"两套定时器抢着写同一段的灯态，相位互相覆盖。"
          u"收紧 gen_intersections.py 的 CLUSTER_DIST 或手改那几个盒子。")
    block(u"B3 单组路口（Length == 1）", b3,
          u"蓝图里那道 `Length > 1` 闸生效时它们恒绿、不拦车，属于正常。"
          u"**闸要是没了就全是恒红**——拿 check_intersection_gate.py 确认闸还在。")
    block(u"B4 所属路口的 LightDuration 为空/0", b4,
          u"定时器周期是 0，NextTrafficLight 不会被正常驱动。")

    total_run = len(a1) + len(a2) + len(a3)   # 不含 a1_edge：那是正常的路到头
    total_stuck = len([x for x in b1 if u"永久红灯" in x]) + len(b2) + len(b4)
    w(u"")
    w(u"=" * 78)
    w(u"小结")
    w(u"=" * 78)
    w(u"  裸奔源 %d 处（A1 %d / A2 %d / A3 %d）" % (total_run, len(a1), len(a2), len(a3)))
    w(u"  另有 %d 处是「路到头了」（断头路/地图边缘），**正常，不计入**" % len(a1_edge))
    w(u"  卡死源 %d 处（B1 永久红灯 %d / B2 %d / B4 %d）"
      % (total_stuck, len([x for x in b1 if u"永久红灯" in x]), len(b2), len(b4)))
    w(u"  单组路口 %d 个（有闸就正常）" % len(b3))
    w(u"")
    if total_run == 0 and total_stuck == 0:
        w(u"  静态层面干净。剩下的只可能是**动态**原因：")
        w(u"    · 接缝处横向漂移错过下一段的 Box（空隙里车是脱离样条直行的，"
          u"折角越大漂得越多）")
        w(u"    · 前车避让死锁（有 MaxBlockedTime 6 秒 / 路口 16 秒兜底）")
        w(u"    · 车停着超时回溯（StalledTimeout 12 秒 / 路口 15 秒）")
        w(u"  这几样只能在 Simulate 里看，日志里搜「车速为 0 已」能抓到回溯记录。")
    else:
        w(u"  上面每一类都写了对应的修复脚本。A 组基本都是 fix_t_junctions.py /")
        w(u"  fill_turn_splines.py 没跑过；B1 是 gen_intersections.py 没跑过。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[health] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[health] 写入 %s" % OUT)
