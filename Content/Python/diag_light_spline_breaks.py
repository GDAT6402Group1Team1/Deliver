# -*- coding: utf-8 -*-
"""用 light_spline 的断点当独立标答，找出被漏判的路口。只读。

思路（用户提的，很好用）
------------------------
`light_spline_*` 是从每条 `Lane_` 外偏 410cm 生成的路灯路径，采样步长 200cm，
随后 `cut_light_splines_at_roads.py` 用 `RoadQuery.on_road(p)` **按真实路面几何**
把落在路面上的点删掉。所以"路灯线中间缺一段"= 那里有路横穿。

**关键区分**：段与段之间（S08 / S09 之间）的断开只是 `Lane_` 本来就断开，
反映的是**已经判出来**的路口，没有新信息。有价值的是**同一个 actor 内部**
被删掉的那一截——它是按路面实测的，会出现在"确实有路横穿、但当初
XY_CROSS 没判成路口"的位置，正是 T 形漏判的地方。

判据：同一条样条内相邻两点间距 > BREAK_MIN（默认 600 = 3 倍采样步长）。

然后逐个候选位置查附近有没有 `Inter_` 段：
  有  → 这个路口已经有交通线，正常
  没有 → **漏判**，这些就是要让检测规则捡回来的目标

只读。结果写到 Saved/diag_light_spline_breaks.txt
用法：py diag_light_spline_breaks.py
"""

import re
import traceback

import unreal

LIGHT_PREFIX = "light_spline_"
INTER_PREFIX = "Inter_"
SAMPLE_STEP = 200.0      # gen_light_splines.py 的采样步长
BREAK_MIN = 600.0        # 相邻点间距超过这个值算"中间被删过"
CLUSTER_DIST = 2500.0    # 候选点聚类 / 判定附近有没有 Inter_（和 gen_intersections 一致）

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "diag_light_spline_breaks.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[lsbreak] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def d2(a, b):
    return ((a.x - b.x) ** 2 + (a.y - b.y) ** 2) ** 0.5


