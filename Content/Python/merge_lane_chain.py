# -*- coding: utf-8 -*-
u"""把「车道段 → 路口段 → 车道段」这条链合并成**一条**连续车道，并删掉多余的 actor。

背景（用户 2026-10-29）
----------------------
`Intersection_19` 要降级成普通路。`drop_intersection.py` 的做法是
**保留 `Inter_` 段、只把它的 `StopAtInter` 写成 false**，理由写在那份 docstring 里：
删掉它会在两条 `Lane_` 之间留下约 1900cm 没有样条的缺口，而车靠
`TraceForNewPath` 盲接的余量只有 233cm（球半径 150 + Box 半尺寸 83）。

**合并比那个做法更彻底**，这也是本脚本存在的理由：
三段拼成一条之后，原来两道 150cm 的接缝变成样条**内部的点**，
车从头到尾不再有任何一次「脱离样条 → 重新 sweep 搜路」的交接。
接缝处的折角同时消失（那两个点原来是端点、`CURVE_CLAMPED` 把切线归零过，
现在是中间点、按 `CURVE` 给非零自动切线）。

**保留的是链首那个 actor**，不是新建一个。原因是 Box 挂在**第一个控制点**上：
留链首 → 上游车道交接用的那个 Box 原地不动，入口行为一个字节都没变；
而链尾的终点也没变，所以下游交接同样不变。**中间两个 Box 连同 actor 一起删掉。**

写之前有三道**会拒绝执行**的检查，每一道都对应一种"合并会把事情弄坏"的情形：
  1. **接缝必须真的首尾相接且同向**（间距 ≤ SEAM_MAX、切线 dot > LINK_DOT）。
     标签顺序填错、或者中间那段方向是反的时候，盲目拼接会拼出一条 Z 字形，
     而点数和长度看起来都正常——报告里不打这两个数就查不出来。
  2. **要删的段不能被别的路口盒罩着**。`Inter_` 段是路口内部的行驶路径，
     若它同时属于另一个路口，删掉等于把那个路口的一条通路抽走。
  3. **要删的段的起点不能是别人的下家**。横向那条路的转弯曲线可能正好
     终止在这里、指望这个 Box 接棒；删了它那条转弯就通到虚空。
     降级路口时横向本来就不该再转进来，但那是另一处要改的东西，
     不能靠这个脚本静默替它决定——报出来让人看。

`StopAtInter` 一律写成 false。合并后留下的是 `Lane_`，而 `Lane_` 和 `Inter_`
**是同一个类**（CLAUDE.md：两者都是 `IntersectionChild`、同一个碰撞通道），
所以车的 `TraceForIntersection` 照样能 cast 到它、照样会读这个值；
盒子删掉之后再没有 `BP_Intersection` 去刷它，残留的 `True` 会让车永远等绿灯
——就是最初 I08 卡车那条机制。

**永久性**：下次全图重建时这个路口会原样长回来。配置那一半要在
`gen_traffic_lanes.py` 的 `NO_JUNCTION_WITH` 里加上横向那条路
（和 R020 / R004 / R005 同一个办法；只加 `EXCLUDE_ROADS` 没用，
被排除的样条仍然参与相交检测）。报告会打出横向那条路的名字。

DRY_RUN=True 只报不改（默认）。
用法：py merge_lane_chain.py
"""

import math
import traceback

import unreal

DRY_RUN = False

# 要合并的几条链。**标签随便填什么顺序都行**，行驶顺序由脚本从几何推（见
# order_chain）。每条链合并后只留一个 actor（链首），其余删掉。
#
# 为什么不让人填顺序：段号**不保证**沿行驶方向递增。实测 `+0180` 这条是递减的
# （S01 → I01 → S00，接缝 147cm / 141cm，Z 从 2998 降到 2416），而对向的
# `-0180` 大概率相反。填错的后果是拼出一条跨数千厘米的 Z 字形，**而点数和
# 长度看起来完全正常**——只能靠接缝间距和切线 dot 才看得出来。
# 顺序既然是数据里就有的，就不该靠人记。
CHAINS = [
    [   # +0180：实测行驶顺序 S01 → I01 → S00
        "Lane_BranchRoad_R024_Merged_+0180_S01",
        "Inter_BranchRoad_R024_Merged_+0180_I01",
        "Lane_BranchRoad_R024_Merged_+0180_S00",
    ],
    [   # -0180：对向车道，顺序交给 order_chain 推
        "Lane_BranchRoad_R024_Merged_-0180_S00",
        "Inter_BranchRoad_R024_Merged_-0180_I01",
        "Lane_BranchRoad_R024_Merged_-0180_S01",
    ],
]

