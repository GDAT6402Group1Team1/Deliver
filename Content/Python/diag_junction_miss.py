# -*- coding: utf-8 -*-
"""查「队友把一条路切成几段之后，路口判不出来」到底差多少。只读。

背景：道路从 38 条 形状xx 换成 45 条 MainRoad_*/BranchRoad_*，而且
**原来一条的路被切成了几段**（例如 BranchRoad_R035_Part01 / _Part02）。
原来支路是"穿过"主路、采样点能在主路上找到 <120cm 的近点；
切开之后支路是"停在"主路外面，缺口一旦超过 XY_CROSS（200cm）就判不出路口。

不靠猜阈值，先量三件事：

  1. **用旧路口当标答**。Saved/gen_intersections.txt 里有改路前聚出来的
     47 个物理路口中心坐标（那是按旧车道 Box 聚的，位置就是真实路口）。
     逐个检查：新几何下那附近有没有任何一对道路被判成路口。
  2. **判不出来的，报实测缺口**。列出那个位置附近经过的道路，两两之间
     最近距离是多少、是不是发生在端点上。要把 XY_CROSS 放到多少才够，
     看这个数就知道，不用试。
  3. **_PartNN 兄弟段之间的缺口单独列**。这些**不能**算路口——它们是
     同一条路被切开，两段之间本来就该连起来走。放宽阈值时如果不排除它们，
     每个切点都会多出一个假路口，车到那儿会当成要选路。

同时把所有道路对的"最近接近距离"分布打出来：如果"真路口"和"只是并行经过"
之间有明显的数值空隙，阈值就能卡在空隙里，不用担心放宽之后误判。

中文里一律用「」不用半角引号。
用法：py diag_junction_miss.py
"""

import re
import traceback

import unreal

OLD_REPORT = unreal.Paths.project_saved_dir() + "gen_intersections.txt"
OUT = unreal.Paths.project_saved_dir() + "diag_junction_miss.txt"

ROAD_FOLDERS = ["PS2DEM/Splines/MainRoad", "PS2DEM/Splines/BranchRoad"]
WS = unreal.SplineCoordinateSpace.WORLD

# 和 gen_traffic_lanes.py 保持一致
DETECT_STEP = 250.0
XY_CROSS = 200.0
Z_CROSS = 600.0
MATCH_RADIUS = 2500.0     # 旧路口中心附近多大范围内算"判出来了"（= MERGE_DIST）
NEAR_ROAD = 3000.0        # 列"这个路口附近经过哪些路"的范围
ENDPOINT_EPS = 400.0      # 最近点离样条端点多近算"发生在端点上"

lines = []


def w(s=""):
    lines.append(str(s))


def base_name(label):
    """去掉 _PartNN 后缀，得到逻辑路名。

    BranchRoad_R035_Part01 和 _Part02 是同一条路被切开的两段，
    它们之间不该算路口。
    """
    return re.sub(r"_[Pp]art\d+$", "", str(label))


def collect_roads():
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


def read_old_centers():
    """从旧报告里读出路口中心坐标，形如 [00] (-33941, 29003)  半尺寸 ..."""
    centers = []
    try:
        with open(OLD_REPORT, "r", encoding="utf-8") as f:
            text = f.read()
    except Exception as exc:
        w("读不到旧报告 %s：%s" % (OLD_REPORT, exc))
        return centers
    for m in re.finditer(r"\[(\d+)\]\s*\(\s*(-?\d+)\s*,\s*(-?\d+)\s*\)", text):
        centers.append((int(m.group(1)), float(m.group(2)), float(m.group(3))))
    return centers


def closest_between(sp_a, sp_b):
    """沿 A 采样、在 B 上找最近点。返回 (最小 xy 距离, 位置, dz, 是否在端点)。"""
    total = sp_a.get_spline_length()
    best = None
    d = 0.0
    while d <= total:
        p = sp_a.get_location_at_distance_along_spline(d, WS)
        q = sp_b.find_location_closest_to_world_location(p, WS)
        dxy = ((q.x - p.x) ** 2 + (q.y - p.y) ** 2) ** 0.5
        if best is None or dxy < best[0]:
            # 这个最近点是不是落在 B 的端点附近——切开留下的缺口都在端点上
            lb = sp_b.get_spline_length()
            e0 = sp_b.get_location_at_distance_along_spline(0.0, WS)
            e1 = sp_b.get_location_at_distance_along_spline(lb, WS)
            d0 = ((q.x - e0.x) ** 2 + (q.y - e0.y) ** 2) ** 0.5
            d1 = ((q.x - e1.x) ** 2 + (q.y - e1.y) ** 2) ** 0.5
            at_end = min(d0, d1) < ENDPOINT_EPS
            best = (dxy, p, abs(q.z - p.z), at_end)
        d += DETECT_STEP
    return best


