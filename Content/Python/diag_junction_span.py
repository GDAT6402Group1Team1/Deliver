# -*- coding: utf-8 -*-
"""判定「方向多于两个」的路口到底是真多岔，还是路口盒跨了两个路口。

背景见 Document/TrafficLightGroups.md：5 个路口聚出 3 个以上轴向，
`fix_light_number_by_axis.py` 把它们挂进 SKIP_INTERSECTIONS 等人工定策略。
两种成因对应**完全不同**的修法，不能靠看角度猜：
    真多岔     → 开 N 相位（或按几何把互不冲突的两向并组）
    盒子过界   → 调盒子，相位一点都不用动

判据不用角度，用**位置**：把每个 BP_Intersection 的盒子罩住的 Inter_ 段
按世界位置做连通聚类。真路口的段必然挤在一团（路口就那么大）；
盒子跨了两个路口的话，段会分成两团、团心相距几千。
角度只能提示、位置才是证据——同一条路在真路口里也可能因为曲率差几十度。

顺带报每条路**自己**的轴角跨度：同一条路在一个路口里应该只有一个走向
（对向车道归一化后同轴），跨度大说明这条路的两段其实属于不同路口。

盒子罩住谁的判据和蓝图一致（`GetOverlappingActors` 按盒子重叠），
这里用「段的原点落在盒子的有向包围盒内」近似，和 gen_intersections 同一套。

只读，什么都不改。
用法：py diag_junction_span.py
"""

import math
import traceback

import unreal

INTER_PREFIX = "BP_Intersection"   # 路口盒 actor 的名字前缀
CHILD_PREFIX = "Inter_"            # 路口段 actor 的名字前缀
CLUSTER_DIST = 2000.0              # 段之间近于这个值算同一团
AXIS_SPREAD_WARN = 40.0            # 同一条路的轴角跨度超过这个值就点名
ONLY = []                          # 只查这几个路口（填 actor 名）；空 = 全查
REPORT_OK = False                  # True = 连正常的路口也逐个列出来

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "junction_span.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def road_of(label):
    """Inter_MainRoad_R010_+0540_I16 -> MainRoad_R010

    用 rsplit 从右边剥「偏移」和「序号」两段，所以路名里有几个下划线都不怕
    （BranchRoad_R036_Merged 这种按左边切会被截成 BranchRoad）。
    """
    s = str(label)
    if s.startswith(CHILD_PREFIX):
        s = s[len(CHILD_PREFIX):]
    parts = s.rsplit("_", 2)
    return parts[0] if len(parts) == 3 else s


def axis_angle(sp):
    """样条首尾连线的**无向**轴角，归一化到 [0,180)。

    对向车道差 180°，归一化之后同轴——这正是「同轴同组」要的语义。
    """
    L = sp.get_spline_length()
    if L < 1.0:
        return None
    a = sp.get_location_at_distance_along_spline(0.0, WS)
    b = sp.get_location_at_distance_along_spline(L, WS)
    dx, dy = b.x - a.x, b.y - a.y
    if (dx * dx + dy * dy) ** 0.5 < 1e-3:
        return None
    return math.degrees(math.atan2(dy, dx)) % 180.0


def axis_diff(a, b):
    """两个无向轴角的夹角，0~90。"""
    d = abs(a - b) % 180.0
    return min(d, 180.0 - d)


def covers(box, p):
    """点 p 在不在这个盒子里。转进盒子的局部空间再和半尺寸比。

    inverse_transform_location 会把缩放一起除掉，所以局部坐标正好和
    box_extent（未缩放的半尺寸）同一套单位，直接比即可。
    """
    try:
        t = box.get_world_transform()
        local = t.inverse_transform_location(p)
        e = box.get_editor_property("box_extent")
        return (abs(local.x) <= e.x and abs(local.y) <= e.y
                and abs(local.z) <= e.z)
    except Exception:
        return False


def cluster(points, dist):
    """按距离做连通聚类，返回每团的下标列表。"""
    n = len(points)
    parent = list(range(n))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for i in range(n):
        for j in range(i + 1, n):
            a, b = points[i], points[j]
            if ((a.x - b.x) ** 2 + (a.y - b.y) ** 2) ** 0.5 < dist:
                ri, rj = find(i), find(j)
                if ri != rj:
                    parent[max(ri, rj)] = min(ri, rj)

    groups = {}
    for i in range(n):
        groups.setdefault(find(i), []).append(i)
    return list(groups.values())