# 要降级的那个路口。只用在检查 2 的提示文字里（"罩它的不是这个盒子"），
# 写死名字会让提示和实际目标不一致。
TARGET_INTERSECTION = "Intersection_19"

SEAM_MAX = 400.0   # 接缝间距上限。生成器的 TARGET_GAP 是 150，留两倍余量兜住
                   # INTER_HALF_EXTRA 之类的加长；超了就不是"首尾相接"，拒绝
LINK_DIST = 600.0  # 下家判定距离，和 fix_t_junctions.py / repair 一致
STUB_LEN_MAX = 150.0  # 2 点且不长于此 = 蓝图默认残桩，和 repair_degenerate_splines 一致
LINK_DOT = 0.5     # 同向判据：只看位置会把对向车道当成下家
MAIN_KEYS = ("Spline", "SplineComponent")

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "merge_lane_chain.txt"
lines = []
_mark_failed = []


def w(s=u""):
    lines.append(str(s))
    unreal.log("[merge] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def mark_edited(sp):
    u"""实测 spline_has_been_edited 可用、b_spline_has_been_edited 不存在。"""
    for n in ("spline_has_been_edited", "b_spline_has_been_edited"):
        try:
            sp.set_editor_property(n, True)
            return True
        except Exception:
            continue
    return False


def dedup_pts(pts, eps=2.0):
    u"""相邻重合点去重。两点重合时自动切线必然是零，点型改成 CURVE 也救不回来。"""
    out = []
    for p in pts:
        if out:
            q = out[-1]
            if ((p.x - q.x) ** 2 + (p.y - q.y) ** 2 + (p.z - q.z) ** 2) ** 0.5 < eps:
                continue
        out.append(p)
    return out


def write_spline(sp, pts):
    u"""照抄 fill_turn_splines.py 的写入顺序，两条铁律都在注释里。"""
    # mark_edited 必须在写点**之前**：它是 set_editor_property，而在组件上改属性
    # 会触发这个 actor 重跑构造脚本、组件整批重建——放最后调的话刚写进去的点
    # 正好落在被丢弃的那批上（名字变成 TRASH_SplineComponent_xxxx，回读全不对）。
    marked = mark_edited(sp)
    # modify() 也必须在改动之前：clear/add/update_spline 都只改内存、不标记已修改，
    # 保存时整个对象不被序列化（实测 8 条只活 1 条）。
    try:
        sp.modify(True)
        owner = sp.get_owner()
        if owner is not None:
            owner.modify(True)
    except Exception:
        pass
    sp.clear_spline_points(False)
    for p in dedup_pts(pts):
        sp.add_spline_point(p, WS, False)
    n = sp.get_number_of_spline_points()
    for i in range(n):
        # 全部 CURVE：接缝那两个点现在是中间点，CURVE_CLAMPED 会把局部极值
        # 处的切线归零，正是"折线样"的来源（CLAUDE.md 有实测）。
        sp.set_spline_point_type(i, unreal.SplinePointType.CURVE, False)
    sp.update_spline()
    if not marked:
        _mark_failed.append(sp.get_name())
    return n


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


def d2(p, q):
    return math.sqrt((p.x - q.x) ** 2 + (p.y - q.y) ** 2)


def stop_prop(a):
    u"""StopAtInter 的真实属性名。蓝图变量用原名，snake_case 只适用于原生属性。"""
    for n in ("StopAtInter", "stopatinter", "stop_at_inter"):
        try:
            a.get_editor_property(n)
            return n
        except Exception:
            continue
    return None


def box_aabb(a):
    u"""Box 的世界 AABB。Box 跟着行驶方向转过，所以逐轴外接（保守偏大）。"""
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
        u_ = unreal.MathLibrary.get_up_vector(rot)
        return center, unreal.Vector(
            abs(f.x) * hx + abs(r.x) * hy + abs(u_.x) * hz,
            abs(f.y) * hx + abs(r.y) * hy + abs(u_.y) * hz,
            abs(f.z) * hx + abs(r.z) * hy + abs(u_.z) * hz)
    return None


def overlaps(a_box, b_box):
    (ac, ah), (bc, bh) = a_box, b_box
    for axis in ("x", "y", "z"):
        if abs(getattr(ac, axis) - getattr(bc, axis)) > getattr(ah, axis) + getattr(bh, axis):
            return False
    return True


def is_stub(sp):
    u"""是不是"不算路径"的退化样条：只剩 1 点，或还是蓝图默认那根 ~100cm 小棍子。

    **这类东西不能算成"指望某个 Box 接棒的上游"。** 默认残桩方向一律朝世界 +X、
    和路的走向无关，撞巧落在某个段起点附近的概率不低——实测
    `+0180_I01` 的两根 100cm 残桩就正好落在 `-0180_S01` 起点 378cm 处、
    方向还对得上，于是检查 3 报出两个"上游"，而它们压根不是路径。
    判据和 repair_degenerate_splines.py 保持一致，免得两处对"残桩"的定义打架。
    """
    n = sp.get_number_of_spline_points()
    L = sp.get_spline_length()
    return n <= 1 or L < 1.0 or (n <= 2 and L <= STUB_LEN_MAX)


def dot_str(da, db):
    u"""切线点积 + 可读文本。

    **取不到切线和方向相反必须分开报。** 原来两种都显示 -1.00（兜底值就是 -1.0），
    于是单点退化样条看起来像"对向车道"，我据此得出过"点序反了"的错误结论。
    """
    if da is None or db is None:
        return None, u"切线取不到（样条退化）"
    v = da[0] * db[0] + da[1] * db[1]
    return v, u"dot %.2f" % v


def lane_key(label):
    u"""标签里"路名 + 偏移"那一段，用来找同路同偏移的兄弟段。

    不能用字面公共前缀：`Lane_...` 和 `Inter_...` 混在一条链里时公共前缀是**空串**，
    于是"兄弟段表"会把全图 934 条样条全打出来（实测报告 58KB）。
    """
    for pre in ("Lane_", "Inter_"):
        if label.startswith(pre):
            label = label[len(pre):]
            break
    return label[:label.rfind("_")] if "_" in label else label


def show_segment(a, lbl, sp, comps, inter_boxes):
    pts = pts_of(sp)
    w(u"%s" % lbl)
    w(u"    主样条 %d 点 / %.0f cm   起点 (%.0f, %.0f, %.0f)   终点 (%.0f, %.0f, %.0f)"
      % (len(pts), sp.get_spline_length(),
         pts[0].x, pts[0].y, pts[0].z, pts[-1].x, pts[-1].y, pts[-1].z))
    din, dout = dir_at(sp, True), dir_at(sp, False)
    w(u"    起点切线 %s   终点切线 %s"
      % (u"(%.2f, %.2f)" % din if din else u"零（退化）",
         u"(%.2f, %.2f)" % dout if dout else u"零（退化）"))
    for n in sorted(comps):
        if n in MAIN_KEYS:
            continue
        c = comps[n]
        # 转弯样条也要打坐标：主样条退化时，真正可用的路径就在这几条里，
        # 而"它通向哪里"只有首尾坐标能回答，点数和长度回答不了。
        if c.get_number_of_spline_points() >= 1:
            q = pts_of(c)
            w(u"    %-12s %d 点 / %.0f cm   起 (%.0f, %.0f, %.0f) → 终 (%.0f, %.0f, %.0f)"
              % (n, len(q), c.get_spline_length(),
                 q[0].x, q[0].y, q[0].z, q[-1].x, q[-1].y, q[-1].z))
        else:
            w(u"    %-12s 0 点" % n)
    sprop = stop_prop(a)
    w(u"    StopAtInter = %s" % (a.get_editor_property(sprop) if sprop else u"(读不到)"))
    bb = box_aabb(a)
    cover = [nm for nm, obb in inter_boxes if bb and overlaps(bb, obb)]
    w(u"    被路口盒罩住：%s" % (u"、".join(cover) if cover else u"无"))


def order_chain(items):
    u"""按几何推行驶顺序。items 是 [(label, spline)]，返回排好的 list 或 None。

    判据和交接判定同一套：A 的终点落在 B 的起点附近（≤ SEAM_MAX）且切线同向
    （dot > LINK_DOT）就连一条 A→B 的边。合法的链要求**每个点最多一个后继、
    最多一个前驱，且恰好有一个没有前驱的起点**，这样才唯一；否则宁可报错，
    不猜。顺手也拒绝环（三段首尾相接成环的话没有起点，下面的 while 自然走不满）。
    """
    n = len(items)
    nxt = {}
    for i in range(n):
        for j in range(n):
            if i == j:
                continue
            si, sj = items[i][1], items[j][1]
            e, st = pts_of(si)[-1], pts_of(sj)[0]
            di, dj = dir_at(si, False), dir_at(sj, True)
            if di is None or dj is None:
                continue          # 切线取不到 = 样条退化，连不了边，由诊断去报
            if d2(e, st) <= SEAM_MAX and (di[0] * dj[0] + di[1] * dj[1]) > LINK_DOT:
                if i in nxt:          # 一个段有两个后继 = 判据没能唯一确定
                    return None
                nxt[i] = j
    has_prev = set(nxt.values())
    heads = [i for i in range(n) if i not in has_prev]
    if len(heads) != 1:
        return None
    order, cur, seen = [], heads[0], set()
    while cur is not None and cur not in seen:
        seen.add(cur)
        order.append(cur)
        cur = nxt.get(cur)
    if len(order) != n:
        return None
    return [items[i] for i in order]


def main_spline(a):
    comps = {c.get_name(): c for c in a.get_components_by_class(unreal.SplineComponent)}
    for k in MAIN_KEYS:
        if k in comps:
            return comps[k], comps
    return (None, comps)


def do_chain(eas, chain_labels, idx):
    u"""处理一条链。索引**每条链各扫一次**，不复用：前一条链如果已经删过 actor，
    复用旧索引会让检查 3 把已经不存在的段当成上游报出来。"""
    w(u"")
    w(u"=" * 60)
    w(u"链 %d：%s" % (idx, u" / ".join(l.split("_")[-1] for l in chain_labels)))
    w(u"=" * 60)

    by_label, inter_boxes, all_splines = {}, [], []
    for a in eas.get_all_level_actors():
        lbl = a.get_actor_label()
        if lbl.startswith("BP_Intersection") or lbl.startswith("Intersection_"):
            bb = box_aabb(a)
            if bb:
                inter_boxes.append((lbl, bb))
            continue
        if not (lbl.startswith("Lane_") or lbl.startswith("Inter_")):
            continue
        if lbl in chain_labels:
            by_label[lbl] = a
        for c in a.get_components_by_class(unreal.SplineComponent):
            if c.get_number_of_spline_points() >= 2:
                all_splines.append((lbl, c.get_name(), c))

    missing = [l for l in chain_labels if l not in by_label]
    if missing:
        w(u"!! 关卡里找不到这些段，这条链什么都没做：")
        for l in missing:
            w(u"   %s" % l)
        return False
    w(u"路口盒 %d 个；全图可参与交接判定的样条 %d 条" % (len(inter_boxes), len(all_splines)))

    items = []
    for lbl in chain_labels:
        a = by_label[lbl]
        sp, comps = main_spline(a)
        if sp is None:
            w(u"!! %s 没有主样条组件（有的是 %s），这条链什么都没做"
              % (lbl, u"、".join(sorted(comps)) or u"无"))
            return False
        items.append((lbl, sp, a, comps))

    # 现状必须打在推顺序**之前**。上一版打在之后，于是排序失败时报告里
    # 只有两两接缝的矩阵、没有坐标，分不出是"方向反了"还是"这一段压根不在这条链上"。
    w(u"--- 现状 ---")
    for lbl, sp, a, comps in items:
        show_segment(a, lbl, sp, comps, inter_boxes)
    w(u"")

    # 同路同偏移的全部段。链里某一段够不上接缝时，真正的那一段通常就在这张表里
    # （段号不连续、或者它的点序是反的）。不打这张表只能靠猜名字。
    keys = {lane_key(l) for l in chain_labels}
    sibs = []
    for olbl, ocomp, oc in all_splines:
        if ocomp not in MAIN_KEYS or lane_key(olbl) not in keys:
            continue
        spts = pts_of(oc)
        sibs.append((olbl, spts[0], spts[-1], oc.get_spline_length(),
                     olbl in chain_labels))
    if sibs:
        w(u"--- 同路同偏移的全部段（%s）---" % u"、".join(sorted(keys)))
        for olbl, p0, p1, Ln, inchain in sorted(sibs):
            w(u"  %-46s %s  起 (%.0f, %.0f, %.0f) → 终 (%.0f, %.0f, %.0f)  %.0f cm"
              % (olbl, u"[链内]" if inchain else u"      ",
                 p0.x, p0.y, p0.z, p1.x, p1.y, p1.z, Ln))
        w(u"")

    # ---- 推行驶顺序 ----
    ordered = order_chain([(it[0], it[1]) for it in items])
    if ordered is None:
        w(u"")
        w(u"!! **排不出唯一的行驶顺序**，这条链不动。实测两两接缝：")
        for la, sa, _, _ in items:
            for lb, sb, _, _ in items:
                if la == lb:
                    continue
                e, st = pts_of(sa)[-1], pts_of(sb)[0]
                _, dtxt = dot_str(dir_at(sa, False), dir_at(sb, True))
                w(u"   %s 终点 → %s 起点：%.0f cm，%s"
                  % (la.split("_")[-1], lb.split("_")[-1], d2(e, st), dtxt))
        # 端对端也量一遍：位置贴着但 dot=-1，说明那一段的**点序是反的**，
        # 而不是"这两段不相邻"。两种原因要做的事完全不同，必须分得出来。
        w(u"   端对端（位置贴着 = 相邻，只是点序反了，不是不相邻）：")
        for la, sa, _, _ in items:
            for lb, sb, _, _ in items:
                if la >= lb:
                    continue
                w(u"   %s 终↔%s 终：%.0f cm      %s 起↔%s 起：%.0f cm"
                  % (la.split("_")[-1], lb.split("_")[-1],
                     d2(pts_of(sa)[-1], pts_of(sb)[-1]),
                     la.split("_")[-1], lb.split("_")[-1],
                     d2(pts_of(sa)[0], pts_of(sb)[0])))
        w(u"   三段本来就不首尾相接，或者其中一段的点序是反的（看上面的坐标和兄弟段表）。")
        return False

    pos = {lbl: k for k, (lbl, _) in enumerate(ordered)}
    chain = sorted(items, key=lambda it: pos[it[0]])
    chain = [(a, lbl, sp, comps) for (lbl, sp, a, comps) in chain]

    w(u"")
    w(u"推出的行驶顺序：")
    for i, (_, lbl, _, _) in enumerate(chain):
        w(u"  %d. %s%s" % (i, lbl, u"   ← 保留，合并写进它" if i == 0 else u"   ← 删除"))
    w(u"")


    # ---- 检查 1：接缝必须首尾相接且同向 ----
    w(u"--- 检查 1：接缝 ---")
    ok1 = True
    for i in range(len(chain) - 1):
        _, la, sa, _ = chain[i]
        _, lb, sb, _ = chain[i + 1]
        e, st = pts_of(sa)[-1], pts_of(sb)[0]
        gap = d2(e, st)
        dot, dtxt = dot_str(dir_at(sa, False), dir_at(sb, True))
        bad = (gap > SEAM_MAX) or (dot is None) or (dot <= LINK_DOT)
        ok1 = ok1 and not bad
        w(u"  %s 终点 → %s 起点：间距 %.0f cm（上限 %.0f）  %s（下限 %.2f）%s"
          % (la.split("_")[-1], lb.split("_")[-1], gap, SEAM_MAX, dtxt, LINK_DOT,
             u"   **不合格**" if bad else u"   合格"))
    w(u"")

    # ---- 检查 2：要删的段不能被别的路口盒罩着 ----
    w(u"--- 检查 2：要删的段是否属于别的路口 ---")
    ok2 = True
    for a, lbl, sp, comps in chain[1:]:
        bb = box_aabb(a)
        cover = [nm for nm, obb in inter_boxes if bb and overlaps(bb, obb)]
        extra = [nm for nm in cover if nm != TARGET_INTERSECTION]
        w(u"  %s 被罩住：%s" % (lbl, u"、".join(cover) if cover else u"无"))
        if len(cover) > 1:
            ok2 = False
            w(u"     **被 %d 个盒子同时罩住**，删掉会抽走别的路口的通路；"
              u"要么先修盒子，要么别删这一段" % len(cover))
        elif extra:
            w(u"     注意：罩它的不是 %s 而是 %s——"
              u"确认一下要降级的到底是哪个路口，以及这一段是不是真该删"
              % (TARGET_INTERSECTION, extra[0]))
    w(u"")

    # ---- 检查 3：要删的段的起点不能是别人的下家 ----
    w(u"--- 检查 3：有没有别的样条指望这些 Box 接棒 ---")
    ok3 = True
    for a, lbl, sp, comps in chain[1:]:
        s_pt, s_dir = pts_of(sp)[0], dir_at(sp, True)
        feeders, stubs = [], []
        for olbl, ocomp, oc in all_splines:
            if olbl in chain_labels:     # 链内部的交接正是我们要消掉的，不算
                continue
            e_pt, e_dir = pts_of(oc)[-1], dir_at(oc, False)
            if d2(e_pt, s_pt) >= LINK_DIST or e_dir is None or s_dir is None:
                continue
            if (e_dir[0] * s_dir[0] + e_dir[1] * s_dir[1]) <= LINK_DOT:
                continue
            tag = u"%s.%s（%.0f cm）" % (olbl, ocomp, d2(e_pt, s_pt))
            (stubs if is_stub(oc) else feeders).append(tag)
        w(u"  %s 起点的上游：%s" % (lbl, u"；".join(feeders) if feeders else u"无"))
        if stubs:
            # 列出来而不是静默跳过：静默过滤会让"为什么这里没报上游"变成无从查证的事。
            w(u"     忽略的蓝图默认残桩（不是路径，方向一律朝世界 +X）：%s"
              % u"；".join(stubs))
        if feeders:
            ok3 = False
            w(u"     **删掉之后上面这些就通到虚空了**。横向那条路降级后本来不该"
              u"再转进来，但那是 fill_turn_splines / NO_JUNCTION_WITH 那边的事，"
              u"本脚本不替它决定")
    w(u"")

    # ---- 合并后的样子 ----
    merged = []
    for _, _, sp, _ in chain:
        merged.extend(pts_of(sp))
    raw = sum(sp.get_number_of_spline_points() for _, _, sp, _ in chain)
    merged = dedup_pts(merged)
    w(u"--- 合并结果（预计） ---")
    w(u"  %d 点（去重前 %d），首 (%.0f, %.0f, %.0f) → 尾 (%.0f, %.0f, %.0f)"
      % (len(merged), raw, merged[0].x, merged[0].y, merged[0].z,
         merged[-1].x, merged[-1].y, merged[-1].z))
    w(u"  入口 Box 不动（链首保留），出口终点不变（和链尾原终点同一个点）")

    # 下游还在不在：合并不改终点，所以这只是确认，不是检查
    e_pt, e_dir = merged[-1], dir_at(chain[-1][2], False)
    downs = []
    for olbl, ocomp, oc in all_splines:
        if olbl in chain_labels:
            continue
        s_pt, s_dir = pts_of(oc)[0], dir_at(oc, True)
        if d2(s_pt, e_pt) >= LINK_DIST or s_dir is None or e_dir is None:
            continue
        if (s_dir[0] * e_dir[0] + s_dir[1] * e_dir[1]) > LINK_DOT:
            downs.append(u"%s.%s（%.0f cm）" % (olbl, ocomp, d2(s_pt, e_pt)))
    w(u"  出口下家：%s"
      % (u"；".join(downs) if downs else u"无（车开到头会裸奔，但这不是合并造成的）"))
    w(u"")

    if not (ok1 and ok2 and ok3):
        w(u"=== 有检查不合格，这条链**不执行写入** ===")
        w(u"检查 1 接缝 %s / 检查 2 路口归属 %s / 检查 3 上游 %s"
          % (u"过" if ok1 else u"**不过**", u"过" if ok2 else u"**不过**",
             u"过" if ok3 else u"**不过**"))
        return False

    if DRY_RUN:
        w(u"=== 三道检查全过。DRY_RUN=True，没有写入 ===")
        return True

    # ---- 写入 ----
    keep_a, keep_lbl, keep_sp, keep_comps = chain[0]
    n = write_spline(keep_sp, merged)
    w(u"已写 %s 主样条：%d 点 / %.0f cm" % (keep_lbl, n, keep_sp.get_spline_length()))

    # 用不着的转弯**写成和直行一模一样**（项目既有惯例）：车探到一个 Box 拿到
    # 三条候选、随机选一条，留着旧的短转弯等于留一条会把车甩走的路径。
    for nm in sorted(keep_comps):
        if nm in MAIN_KEYS:
            continue
        m = write_spline(keep_comps[nm], merged)
        w(u"已把 %s.%s 写成直行副本：%d 点" % (keep_lbl, nm, m))

    # StopAtInter 必须显式写 false：盒子删了之后没人再刷它，
    # 残留的 True 会让车永远等绿灯（最初 I08 卡车那条机制）。
    sprop = stop_prop(keep_a)
    if sprop:
        try:
            keep_a.modify(True)
            keep_a.set_editor_property(sprop, False)
            w(u"已把 %s 的 %s 写成 False（回读 %s）"
              % (keep_lbl, sprop, keep_a.get_editor_property(sprop)))
        except Exception as exc:
            w(u"!! 写 StopAtInter 失败：%s —— **这条必须成功**，否则车会永远等绿灯" % exc)
    else:
        w(u"!! 读不到 StopAtInter 属性名，没能写 False —— 手动确认一下")

    for a, lbl, _, _ in chain[1:]:
        try:
            ok = eas.destroy_actor(a)
            w(u"已删 %s：%s" % (lbl, u"成功" if ok else u"**失败**"))
        except Exception as exc:
            w(u"!! 删 %s 失败：%s" % (lbl, exc))
    return True


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    w(u"=== 车道链合并（每条链拼成一条连续车道）===")
    w(u"关卡：%s     模式：%s     共 %d 条链"
      % (world.get_name() if world else u"?",
         u"只报不改" if DRY_RUN else u"**写入**", len(CHAINS)))

    done = 0
    for i, labels in enumerate(CHAINS):
        if do_chain(eas, labels, i):
            done += 1

    w(u"")
    w(u"=" * 60)
    w(u"%d / %d 条链%s" % (done, len(CHAINS),
                          u"检查通过（未写入）" if DRY_RUN else u"已合并"))
    if _mark_failed:
        w(u"!! 这些样条 mark_edited 没成功，存盘可能留不住：%s" % u"、".join(_mark_failed))
    if DRY_RUN:
        w(u"确认报告无误后把 DRY_RUN 改成 False 再跑一次。")
        return
    w(u"")
    w(u"=== 还要做的三件事 ===")
    w(u"1. 路口盒 %s 本身还在。跑 drop_intersection.py（TARGET_INTERSECTIONS "
      u"填 %s）把它删掉，顺带把横向那条路的 Inter_ 段的 StopAtInter 刷成 false。"
      % (TARGET_INTERSECTION, TARGET_INTERSECTION))
    w(u"2. 全图重建会让这个路口原样长回来。要复现得在 gen_traffic_lanes.py 的"
      u" NO_JUNCTION_WITH 里加上横向那条路；合并本身属于"
      u" Design/TrafficManualEdits.json 现在还表达不了的改动（它只记"
      u"「路口三条样条的重合关系」，不记 actor 删除 + 样条拼接）。")
    w(u"3. 别忘了在编辑器里 Ctrl+S 存盘。")


try:
    run()
except Exception:
    w(u"!! 脚本异常：")
    w(traceback.format_exc())
flush()
w(u"报告：%s" % OUT)
