# -*- coding: utf-8 -*-
"""量车道各段接缝处的**水平间距**和**高度落差**。

高度落差是后加的：实测 Inter_形状13_-0675_I03 和 Lane_形状13_-0675_S04
之间有极大的高低落差。根源是取高度的射线打空后按段插值——
每段各自在段内找锚点，打空区域跨在两段交界上时，两段的锚点不是同一批，
各自平滑但接缝对不上。

不靠"依次串起来"来判断谁挨着谁：段间距只有 250cm 时，
串错一次后面全错。改成两两配对——A 的终点离 B 的起点足够近就算一对接缝，
顺序错不了。

用法：py diag_lane_gaps.py
"""

import traceback

import unreal

TAG_PREFIX = "ClaudeGenLane"
NEIGHBOUR_MAX = 800.0    # 端点距离小于此值才算一对相邻接缝
Z_WARN = 100.0           # 高度落差超过这个值就标出来

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "lane_gaps.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def parse(label):
    """Lane_形状13_-0675_S00 -> ('形状13', '-0675')"""
    parts = str(label).split("_")
    if len(parts) >= 3:
        return parts[1], parts[2]
    return str(label), ""


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    groups = {}
    for a in eas.get_all_level_actors():
        if not any(str(t).startswith(TAG_PREFIX) for t in a.tags):
            continue
        lbl = a.get_actor_label()
        if not (lbl.startswith("Lane_") or lbl.startswith("Inter_")):
            continue
        sp = a.get_component_by_class(unreal.SplineComponent)
        if sp is None or sp.get_number_of_spline_points() < 2:
            continue
        L = sp.get_spline_length()
        groups.setdefault(parse(lbl), []).append({
            "label": lbl,
            "len": L,
            "p0": sp.get_location_at_distance_along_spline(0.0, WS),
            "p1": sp.get_location_at_distance_along_spline(L, WS),
        })

    w("车道组 %d 个（按 路名 + 偏移 分组）" % len(groups))
    w("接缝判定：A 的终点离 B 的起点 < %.0fcm" % NEIGHBOUR_MAX)
    w("")
    flush()

    all_gaps, all_dz, worst = [], [], []
    for (road, off), items in sorted(groups.items()):
        pairs = []
        for a in items:
            for b in items:
                if a is b:
                    continue
                d = (b["p0"] - a["p1"]).length()
                if d < NEIGHBOUR_MAX:
                    dz = b["p0"].z - a["p1"].z
                    pairs.append((d, dz, a["label"], b["label"]))
        if not pairs:
            continue
        w("=" * 96)
        w("%s  偏移 %s   段 %d 个，接缝 %d 处" % (road, off, len(items), len(pairs)))
        w("=" * 96)
        w("  %-34s -> %-34s %9s %9s" % ("上一段", "下一段", "水平间距", "高度差"))
        for d, dz, la, lb in sorted(pairs, key=lambda t: -abs(t[1])):
            flag = "   <<<" if abs(dz) > Z_WARN else ""
            w("  %-34s -> %-34s %7.0fcm %+8.0fcm%s"
              % (la[:34], lb[:34], d, dz, flag))
            all_gaps.append(d)
            all_dz.append(abs(dz))
            worst.append((abs(dz), la, lb, d, dz))
        w("")
        flush()

    w("=" * 96)
    w("总览")
    w("=" * 96)
    if all_gaps:
        g = sorted(all_gaps)
        z = sorted(all_dz)
        w("  接缝 %d 处" % len(all_gaps))
        w("  水平间距  最小 %.0f  中位 %.0f  最大 %.0f cm"
          % (g[0], g[len(g) // 2], g[-1]))
        w("  高度落差  中位 %.0f  最大 %.0f cm"
          % (z[len(z) // 2], z[-1]))
        bad = [t for t in worst if t[0] > Z_WARN]
        w("")
        w("  落差超过 %.0fcm 的接缝：%d 处" % (Z_WARN, len(bad)))
        for dza, la, lb, d, dz in sorted(bad, reverse=True)[:15]:
            w("     %+8.0fcm   %s -> %s" % (dz, la[:32], lb[:32]))
        if not bad:
            w("     没有。所有接缝高度都连续。")
    else:
        w("  没找到任何接缝。可能是段间距大于 %.0f，或者车道还没生成。"
          % NEIGHBOUR_MAX)
    flush()
    unreal.log("[lanegaps] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[lanegaps] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
