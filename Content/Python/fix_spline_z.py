# -*- coding: utf-8 -*-
u"""只调样条的 Z，让它贴地、并让接缝连贯。**所有点的 XY 一个都不动。**

为什么要做（用户 2026-10-29）
----------------------------
车的位置是 `GetLocationAtDistanceAlongSpline` 给的，接缝处 Z 一跳车就竖着蹿；
更要紧的是 `DriveForward` 每帧用 `GetDistanceAlongSplineAtLocation(自己的世界坐标)`
反推「走到哪了」，那是个**三维**最近点查询——Z 差几百厘米时最近点可能落到样条
中段去，车会沿样条前后跳，表现就像脱轨。
（在此之前的交接诊断量的纵向/横向全是 2D 的，Z 一次都没查，这是那份诊断的盲点。）

三条测高铁律，都是生成器里实测踩出来的，这里原样沿用
---------------------------------------------------
1. **高度必须逐点打射线重测**，不能信样条自己的 Z（实测偏差 −546 ~ +411cm）。
2. **一竖线上可能有多层路面**：路口处两条路的面叠着、实测差约 200cm。只取最上面
   那层的话，路口内的点会爬到交叉路的面上、路口外的点还在本路面上，接缝出现
   250cm 台阶。所以做**链式选层**——从只有单层的点起锚、向两头传播、每步挑离
   上一点所选那层最近的，而不是挑最高的。
3. **命中同一块板要跨过去**：射线每次只降 1cm，26cm 厚的板会被连续命中二十几次，
   预算全耗在同一块板上够不到下层。命中后直接跳 SLAB_SKIP。

`Inter_` 段**不实测、由两端桥接**（和生成器的 BRIDGE_INTERSECTIONS 同一个理由）
------------------------------------------------------------------------
路口内部不存在唯一正确的「路面」——两条路的面叠在一起，实测必然在某处跳层。
所以路口段的两个端点**直接取相邻 `Lane_` 端点修好后的 Z**（接缝于是天然连续，
不是"修完再对齐"），中间点按弧长线性插值。这正好同时满足"贴地"和"接缝连贯"。
顺序因此不能换：**先修 `Lane_`（实测），再修 `Inter_`（桥接）**。

同一个 actor 里三条样条常常是同一条路径的副本。本脚本按「XY 序列」做缓存，
副本天然算出同一组 Z，不需要特判——这也是写完之后副本关系不会被破坏的原因。

DRY_RUN=True 只报不改（默认）。
用法：py fix_spline_z.py
"""

import math
import traceback

import unreal

DRY_RUN = False

Z_OFFSET = 2.0            # 车道点应当高于路面这么多，和 gen_traffic_lanes.py 一致
SURFACE_MAX_LAYERS = 40   # 与生成器一致
SLAB_SKIP = 30.0          # 命中路面后往下跳这么多，一步跨过整块板（板厚 26cm）
SLAB_MERGE = 60.0         # 两次命中差小于此视为同一块板
DZ_WARN = 60.0            # 单点高度偏差超过这个值就算不贴
SEAM_WARN = 30.0          # 接缝 Z 落差超过这个值就算不连贯
# 中间点修正的开关。**默认关**，理由见下面 run() 里那段坡度对比：
# 偏差最大的点往往是量到了**交叉路**的面（路口内部本条路的 SplineMesh 可能是断的），
# 把它拉过去会在紧邻端点的地方造出几米的断崖，而端点正是要保证顺畅的交接处。
# 用户 2026-10-29 定的优先级：先保证车从 lane 到 inter 再回 lane 顺畅。
INTER_FIX_ENABLE = False
INTER_FIX_DEV = 80.0      # 路口段中间点偏离实测路面超过这个值，就把**那个点**
                          # 拉回路面当锚点；没有超标点的路口段就是纯两端桥接。
                          # 用户 2026-10-29 定的策略：主要靠两端桥接，中间落差太大
                          # 才调。所以这是个"例外阈值"，不是常规修正量——调小它
                          # 会让路口越来越接近"逐点实测"，而实测在路口内部会跳层。
BRIDGE_DEV_WARN = 80.0    # 路口段插值出来的 Z 与实测路面差超过这个值就点名。
                          # 和 gen_traffic_lanes.py 同名常量一致：路口内部不存在
                          # 唯一正确的路面，桥接保证车开得顺，代价是可能浮起/陷入。
                          # 偏差大说明那个路口两条路的路面本来就对不上。