def road_of(label):
    """light_spline_MainRoad_R010_-0540_S08 -> MainRoad_R010"""
    m = re.match(r'^light_spline_(.+)_([+-]\d+)_S(\d+)$', label)
    return m.group(1) if m else label[len(LIGHT_PREFIX):]


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()
    w(u"=== 用 light_spline 断点找漏判的路口（只读）===")
    w(u"关卡：%s" % (world.get_name() if world else u"?"))

    lights, inters = [], []
    for a in actors:
        lbl = a.get_actor_label()
        if lbl.startswith(LIGHT_PREFIX):
            sp = a.get_component_by_class(unreal.SplineComponent)
            if sp and sp.get_number_of_spline_points() >= 2:
                lights.append((lbl, sp))
        elif lbl.startswith(INTER_PREFIX):
            inters.append(a)
    w(u"light_spline %d 条、Inter_ 段 %d 个" % (len(lights), len(inters)))
    if not lights:
        w(u"！没有 light_spline。先跑 gen_light_splines.py，或者标签前缀变了。")
        return

    # ---- 找内部断点 ----
    cands = []
    spacing_all = []
    for lbl, sp in lights:
        n = sp.get_number_of_spline_points()
        pts = [sp.get_location_at_spline_point(i, WS) for i in range(n)]
        for i in range(n - 1):
            gap = d2(pts[i], pts[i + 1])
            spacing_all.append(gap)
            if gap > BREAK_MIN:
                mid = unreal.Vector((pts[i].x + pts[i + 1].x) / 2.0,
                                    (pts[i].y + pts[i + 1].y) / 2.0,
                                    (pts[i].z + pts[i + 1].z) / 2.0)
                cands.append((mid, gap, lbl))

    if spacing_all:
        spacing_all.sort()
        w(u"相邻点间距：中位数 %.0f（采样步长 %.0f）  最大 %.0f"
          % (spacing_all[len(spacing_all) // 2], SAMPLE_STEP, spacing_all[-1]))
    w(u"内部断点（间距 > %.0f）共 %d 处" % (BREAK_MIN, len(cands)))
    # ---- 没有内部缺口时，改用 _CutNN 兄弟之间的缺口 ----
    # cut_light_splines_at_roads.py 不是在一条样条里留洞，而是**拆成多个 actor**，
    # 多出来的命名为 <原名>_Cut01 / _Cut02（main 里 base = label + '_Cut%02d'）。
    # 所以真正的信号是同一条路灯线被拆开的那些兄弟之间的缺口。
    if not cands:
        w(u"")
        w(u"没有内部缺口——切路面那步不是留洞，是拆 actor。改查 _CutNN 兄弟。")
        groups = {}
        for lbl, sp in lights:
            base = re.sub(r"_Cut\d+$", "", lbl)
            groups.setdefault(base, []).append((lbl, sp))
        cut_groups = {k: v for k, v in groups.items() if len(v) > 1}
        w(u"被拆开过的路灯线 %d 条（共 %d 条）" % (len(cut_groups), len(groups)))
        if not cut_groups:
            w(u"")
            w(u"一条都没被拆过 → **cut_light_splines_at_roads.py 没真正跑过**")
            w(u"（或者跑的是 DRY_RUN）。这个标答信号现在拿不到。")
            w(u"不过阈值已经能从缺口分布里定（见 preview_endpoint_junctions.py 的")
            w(u"直方图：900~1000 整档为空），标答只是用来二次确认，不是必需。")
            return
        for base, parts in sorted(cut_groups.items()):
            ends = []
            for lbl, sp in parts:
                n = sp.get_number_of_spline_points()
                ends.append((lbl,
                             sp.get_location_at_spline_point(0, WS),
                             sp.get_location_at_spline_point(n - 1, WS)))
            # 两两取最近的端点对，就是被切掉的那一截
            best = None
            for i in range(len(ends)):
                for j in range(i + 1, len(ends)):
                    for a_pt in ends[i][1:]:
                        for b_pt in ends[j][1:]:
                            g = d2(a_pt, b_pt)
                            if best is None or g < best[0]:
                                best = (g, a_pt, b_pt)
            if best is None:
                continue
            g, a_pt, b_pt = best
            mid = unreal.Vector((a_pt.x + b_pt.x) / 2.0,
                                (a_pt.y + b_pt.y) / 2.0,
                                (a_pt.z + b_pt.z) / 2.0)
            cands.append((mid, g, base))
        w(u"从 _CutNN 兄弟取到 %d 处缺口" % len(cands))
        if not cands:
            return

    # ---- 聚类成候选路口 ----
    clusters = []
    for mid, gap, lbl in cands:
        got = None
        for c in clusters:
            if d2(c["center"], mid) < CLUSTER_DIST:
                got = c
                break
        if got is None:
            clusters.append({"center": mid, "items": [(mid, gap, lbl)]})
        else:
            got["items"].append((mid, gap, lbl))
            k = len(got["items"])
            got["center"] = unreal.Vector(
                sum(m.x for m, _g, _l in got["items"]) / k,
                sum(m.y for m, _g, _l in got["items"]) / k,
                sum(m.z for m, _g, _l in got["items"]) / k)
    w(u"聚类成 %d 个候选位置" % len(clusters))

    have, missing = [], []
    for c in clusters:
        near = [a for a in inters
                if d2(a.get_actor_location(), c["center"]) < CLUSTER_DIST]
        roads = sorted({road_of(l) for _m, _g, l in c["items"]})
        row = (c["center"], len(c["items"]), roads, near)
        (have if near else missing).append(row)

    w(u"")
    w(u"=" * 76)
    w(u"已经有交通线的路口：%d 个（正常，不用管）" % len(have))
    w(u"**附近没有任何 Inter_ 段的：%d 个 ← 这些是漏判的路口**" % len(missing))
    w(u"=" * 76)
    w(u"%-26s %-6s %s" % (u"位置", u"断点数", u"涉及的路（按路灯线名解析）"))
    for center, cnt, roads, _near in sorted(missing, key=lambda r: -r[1]):
        w(u"(%7.0f,%8.0f) %-6d %s" % (center.x, center.y, cnt,
                                      u" + ".join(roads)[:46]))

    w(u"")
    w(u"=" * 76)
    w(u"怎么用")
    w(u"=" * 76)
    w(u"  · 上面「漏判」那张表就是检测规则该捡回来的目标清单。")
    w(u"    拿它和 preview_endpoint_junctions.py 的新增命中逐个对位置：")
    w(u"      都覆盖到了 → 端点规则的阈值够用，可以跑生成链")
    w(u"      还差一些   → 那几处的缺口比 ENDPOINT_CROSS 还大，按差额调")
    w(u"      多出来一堆不在这张表里的 → 阈值放太宽了，调小")
    w(u"  · 只涉及**一条路**的漏判位置要留意：可能那里确实只有一条路经过")
    w(u"    （路灯线被别的东西切了，比如建筑或广场），不是路口。")
    w(u"  · 这个信号的局限：它反映的是「路灯线被路面切过」，而路灯线只沿着")
    w(u"    **有车道的路**生成——交叉的那条路如果本身没铺车道，它照样会切断")
    w(u"    路灯线，但那种地方生成交通线也没有意义（三条路全是直行副本）。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[lsbreak] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[lsbreak] 写入 %s" % OUT)
