# -*- coding: utf-8 -*-
"""查「路被切成 _PartNN 的那些切点，是不是路口；如果是，现在判不判得出来」。只读。

为什么要单独查这个：被切开的 5 条路（R024/R035/R036/R038/R039）兄弟段之间
缺口实测 629~1889cm，而主路标称宽 1800、次路 1080——**缺口 ≈ 横穿那条路的宽度**。
也就是说路大概率就是**在路口处被切开的**，横穿的那条路正从缺口中间过。

那么从 Part01 的端点到横穿路的中心线约是「半个缺口」= 314~944cm，
而 XY_CROSS 现在是 400 —— 除了最小那个 629（半缺口 314），
**其余切点处的路口应该全都判不出来**。

上一轮的 diag_junction_miss.py 没报出这件事，是因为它对标旧路口时
用了 2500cm 的匹配半径，附近任何一对路命中就算「判出来了」，把这种漏判盖住了。
这个脚本不走对标，直接量：

  对每一对兄弟段：
    1. 算出两段端点之间的缺口中点（= 推测的路口位置）
    2. 列出从那附近经过的**别的逻辑路**，给出它到两段端点各自的距离
    3. 标出按当前 XY_CROSS 判不判得出来

据此决定怎么改：如果缺口都发生在**端点**上，那正确做法是
「最近点落在样条端点附近时用一个放宽的阈值」——
端点停在另一条路旁边是「路被切开」的结构特征，
而两条平行路贴得近是发生在样条**内部**的，两者能分开。
这比把 XY_CROSS 一刀调大安全得多：实测距离分布是连续的
（…185、199、200、210、244…342、351…），没有空隙可卡，
一刀调大会把并行的路对也判成路口，表现是「车在直路中间莫名停下等红灯」。

中文里一律用「」不用半角引号。
用法：py diag_part_junctions.py
"""

import re
import traceback

import unreal

ROAD_FOLDERS = ["PS2DEM/Splines/MainRoad", "PS2DEM/Splines/BranchRoad"]
WS = unreal.SplineCoordinateSpace.WORLD

XY_CROSS = 400.0          # 和 gen_traffic_lanes.py 当前值一致
Z_CROSS = 600.0
NEAR = 2500.0             # 缺口中点附近多大范围内算「从这儿经过」

OUT = unreal.Paths.project_saved_dir() + "diag_part_junctions.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def base_road_name(label):
    return re.sub(r"_[Pp]art\d+$", "", str(label))


def collect():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    out = []
    for a in eas.get_all_level_actors():
        if not a:
            continue
        try:
            folder = str(a.get_folder_path()).replace("\\", "/").lower()
        except Exception:
            continue
        if not any(f.lower() in folder for f in ROAD_FOLDERS):
            continue
        try:
            sp = a.get_component_by_class(unreal.SplineComponent)
        except Exception:
            sp = None
        if sp:
            out.append((a.get_actor_label(), sp))
    return out


def ends(sp):
    L = sp.get_spline_length()
    return (sp.get_location_at_distance_along_spline(0.0, WS),
            sp.get_location_at_distance_along_spline(L, WS))


def dist2d(a, b):
    return ((a.x - b.x) ** 2 + (a.y - b.y) ** 2) ** 0.5


def main():
    roads = collect()
    w("道路样条 %d 条" % len(roads))
    by_base = {}
    for label, sp in roads:
        by_base.setdefault(base_road_name(label), []).append((label, sp))
    split = {k: v for k, v in by_base.items() if len(v) > 1}
    w("被切成多段的逻辑路 %d 条" % len(split))

    for base in sorted(split):
        parts = sorted(split[base])
        w()
        w("=" * 70)
        w("%s  （%d 段）" % (base, len(parts)))
        w("=" * 70)

        # 两两找「最近的那一对端点」= 切点
        for i in range(len(parts)):
            for j in range(i + 1, len(parts)):
                la, spa = parts[i]
                lb, spb = parts[j]
                ea = ends(spa)
                eb = ends(spb)
                best = None
                for pa in ea:
                    for pb in eb:
                        d = dist2d(pa, pb)
                        if best is None or d < best[0]:
                            best = (d, pa, pb)
                gap, pa, pb = best
                mid = unreal.Vector((pa.x + pb.x) / 2.0,
                                    (pa.y + pb.y) / 2.0,
                                    (pa.z + pb.z) / 2.0)
                w()
                w("  %s ←→ %s" % (la, lb))
                w("    端点缺口 %.0f cm，缺口中点 (%.0f, %.0f)" % (gap, mid.x, mid.y))

                # 谁从这个中点附近过
                others = []
                for name, osp in roads:
                    if base_road_name(name) == base:
                        continue
                    q = osp.find_location_closest_to_world_location(mid, WS)
                    dmid = dist2d(q, mid)
                    if dmid > NEAR:
                        continue
                    qa = osp.find_location_closest_to_world_location(pa, WS)
                    qb = osp.find_location_closest_to_world_location(pb, WS)
                    others.append((dmid, name,
                                   dist2d(qa, pa), abs(qa.z - pa.z),
                                   dist2d(qb, pb), abs(qb.z - pb.z)))
                if not others:
                    w("    附近 %.0f cm 内没有别的路经过 —— 这个切点**不是路口**，"
                      % NEAR)
                    w("    那车道会在这儿断开（缺口 %.0f 远大于 TARGET_GAP 150），" % gap)
                    w("    需要的是把两段当一条路连续铺车道，而不是加路口。")
                    continue

                for dmid, name, da, dza, db, dzb in sorted(others):
                    ok_a = da < XY_CROSS and dza < Z_CROSS
                    ok_b = db < XY_CROSS and dzb < Z_CROSS
                    w("    横穿: %-28s 到中点 %6.0f" % (name, dmid))
                    w("        到 %-26s 端点 %6.0f (dz %4.0f) %s"
                      % (la, da, dza, "判得出" if ok_a else "**判不出**"))
                    w("        到 %-26s 端点 %6.0f (dz %4.0f) %s"
                      % (lb, db, dzb, "判得出" if ok_b else "**判不出**"))

    w()
    w("=" * 70)
    w("怎么用这份报告")
    w("=" * 70)
    w("1. 「横穿」那几行里 **判不出** 的距离值，就是端点放宽阈值要盖住的量。")
    w("   取其中最大值 + 余量，但要低于「不同逻辑路、发生在样条内部的最近接近距离」")
    w("   的下界，否则并行的路会被误判。后者跑 diag_junction_miss.py 看分布。")
    w("2. 出现「附近没有别的路经过」的切点，是另一类问题：那不是路口，")
    w("   而是一条路被切成两段，中间需要把车道连续铺过去（桥接），")
    w("   加路口是错的——车会在直路中间停下等一个不存在的灯。")
    w("3. 端点放宽只在「最近点落在样条端点附近」时生效，所以不会影响")
    w("   两条路在中段贴近的那些路对。")


try:
    main()
except Exception:
    w("抛异常：")
    w(traceback.format_exc())

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

print("\n".join(lines))
print()
print("报告：" + OUT)