LINK_DIST = 600.0         # 接缝配对距离，和 fix_t_junctions.py 一致
LINK_DOT = 0.5            # 同向判据：只看位置会把对向车道当成下家
MAIN_KEYS = ("Spline", "SplineComponent")
WORST_ROWS = 30

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "fix_spline_z.txt"
lines = []
_mark_failed = []


def w(s=u""):
    lines.append(str(s))
    unreal.log("[z] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def pts_of(sp):
    return [sp.get_location_at_spline_point(i, WS)
            for i in range(sp.get_number_of_spline_points())]


def dir_at(sp, at_start):
    n = sp.get_number_of_spline_points()
    if n < 2:
        return None
    i, j = (0, 1) if at_start else (n - 1, n - 2)
    p, q = sp.get_location_at_spline_point(i, WS), sp.get_location_at_spline_point(j, WS)
    dx, dy = (q.x - p.x, q.y - p.y) if at_start else (p.x - q.x, p.y - q.y)
    L = math.sqrt(dx * dx + dy * dy)
    return None if L < 1e-3 else (dx / L, dy / L)


def tangent3(pts, zs, at_start):
    u"""端点的 3D 切线方向（单位向量）。zs 是这条样条的 Z 序列（可传改后的）。

    所有点都是 CURVE 类型，自动切线由邻点算；**端点的切线只看它自己这条样条
    的内部邻点，完全不知道隔壁那条样条往哪走**——所以接缝处的切线连续性
    没有任何机制保证，这是结构性的，不是数值误差。
    """
    n = len(pts)
    if n < 2:
        return None
    i, j = (0, 1) if at_start else (n - 1, n - 2)
    dx = pts[j].x - pts[i].x
    dy = pts[j].y - pts[i].y
    dz = zs[j] - zs[i]
    if not at_start:
        dx, dy, dz = -dx, -dy, -dz
    L = math.sqrt(dx * dx + dy * dy + dz * dz)
    return None if L < 1e-3 else (dx / L, dy / L, dz / L)


def yaw_pitch(v):
    u"""切线的偏航（度，0~360 无意义，只用来求差）和俯仰（度，抬头为正）。"""
    yaw = math.degrees(math.atan2(v[1], v[0]))
    pitch = math.degrees(math.asin(max(-1.0, min(1.0, v[2]))))
    return yaw, pitch


def angdiff(a, b):
    d = (a - b + 180.0) % 360.0 - 180.0
    return abs(d)


def d2(p, q):
    return math.sqrt((p.x - q.x) ** 2 + (p.y - q.y) ** 2)


def mark_edited(sp):
    u"""实测 spline_has_been_edited 可用、b_spline_has_been_edited 不存在。"""
    for n in ("spline_has_been_edited", "b_spline_has_been_edited"):
        try:
            sp.set_editor_property(n, True)
            return True
        except Exception:
            continue
    return False


def surface_layers(world, x, y, z_hint):
    u"""这一竖线上所有互不相同的路面（SplineMesh）高度，由高到低。照抄
    diag_lane_fit.py，判据必须和生成器一致，否则两处对「路面」的定义会打架。"""
    top = z_hint + 60000.0
    bottom = z_hint - 60000.0
    layers = []
    for _ in range(SURFACE_MAX_LAYERS):
        try:
            hit = unreal.SystemLibrary.line_trace_single(
                world, unreal.Vector(x, y, top), unreal.Vector(x, y, bottom),
                unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
                False, [], unreal.DrawDebugTrace.NONE, True)
        except Exception:
            break
        if hit is None:
            break
        d = hit.to_dict()
        if not d.get("blocking_hit"):
            break
        comp = d.get("hit_component")
        z = d["impact_point"].z
        cn = comp.get_class().get_name() if comp is not None else "?"
        if "SplineMesh" in cn:
            if not layers or (layers[-1] - z) > SLAB_MERGE:
                layers.append(z)
            top = z - SLAB_SKIP
        else:
            top = z - 1.0
        if top <= bottom:
            break
    return layers


def chained_pick(all_layers, hints):
    u"""链式选层。all_layers[i] 是第 i 点的所有路面高度（由高到低），hints[i] 是
    该点现在的裸路面高度（当前 Z 减 Z_OFFSET）。

    **不能逐点各挑最近的**：路口处两层只差 200cm，逐点挑会在中途跳层、接缝出台阶。
    做法是从**只有一层**的点起锚（那儿没有歧义），向两头传播，每步挑离上一点
    所选那层最近的。全程都是多层时退回「离现值最近」起锚。
    """
    n = len(all_layers)
    chosen = [None] * n
    anchors = [i for i in range(n) if len(all_layers[i]) == 1]
    if anchors:
        a = anchors[len(anchors) // 2]
    else:
        a = None
        for i in range(n):
            if all_layers[i]:
                a = i
                break
        if a is None:
            return chosen
    chosen[a] = min(all_layers[a], key=lambda z: abs(z - hints[a])) \
        if all_layers[a] else None
    for i in range(a + 1, n):
        prev = chosen[i - 1]
        if not all_layers[i]:
            continue
        ref = prev if prev is not None else hints[i]
        chosen[i] = min(all_layers[i], key=lambda z: abs(z - ref))
    for i in range(a - 1, -1, -1):
        prev = chosen[i + 1]
        if not all_layers[i]:
            continue
        ref = prev if prev is not None else hints[i]
        chosen[i] = min(all_layers[i], key=lambda z: abs(z - ref))
    return chosen


def measure_lane_z(world, pts):
    u"""逐点测出「应该的 Z」。返回 (list[新 Z 或 None], 多层点数, 无面点数)。"""
    hints = [p.z - Z_OFFSET for p in pts]
    all_layers = [surface_layers(world, p.x, p.y, h) for p, h in zip(pts, hints)]
    picked = chained_pick(all_layers, hints)
    multi = sum(1 for L in all_layers if len(L) > 1)
    none_ = sum(1 for L in all_layers if not L)
    return [None if z is None else z + Z_OFFSET for z in picked], multi, none_


def arc_lengths(pts):
    u"""累计弧长（按控制点折线算，够用：只拿来做插值权重）。"""
    out = [0.0]
    for i in range(1, len(pts)):
        out.append(out[-1] + math.sqrt((pts[i].x - pts[i - 1].x) ** 2 +
                                       (pts[i].y - pts[i - 1].y) ** 2))
    return out


def write_z(sp, pts, newz):
    u"""只改 Z，XY 原值回填。

    用 set_location_at_spline_point 而不是 clear + 重加：后者会把点型和切线一起
    重建，而我们只想动 Z；前者传回同样的 x/y，XY 位是**原样**的。
    两条铁律照旧：mark_edited 要在改动**之前**（它是 set_editor_property，会触发
    actor 重跑构造脚本、组件整批重建，放最后调的话刚写的值落在被丢弃的那批上）；
    modify() 也要在之前（普通函数调用只改内存、不标记已修改，存盘时整个对象不被
    序列化，实测 8 条只活 1 条）。
    """
    marked = mark_edited(sp)
    try:
        sp.modify(True)
        owner = sp.get_owner()
        if owner is not None:
            owner.modify(True)
    except Exception:
        pass
    n = 0
    for i, z in enumerate(newz):
        if z is None:
            continue
        p = pts[i]
        if abs(p.z - z) < 0.01:
            continue
        sp.set_location_at_spline_point(i, unreal.Vector(p.x, p.y, z), WS, False)
        n += 1
    sp.update_spline()
    if not marked:
        _mark_failed.append(sp.get_name())
    return n


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    w(u"=== 样条贴地与接缝连贯（只动 Z，XY 不变）===")
    w(u"关卡：%s     模式：%s"
      % (world.get_name() if world else u"?", u"只报不改" if DRY_RUN else u"**写入**"))
    w(u"期望车道点高于路面 %.0f cm；单点偏差 >%.0f 算不贴；接缝落差 >%.0f 算不连贯"
      % (Z_OFFSET, DZ_WARN, SEAM_WARN))
    w(u"")

    segs = []
    for a in eas.get_all_level_actors():
        lbl = a.get_actor_label()
        if not (lbl.startswith("Lane_") or lbl.startswith("Inter_")):
            continue
        comps = {c.get_name(): c
                 for c in a.get_components_by_class(unreal.SplineComponent)}
        comps = {k: v for k, v in comps.items()
                 if v.get_number_of_spline_points() >= 2}
        if comps:
            segs.append({"a": a, "lbl": lbl, "comps": comps,
                         "is_inter": lbl.startswith("Inter_")})
    n_lane = sum(1 for s in segs if not s["is_inter"])
    w(u"段 %d 个（Lane_ %d / Inter_ %d）" % (len(segs), n_lane, len(segs) - n_lane))

    # ---------- 第一步：Lane_ 逐点实测 ----------
    cache, plan = {}, {}        # plan[(lbl, comp)] = (pts, newz)
    surf_cache = {}             # (x, y) -> layers，路口段校验用，按点缓存
    lane_stat = []
    for s in segs:
        if s["is_inter"]:
            continue
        for nm, sp in sorted(s["comps"].items()):
            pts = pts_of(sp)
            key = tuple((round(p.x, 1), round(p.y, 1)) for p in pts)
            if key in cache:
                newz, multi, none_ = cache[key]
            else:
                newz, multi, none_ = measure_lane_z(world, pts)
                cache[key] = (newz, multi, none_)
            plan[(s["lbl"], nm)] = (pts, newz)
            dzs = [abs(p.z - z) for p, z in zip(pts, newz) if z is not None]
            lane_stat.append((max(dzs) if dzs else 0.0, s["lbl"], nm,
                              sum(1 for d in dzs if d > DZ_WARN), len(pts), none_))
    w(u"Lane_ 实测完成，竖线射线缓存命中后共测 %d 条不同的 XY 序列" % len(cache))
    off = [t for t in lane_stat if t[3] > 0]
    nosurf = [t for t in lane_stat if t[5] > 0]
    w(u"  有点偏离 >%.0f cm 的样条：%d / %d" % (DZ_WARN, len(off), len(lane_stat)))
    w(u"  有点底下压根没有路面的样条：%d（这些点的 Z 会保持原样，不瞎填）" % len(nosurf))
    w(u"")
    w(u"  最不贴的 %d 条：" % WORST_ROWS)
    for mx, lbl, nm, cnt, tot, ns in sorted(lane_stat, reverse=True)[:WORST_ROWS]:
        w(u"    %-46s %-12s 最大偏差 %7.1f cm   超标 %d/%d 点%s"
          % (lbl, nm, mx, cnt, tot, u"   无路面 %d 点" % ns if ns else u""))
    w(u"")

    # ---------- 第二步：接缝配对 ----------
    ends, starts = [], []
    for s in segs:
        for nm, sp in s["comps"].items():
            pts = pts_of(sp)
            do, di = dir_at(sp, False), dir_at(sp, True)
            if do:
                ends.append((s, nm, pts[-1], do, len(pts) - 1))
            if di:
                starts.append((s, nm, pts[0], di, 0))
    seams = []
    for (sa, na, pa, da, ia) in ends:
        for (sb, nb, pb, db, ib) in starts:
            if sa is sb:
                continue
            if d2(pa, pb) >= LINK_DIST:
                continue
            if (da[0] * db[0] + da[1] * db[1]) <= LINK_DOT:
                continue
            seams.append((sa, na, ia, pa, sb, nb, ib, pb))
    w(u"接缝 %d 处（同向、%.0f cm 内）" % (len(seams), LINK_DIST))

    def planned_z(s, nm, idx, fallback):
        key = (s["lbl"], nm)
        if key in plan:
            z = plan[key][1][idx]
            if z is not None:
                return z
        return fallback

    # ---------- 第三步：Inter_ 由两端桥接 ----------
    # 端点取「相邻 Lane_ 修好后的 Z」，于是接缝天然连续，不是修完再去对齐。
    bridged, orphan, multi_partner, fixed_pts = 0, [], [], []
    smooth = {u"bridge": [], u"fixed": []}
    for s in segs:
        if not s["is_inter"]:
            continue
        for nm, sp in sorted(s["comps"].items()):
            pts = pts_of(sp)
            # 一个路口段常常有**多个**同向上游/下游：LINK_DIST 是 600，而同一条路
            # 相邻两条车道只差 360，于是 -0540 的车道也会被配给 -0180 的路口段。
            # 600 这个阈值是从 fix_t_junctions 继承的，那边用来判断「存不存在」下家；
            # 拿来**决定 Z 取谁**就不够——必须取**平面最近**的那个伙伴，
            # 它才是同偏移的那条（约 150cm，而跨车道的是 390cm 以上）。
            # 早先写成"遍历时后来者覆盖"，Z 取谁纯看顺序，另一个伙伴就留下
            # 45cm 的台阶：实测 6 处接缝因此比修之前更差。
            cin, cout = [], []
            for (sa, na, ia, pa, sb, nb, ib, pb) in seams:
                if sb is s and nb == nm:          # 有人接进来 → 定我的起点
                    cin.append((d2(pa, pb), planned_z(sa, na, ia, pa.z),
                                u"%s.%s" % (sa["lbl"], na)))
                if sa is s and na == nm:          # 我接出去 → 定我的终点
                    cout.append((d2(pa, pb), planned_z(sb, nb, ib, pb.z),
                                 u"%s.%s" % (sb["lbl"], nb)))
            for side, cands in ((u"上游", cin), (u"下游", cout)):
                if len(cands) > 1:
                    zs = [c[1] for c in cands]
                    if max(zs) - min(zs) > SEAM_WARN:
                        # 列出来而不是静默取最近的：剩下那个伙伴必然留个台阶，
                        # 不打出来就无从查证"为什么这条接缝还是不齐"。
                        multi_partner.append(
                            (max(zs) - min(zs), s["lbl"], nm, side,
                             u"；".join(u"%s %.0fcm Z=%.0f" % (c[2], c[0], c[1])
                                        for c in sorted(cands))))
            z_in = min(cin)[1] if cin else None
            z_out = min(cout)[1] if cout else None
            if z_in is None and z_out is None:
                orphan.append((s["lbl"], nm))
                continue
            if z_in is None:
                z_in = pts[0].z
            if z_out is None:
                z_out = pts[-1].z
            L = arc_lengths(pts)
            total = L[-1] if L[-1] > 1e-3 else 1.0
            frac = [L[i] / total for i in range(len(pts))]
            bridge = [z_in + (z_out - z_in) * f for f in frac]

            # 两端桥接为主；中间点偏离实测路面太多才把**那一个点**拉回路面。
            # 层选择用**链式**、从 z_in 起锚：路口内部两条路的面叠着（实测差约
            # 200cm），逐点各挑最近的会在中途跳层。端点的 Z 由相邻车道定死，
            # 正好是那个锚——这是生成器当年回避实测路口的那个难题的解法。
            anchors = {0: z_in, len(pts) - 1: z_out}
            ref = z_in - Z_OFFSET
            for i in range(1, len(pts) - 1):
                k = (round(pts[i].x, 1), round(pts[i].y, 1))
                if k in surf_cache:
                    layers = surf_cache[k]
                else:
                    layers = surface_layers(world, pts[i].x, pts[i].y, ref)
                    surf_cache[k] = layers
                if not layers:
                    continue
                zs = min(layers, key=lambda z: abs(z - ref))
                ref = zs
                # 这里不看开关：两个方案都要算出来才有得比。
                # 开关只决定最后**采用**哪个（见下面 plan 赋值）。
                if abs(bridge[i] - (zs + Z_OFFSET)) > INTER_FIX_DEV:
                    anchors[i] = zs + Z_OFFSET
                    fixed_pts.append((abs(bridge[i] - (zs + Z_OFFSET)),
                                      s["lbl"], nm, i))

            # 锚点之间按弧长线性插值：没有超标点时 anchors 只有两端，
            # 结果就是纯桥接；有超标点时折线穿过它们，不会在"修过/没修"
            # 之间留折角。
            ai = sorted(anchors)
            fixedz = []
            for i in range(len(pts)):
                if i in anchors:
                    fixedz.append(anchors[i])
                    continue
                lo = max(j for j in ai if j < i)
                hi = min(j for j in ai if j > i)
                span = frac[hi] - frac[lo]
                tt = 0.0 if span <= 1e-9 else (frac[i] - frac[lo]) / span
                fixedz.append(anchors[lo] + (anchors[hi] - anchors[lo]) * tt)
            newz = fixedz if INTER_FIX_ENABLE else bridge
            # 顺畅度指标：相邻控制点之间的 Z 落差和坡度（Δz / 平面间距）。
            # 两个方案都算——「超 80cm 的条数」只说明贴不贴地，说明不了车开过去
            # 颠不颠；而用户的优先级是后者。
            for tag, zz in ((u"bridge", bridge), (u"fixed", fixedz)):
                mstep = mslope = 0.0
                for i in range(1, len(pts)):
                    dxy = math.sqrt((pts[i].x - pts[i - 1].x) ** 2 +
                                    (pts[i].y - pts[i - 1].y) ** 2)
                    dz = abs(zz[i] - zz[i - 1])
                    mstep = max(mstep, dz)
                    if dxy > 1.0:
                        mslope = max(mslope, dz / dxy)
                smooth[tag].append((mstep, mslope, s["lbl"], nm))

            plan[(s["lbl"], nm)] = (pts, newz)
            bridged += 1
    w(u"  路口段按两端桥接：%d 条；两头都没接缝、保持原样：%d 条"
      % (bridged, len(orphan)))
    for lbl, nm in orphan[:10]:
        w(u"    孤立：%s.%s" % (lbl, nm))
    w(u"")
    w(u"  --- 顺畅度（相邻控制点的 Z 落差 / 坡度）---")
    for tag, label in ((u"bridge", u"纯两端桥接"), (u"fixed", u"带中间点修正")):
        rows = smooth[tag]
        if not rows:
            continue
        steps = [r[0] for r in rows]
        slopes = [r[1] for r in rows]
        w(u"  %s：最大单段落差 %.1f cm（平均 %.1f）；最大坡度 %.2f = %.0f°（平均 %.2f）"
          % (label, max(steps), sum(steps) / len(steps), max(slopes),
             math.degrees(math.atan(max(slopes))), sum(slopes) / len(slopes)))
        for how, keyf in ((u"最陡", lambda r: -r[1]), (u"落差最大", lambda r: -r[0])):
            w(u"    %s的 5 条：" % how)
            for mstep, mslope, lbl, nm in sorted(rows, key=keyf)[:5]:
                w(u"      落差 %7.1f cm  坡度 %5.2f（%2.0f°）  %s.%s"
                  % (mstep, mslope, math.degrees(math.atan(mslope)), lbl, nm))
    w(u"  当前采用：%s（INTER_FIX_ENABLE=%s）"
      % (u"带中间点修正" if INTER_FIX_ENABLE else u"纯两端桥接", INTER_FIX_ENABLE))
    w(u"")
    if fixed_pts:
        w(u"  中间点偏离实测路面 >%.0f cm、被拉回路面的点 %d 个（分布在 %d 条样条上）："
          % (INTER_FIX_DEV, len(fixed_pts), len({(f[1], f[2]) for f in fixed_pts})))
        for dev, lbl, nm, i in sorted(fixed_pts, reverse=True)[:15]:
            w(u"    原本会偏 %6.1f cm  %s.%s 第 %d 点" % (dev, lbl, nm, i))
    else:
        w(u"  中间点没有偏离 >%.0f cm 的，全部是纯两端桥接" % INTER_FIX_DEV)
    if multi_partner:
        w(u"  有多个同向伙伴、而且它们的 Z 互相差 >%.0f cm 的端点 %d 处"
          u"（取平面最近的那个，另一个会留台阶）：" % (SEAM_WARN, len(multi_partner)))
        for spread, lbl, nm, side, detail in sorted(multi_partner, reverse=True)[:12]:
            w(u"    差 %5.1f cm  %s.%s 的%s：%s" % (spread, lbl, nm, side, detail))
    w(u"")

    # ---------- 接缝落差：现在 vs 修完 ----------
    rows = []
    for (sa, na, ia, pa, sb, nb, ib, pb) in seams:
        before = abs(pa.z - pb.z)
        za = planned_z(sa, na, ia, pa.z)
        zb = planned_z(sb, nb, ib, pb.z)
        rows.append((before, abs(za - zb), sa["lbl"], na, sb["lbl"], nb,
                     d2(pa, pb)))
    bad_b = [r for r in rows if r[0] > SEAM_WARN]
    bad_a = [r for r in rows if r[1] > SEAM_WARN]
    w(u"接缝 Z 落差 >%.0f cm：修前 %d 处 → 修后 %d 处"
      % (SEAM_WARN, len(bad_b), len(bad_a)))
    if rows:
        w(u"  平均落差 %.1f → %.1f cm；最大 %.1f → %.1f cm"
          % (sum(r[0] for r in rows) / len(rows),
             sum(r[1] for r in rows) / len(rows),
             max(r[0] for r in rows), max(r[1] for r in rows)))
    w(u"  修前最差的 %d 处：" % WORST_ROWS)
    for before, after, la, na, lb, nb, gap in sorted(rows, reverse=True)[:WORST_ROWS]:
        w(u"    %7.1f → %7.1f cm   %s.%s → %s.%s（平面 %.0f cm）"
          % (before, after, la, na, lb, nb, gap))
    w(u"")
    if bad_a:
        w(u"  修后仍然超标的 %d 处（这些多半是两段路面本来就对不上，"
          u"属于关卡几何问题，不是样条能解决的）：" % len(bad_a))
        for before, after, la, na, lb, nb, gap in sorted(
                bad_a, key=lambda r: -r[1])[:WORST_ROWS]:
            w(u"    %7.1f → %7.1f cm   %s.%s → %s.%s" % (before, after, la, na, lb, nb))
        w(u"")

    # ---------- 路口段：插值出来的 Z 离真实路面有多远 ----------
    # 这是「接缝齐了」之外的另一半：接缝数字再漂亮，也不代表路口内部贴地。
    bdev = []
    for s2 in segs:
        if not s2["is_inter"]:
            continue
        for nm, sp in sorted(s2["comps"].items()):
            key = (s2["lbl"], nm)
            if key not in plan:
                continue
            pts, newz = plan[key]
            worst, worst_i, nmeas = 0.0, -1, 0
            for i in range(1, len(pts) - 1):      # 只看中间点，端点是接缝定死的
                if newz[i] is None:
                    continue
                k = (round(pts[i].x, 1), round(pts[i].y, 1))
                if k in surf_cache:
                    layers = surf_cache[k]
                else:
                    layers = surface_layers(world, pts[i].x, pts[i].y,
                                            newz[i] - Z_OFFSET)
                    surf_cache[k] = layers
                if not layers:
                    continue
                nmeas += 1
                zs = min(layers, key=lambda z: abs(z - (newz[i] - Z_OFFSET)))
                dev = abs(newz[i] - Z_OFFSET - zs)
                if dev > worst:
                    worst, worst_i = dev, i
            if nmeas:
                bdev.append((worst, s2["lbl"], nm, worst_i, nmeas, len(pts)))
    over = [b for b in bdev if b[0] > BRIDGE_DEV_WARN]
    w(u"路口段插值 Z 与实测路面的偏差（%d 条有可测的中间点）：" % len(bdev))
    if bdev:
        w(u"  平均最大偏差 %.1f cm；超过 %.0f cm 的 %d 条"
          % (sum(b[0] for b in bdev) / len(bdev), BRIDGE_DEV_WARN, len(over)))
        w(u"  最差的 %d 条（这些路口里车可能浮起或陷入）：" % min(WORST_ROWS, len(bdev)))
        for worst, lbl, nm, wi, nmeas, tot_p in sorted(bdev, reverse=True)[:WORST_ROWS]:
            w(u"    %7.1f cm  %s.%s（第 %d 点，可测 %d/%d）"
              % (worst, lbl, nm, wi, nmeas, tot_p))
    w(u"")

    # ---------- 同一条样条里「测到的点」和「测不到的点」混在一起 ----------
    # 测不到的点保持原 Z 不动，而邻点会被改——于是接缝处会多一个小台阶。
    # 不报的话这种小隆起只能靠肉眼在关卡里找。
    mixed = []
    for key, (pts, newz) in plan.items():
        if key[0].startswith("Inter_"):
            continue
        moved = [i for i, (p, z) in enumerate(zip(pts, newz))
                 if z is not None and abs(p.z - z) > 1.0]
        kept = [i for i, z in enumerate(newz) if z is None]
        if moved and kept:
            step = max(abs(pts[i].z - newz[i]) for i in moved)
            mixed.append((step, key[0], key[1], len(kept), len(pts)))
    if mixed:
        w(u"有「测不到路面、保持原 Z」的点、同时又有点要被改的车道 %d 条"
          u"（这些地方会多一个小台阶，高度等于下面这个数）：" % len(mixed))
        for step, lbl, nm, nkept, tot_p in sorted(mixed, reverse=True)[:12]:
            w(u"    %6.1f cm  %s.%s（保持原样 %d/%d 点）" % (step, lbl, nm, nkept, tot_p))
        w(u"")

    # ---------- 接缝的切线方向连续性（yaw / pitch），改前 vs 改后 ----------
    # 用户 2026-10-29 指出：顺畅不只是 Z，还有斜率指向。确实——车的朝向来自
    # GetRotationAtDistanceAlongSpline，接缝处切线对不上车会瞬间扭头，
    # 比 Z 跳一下更像脱轨。而配对用的 dot>0.5 最多差 60°，根本不够说明顺畅。
    def zs_of(s_, nm, pts, after):
        if not after:
            return [p.z for p in pts]
        key = (s_["lbl"], nm)
        if key not in plan:
            return [p.z for p in pts]
        nz = plan[key][1]
        return [nz[i] if nz[i] is not None else pts[i].z for i in range(len(pts))]

    tan_rows = []
    for (sa, na, ia, pa, sb, nb, ib, pb) in seams:
        psa = pts_of(sa["comps"][na])
        psb = pts_of(sb["comps"][nb])
        rec = []
        for after in (False, True):
            ta = tangent3(psa, zs_of(sa, na, psa, after), False)
            tb = tangent3(psb, zs_of(sb, nb, psb, after), True)
            if ta is None or tb is None:
                rec = None
                break
            ya, pia = yaw_pitch(ta)
            yb, pib = yaw_pitch(tb)
            rec.append((angdiff(ya, yb), abs(pia - pib)))
        if rec:
            tan_rows.append((rec[0][0], rec[0][1], rec[1][0], rec[1][1],
                             sa["lbl"], na, sb["lbl"], nb))
    if tan_rows:
        w(u"接缝切线方向连续性（%d 处）：" % len(tan_rows))
        for idx, label in ((0, u"偏航 yaw"), (1, u"俯仰 pitch")):
            before = [r[idx] for r in tan_rows]
            after = [r[idx + 2] for r in tan_rows]
            w(u"  %s 角差：平均 %.1f° → %.1f°；最大 %.1f° → %.1f°；"
              u">10° 的 %d → %d 处"
              % (label, sum(before) / len(before), sum(after) / len(after),
                 max(before), max(after),
                 sum(1 for x in before if x > 10), sum(1 for x in after if x > 10)))
        w(u"  改后 pitch 角差最大的 %d 处（Z 一动，端点切线的俯仰就跟着动，"
          u"所以这一项有可能被改坏，必须单独看）：" % min(12, len(tan_rows)))
        for yb_, pb_, ya_, pa_, la, na_, lb, nb_ in sorted(
                tan_rows, key=lambda r: -r[3])[:12]:
            w(u"    pitch %5.1f° → %5.1f°   yaw %5.1f° → %5.1f°   %s.%s → %s.%s"
              % (pb_, pa_, yb_, ya_, la, na_, lb, nb_))
        # yaw 角差单列：它**不受本脚本影响**（只动 Z），是两段走向本身对不齐。
        # 和 pitch 混在一张表里会让人以为改 Z 也能治它，所以分开打。
        w(u"  yaw 角差最大的 12 处（**本脚本治不了**：只动 Z 不影响 yaw。"
          u"这是两段样条走向本身对不齐，车过去会扭头）：")
        for yb_, pb_, ya_, pa_, la, na_, lb, nb_ in sorted(
                tan_rows, key=lambda r: -r[2])[:12]:
            w(u"    yaw %5.1f°   pitch %5.1f°   %s.%s → %s.%s"
              % (ya_, pa_, la, na_, lb, nb_))
        w(u"")

    if DRY_RUN:
        tot = sum(1 for (pts, nz) in plan.values()
                  for p, z in zip(pts, nz) if z is not None and abs(p.z - z) > 0.01)
        w(u"=== DRY_RUN=True，没有写入 ===")
        w(u"打算改 %d 条样条上的 %d 个点的 Z，XY 一个都不动。" % (len(plan), tot))
        w(u"确认上面的「修前 → 修后」合理后，把 DRY_RUN 改成 False 再跑一次。")
        return

    # ---------- 写入 ----------
    comp_of = {}
    for s in segs:
        for nm, sp in s["comps"].items():
            comp_of[(s["lbl"], nm)] = sp
    changed = total_pts = 0
    for key, (pts, newz) in sorted(plan.items()):
        sp = comp_of.get(key)
        if sp is None:
            continue
        k = write_z(sp, pts, newz)
        if k:
            changed += 1
            total_pts += k
    w(u"已改 %d 条样条、%d 个点的 Z" % (changed, total_pts))

    # 回读验证：计数器只能证明函数被调用过，证明不了结果留住了
    bad = 0
    for key, (pts, newz) in sorted(plan.items()):
        sp = comp_of.get(key)
        if sp is None:
            continue
        cur = pts_of(sp)
        for i, z in enumerate(newz):
            if z is None or i >= len(cur):
                continue
            if abs(cur[i].z - z) > 1.0:
                bad += 1
            if abs(cur[i].x - pts[i].x) > 0.01 or abs(cur[i].y - pts[i].y) > 0.01:
                w(u"!! %s.%s 第 %d 点的 XY 变了 —— 本脚本不该动 XY" % (key[0], key[1], i))
    w(u"回读：Z 不一致的点 %d 个（应为 0）" % bad)
    if _mark_failed:
        w(u"!! 这些样条 mark_edited 没成功，存盘可能留不住：%s"
          % u"、".join(sorted(set(_mark_failed))[:20]))
    w(u"")
    w(u"别忘了在编辑器里 Ctrl+S 存盘，然后重开关卡再跑一次本脚本对数字"
      u"——这是验证「存住了没有」的标准动作。")


try:
    run()
except Exception:
    w(u"!! 脚本异常：")
    w(traceback.format_exc())
flush()
w(u"报告：%s" % OUT)