def centroid(points, idx):
    n = len(idx)
    return (sum(points[i].x for i in idx) / n,
            sum(points[i].y for i in idx) / n)


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    inters, segs = [], []
    for a in actors:
        lbl = a.get_actor_label()
        if lbl.startswith(INTER_PREFIX) or lbl.startswith("Intersection_"):
            box = a.get_component_by_class(unreal.BoxComponent)
            if box is not None:
                inters.append((lbl, a, box))
        elif lbl.startswith(CHILD_PREFIX):
            sp = a.get_component_by_class(unreal.SplineComponent)
            if sp is not None and sp.get_number_of_spline_points() >= 2:
                segs.append((lbl, a.get_actor_location(), axis_angle(sp)))

    inters.sort(key=lambda t: t[0])
    w("路口盒 %d 个，路口段 %d 个" % (len(inters), len(segs)))
    w("同一团的判据：段间距 < %.0fcm；同一条路轴角跨度 > %.0f° 点名"
      % (CLUSTER_DIST, AXIS_SPREAD_WARN))
    w("")
    flush()
    if not inters or not segs:
        w("!! 缺路口盒或路口段。")
        flush()
        return

    suspect, multi, fine = [], [], []
    for lbl, a, box in inters:
        if ONLY and lbl not in ONLY:
            continue
        idx = [i for i, (_l, p, _ang) in enumerate(segs) if covers(box, p)]
        if not idx:
            suspect.append((lbl, "盒子一段都没罩住", 0, 0, 0.0, []))
            continue

        pts = [segs[i][1] for i in idx]
        blobs = cluster(pts, CLUSTER_DIST)

        # 团心之间最远多少
        far = 0.0
        if len(blobs) > 1:
            cs = [centroid(pts, b) for b in blobs]
            for i in range(len(cs)):
                for j in range(i + 1, len(cs)):
                    d = ((cs[i][0] - cs[j][0]) ** 2
                         + (cs[i][1] - cs[j][1]) ** 2) ** 0.5
                    far = max(far, d)

        # 每条路自己的轴角跨度
        by_road = {}
        for i in idx:
            ang = segs[i][2]
            if ang is not None:
                by_road.setdefault(road_of(segs[i][0]), []).append(ang)
        spread = []
        for rd, angs in sorted(by_road.items()):
            mx = 0.0
            for x in range(len(angs)):
                for y in range(x + 1, len(angs)):
                    mx = max(mx, axis_diff(angs[x], angs[y]))
            if mx > AXIS_SPREAD_WARN:
                spread.append((rd, mx, len(angs)))

        # 轴向种类数（粗粒度，只为了对上另一份审计的说法）
        axes = []
        for i in idx:
            ang = segs[i][2]
            if ang is None:
                continue
            if not any(axis_diff(ang, ax) < 30.0 for ax in axes):
                axes.append(ang)

        row = (lbl, "", len(idx), len(blobs), far, spread)
        if len(blobs) > 1 or spread:
            suspect.append(row)
        elif len(axes) > 2:
            multi.append((lbl, len(idx), len(axes)))
        else:
            fine.append(lbl)

    w("=" * 92)
    w("① 盒子跨了两个路口（段分成多团，或同一条路走向差太多）")
    w("=" * 92)
    if suspect:
        w("%-20s %5s %5s %10s  %s"
          % ("路口", "段数", "团数", "团心最远", "同路走向跨度过大的"))
        w("-" * 92)
        for lbl, note, n, nb, far, spread in suspect:
            tail = "；".join("%s 跨 %.0f°(%d段)" % (r, m, c) for r, m, c in spread)
            if note:
                tail = note
            w("%-20s %5d %5d %10.0f  %s" % (lbl[:20], n, nb, far, tail))
        w("")
        w("团数 > 1 或团心相距几千 = 一个盒子罩住了两个路口。")
        w("**正解是调盒子，不是开多相位**：收紧 gen_intersections.py 的")
        w("CLUSTER_DIST，或手动改这两个盒子的缩放，改完重跑")
        w("fix_light_number_by_axis.py，多出来的轴向会自己消失。")
    else:
        w("  没有。所有盒子罩住的段都挤在一团、每条路走向自洽。")

    w("")
    w("=" * 92)
    w("② 真多岔（段在一团里，但确实有 3 个以上方向）")
    w("=" * 92)
    if multi:
        w("%-20s %5s %5s" % ("路口", "段数", "轴向数"))
        w("-" * 92)
        for lbl, n, na in multi:
            w("%-20s %5d %5d" % (lbl[:20], n, na))
        w("")
        w("这些是真的多岔，两色表达不了。三条路见 TrafficLightGroups.md：")
        w("开 N 相位（每向等 (N-1)×LightDuration）、或按几何把互不冲突的两向并组。")
    else:
        w("  没有。")

    w("")
    w("=" * 92)
    w("合计：可疑盒子 %d 个，真多岔 %d 个，正常 %d 个"
      % (len(suspect), len(multi), len(fine)))
    w("=" * 92)
    if REPORT_OK and fine:
        for lbl in fine:
            w("  正常 %s" % lbl)
    w("只读脚本，没有改动任何东西。")
    flush()
    unreal.log("[span] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[span] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