def main():
    roads = collect_roads()
    w("道路样条 %d 条（按文件夹找）" % len(roads))
    if not roads:
        w("一条都没找到，先确认 ROAD_FOLDERS。")
        return

    # 逻辑路名分组，顺便看看哪些路被切成了几段
    groups = {}
    for label, sp in roads:
        groups.setdefault(base_name(label), []).append(label)
    split = {k: v for k, v in groups.items() if len(v) > 1}
    w("逻辑路 %d 条；其中被切成多段的 %d 条" % (len(groups), len(split)))
    for k in sorted(split):
        w("    %-28s -> %s" % (k, "、".join(sorted(split[k]))))

    # ---------------------------------------------------------------- 当前判定
    w()
    w("=" * 70)
    w("1. 按当前阈值（XY_CROSS=%.0f）能判出来的路口" % XY_CROSS)
    w("=" * 70)
    hits = []          # (x, y, a, b, dxy)
    pair_min = []      # 所有路对的最近接近距离，用来看阈值该卡哪
    for i in range(len(roads)):
        for j in range(i + 1, len(roads)):
            la, spa = roads[i]
            lb, spb = roads[j]
            same_road = base_name(la) == base_name(lb)
            best = closest_between(spa, spb)
            if not best:
                continue
            dxy, p, dz, at_end = best
            pair_min.append((dxy, la, lb, p.x, p.y, dz, at_end, same_road))
            if dxy < XY_CROSS and dz < Z_CROSS and not same_road:
                hits.append((p.x, p.y, la, lb, dxy))
    w("判出 %d 个路口（未聚类，同一处可能被多对路各报一次）" % len(hits))

    # ---------------------------------------------------------------- 对标旧路口
    old = read_old_centers()
    w()
    w("=" * 70)
    w("2. 和改路前的 %d 个路口对标（半径 %.0f cm 内算判出来了）" % (len(old), MATCH_RADIUS))
    w("=" * 70)
    missed = []
    for idx, ox, oy in old:
        near = [h for h in hits
                if ((h[0] - ox) ** 2 + (h[1] - oy) ** 2) ** 0.5 < MATCH_RADIUS]
        if near:
            continue
        missed.append((idx, ox, oy))
    w("判出 %d / %d；**漏了 %d 个**" % (len(old) - len(missed), len(old), len(missed)))

    # ---------------------------------------------------------------- 漏掉的差多少
    w()
    w("=" * 70)
    w("3. 漏掉的路口：附近经过哪些路、实测缺口多少")
    w("=" * 70)
    w("（看 at_end=是 的那些——切开留下的缺口都发生在端点上；")
    w(" same_road=是 表示是同一条路的两段，**不该**算路口）")
    need = []
    for idx, ox, oy in missed:
        w()
        w("--- 旧路口 [%02d] (%.0f, %.0f) ---" % (idx, ox, oy))
        cands = [pm for pm in pair_min
                 if ((pm[3] - ox) ** 2 + (pm[4] - oy) ** 2) ** 0.5 < NEAR_ROAD]
        if not cands:
            w("    附近 %.0f cm 内没有任何道路对的最近点，可能这个路口的路整条没了" % NEAR_ROAD)
            continue
        for dxy, la, lb, px, py, dz, at_end, same in sorted(cands)[:6]:
            w("    %-26s x %-26s 缺口 %7.0f  dz %5.0f  at_end=%s  same_road=%s"
              % (la, lb, dxy, dz, "是" if at_end else "否", "是" if same else "否"))
            if not same and dxy >= XY_CROSS:
                need.append(dxy)

    # ---------------------------------------------------------------- 阈值建议
    w()
    w("=" * 70)
    w("4. 阈值该卡在哪")
    w("=" * 70)
    if need:
        need.sort()
        w("漏掉的路口里，真正该连上的那些路对缺口（升序）：")
        w("    " + "、".join("%.0f" % v for v in need[:30]))
        w("把端点情形的阈值放到 %.0f 以上就能把它们都收进来。" % (need[-1] + 50))
    else:
        w("没有「该连上却超阈值」的路对——那漏判不是阈值问题，另查。")

    allv = sorted(pm[0] for pm in pair_min if not pm[7])
    w()
    w("所有不同逻辑路的路对，最近接近距离分布（升序，前 40）：")
    w("    " + "、".join("%.0f" % v for v in allv[:40]))
    w("找一个明显的数值空隙：空隙左边是真路口，右边是只是并行经过。")
    w("放宽阈值如果跨过了空隙，就会把「并行的两条路」判成路口。")

    sib = [pm for pm in pair_min if pm[7]]
    w()
    w("同一条路的兄弟段（_PartNN）之间的缺口，共 %d 对：" % len(sib))
    for dxy, la, lb, px, py, dz, at_end, _ in sorted(sib)[:20]:
        w("    %-26s x %-26s 缺口 %7.0f  at_end=%s"
          % (la, lb, dxy, "是" if at_end else "否"))
    w("这些必须排除在路口判定之外，否则每个切点都会多一个假路口。")


try:
    main()
except Exception:
    w("抛异常：")
    w(traceback.format_exc())

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

print("\n".join(lines[:40]))
print("...")
print("完整报告：" + OUT)
