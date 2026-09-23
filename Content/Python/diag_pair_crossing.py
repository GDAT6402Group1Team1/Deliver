# -*- coding: utf-8 -*-
"""查两条指定道路为什么没被判成路口（或判成了几个）。

起因：形状1 的路口清单里有形状13（沿线 209750），但形状13 自己的清单里
只有一次形状1。结果那个位置（Intersection_28，约 (3094, -17726)）只有形状1
切出了路口段，形状13 直接穿过去，转弯没法生成。

生成器的判据是三条同时成立（gen_traffic_lanes.find_intersections）：
    XY 最近距离 < XY_CROSS(200)
    |Z 差|      < Z_CROSS(600)      超过视为立交
    相距 < MERGE_DIST(2500) 的会被合并成一个路口
这里把两条路**双向**扫一遍，把每个局部最近点的三项数值都列出来，
看到底是哪一条卡住了——是 XY 差一点点，还是 Z 差过大被当成立交。

按同样的步长 DETECT_STEP 采样，和生成器一致，结论才可迁移。

只读，什么都不改。
用法：py diag_pair_crossing.py
"""

import traceback

import unreal

ROAD_A = "形状 13"
ROAD_B = "形状 1"
ROAD_CLASS = "BP_PS2DEMSplineActor_v3_C"

DETECT_STEP = 250.0      # 和 gen_traffic_lanes 一致
XY_CROSS = 200.0         # 生成器当前阈值
Z_CROSS = 600.0
MERGE_DIST = 2500.0
REPORT_XY = 4000.0       # 报告里列出 XY 小于这个值的局部最近点。
                         # 要比 XY_CROSS 宽得多，否则"差得有点远"的接头
                         # 根本不出现在表里，看着像两条路完全无关。

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "pair_crossing.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def scan(name_a, sp_a, name_b, sp_b):
    """沿 A 采样，报出所有"局部最近"的位置及三项判据。"""
    total = sp_a.get_spline_length()
    rows = []
    d = 0.0
    while d <= total:
        p = sp_a.get_location_at_distance_along_spline(d, WS)
        q = sp_b.find_location_closest_to_world_location(p, WS)
        dxy = ((q.x - p.x) ** 2 + (q.y - p.y) ** 2) ** 0.5
        dz = q.z - p.z
        rows.append((d, dxy, dz, p, q))
        d += DETECT_STEP

    # 找局部极小值：比左右邻居都近，且够近到值得报告
    mins = []
    for i in range(1, len(rows) - 1):
        if (rows[i][1] <= rows[i - 1][1] and rows[i][1] <= rows[i + 1][1]
                and rows[i][1] < REPORT_XY):
            mins.append(rows[i])
    # 相距很近的极小值合并，只留最近的那个
    merged = []
    for r in mins:
        if merged and abs(r[0] - merged[-1][0]) < MERGE_DIST:
            if r[1] < merged[-1][1]:
                merged[-1] = r
            continue
        merged.append(r)

    w("=" * 104)
    w("沿 %s 扫，找 %s（步长 %.0f，全长 %.0f）" % (name_a, name_b, DETECT_STEP, total))
    w("=" * 104)
    if not merged:
        w("  没有任何位置的 XY 距离小于 %.0f。两条路根本不靠近。" % REPORT_XY)
        return []
    w("%10s %10s %10s  %-26s %s"
      % ("沿线", "XY距离", "Z差", "本路上的点", "判定"))
    w("-" * 104)
    hits = []
    for d, dxy, dz, p, q in merged:
        ok_xy = dxy < XY_CROSS
        ok_z = abs(dz) < Z_CROSS
        if ok_xy and ok_z:
            verdict = "算路口"
            hits.append(d)
        elif not ok_xy and ok_z:
            verdict = "!! XY 差 %.0f 就够了（阈值 %.0f）" % (dxy - XY_CROSS, XY_CROSS)
        elif ok_xy and not ok_z:
            verdict = "!! 被当成立交（|Z差| %.0f > %.0f）" % (abs(dz), Z_CROSS)
        else:
            verdict = "XY 和 Z 都不满足"
        w("%10.0f %10.0f %+10.0f  (%7.0f,%8.0f)  %s"
          % (d, dxy, dz, p.x, p.y, verdict))
    return hits


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    roads = {}
    for a in eas.get_all_level_actors():
        if a.get_class().get_name() != ROAD_CLASS:
            continue
        sp = a.get_component_by_class(unreal.SplineComponent)
        if sp is not None and sp.get_number_of_spline_points() >= 2:
            lbl = a.get_actor_label()
            roads.setdefault(lbl, []).append(sp)

    w("关卡道路样条 %d 条" % sum(len(v) for v in roads.values()))
    for nm in (ROAD_A, ROAD_B):
        n = len(roads.get(nm, []))
        if n == 0:
            w("!! 找不到 %s。名字相近的：%s" % (
                nm, "、".join(k for k in roads if nm.replace(" ", "") in
                              k.replace(" ", ""))[:200] or "（没有）"))
        elif n > 1:
            # 同名 actor 会让 actor_by_label 那类字典互相覆盖，检测只认其中一条
            w("!! %s 有 %d 个同名 actor——生成器按 label 建字典，只会认其中一条，"
              % (nm, n))
            w("   这本身就足以让某个方向的路口检测漏掉。")
    w("")
    flush()
    if ROAD_A not in roads or ROAD_B not in roads:
        flush()
        return

    sa, sb = roads[ROAD_A][0], roads[ROAD_B][0]
    hits_ab = scan(ROAD_A, sa, ROAD_B, sb)
    w("")
    hits_ba = scan(ROAD_B, sb, ROAD_A, sa)

    w("")
    w("=" * 104)
    w("结论")
    w("=" * 104)
    w("%s 认出 %d 个路口，%s 认出 %d 个" % (ROAD_A, len(hits_ab), ROAD_B, len(hits_ba)))
    if len(hits_ab) != len(hits_ba):
        w("!! 两个方向数量不一致——这正是问题所在。只有一侧切出路口段，")
        w("   另一侧直接穿过去，那个路口就没有转弯、车也接不上。")
        w("   上面的表里写明了失败的那一侧卡在哪一条判据上。")
    else:
        w("两个方向一致。若仍然缺路口段，问题不在相交检测，要往下游查。")
    w("")
    w("能调的旋钮（都在 gen_traffic_lanes.py 顶部）：")
    w("  XY_CROSS  当前 %.0f —— 差一点点没够到就调大它。" % XY_CROSS)
    w("            代价：调太大会把「擦肩而过但不相交」的两条路也判成路口。")
    w("  Z_CROSS   当前 %.0f —— 被误判成立交就调大它。" % Z_CROSS)
    w("            代价：真立交会被切断，桥上桥下各生成一套路口段。")
    w("只读脚本，没有改动任何东西。")
    flush()
    unreal.log("[pair] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[pair] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
