# -*- coding: utf-8 -*-
"""列出某个 actor 附近所有带样条的东西，看清视口里那些线到底是谁。

视口里样条一律画成青绿色，Lane_ / Inter_ / 源道路样条 / 转弯线看起来一模一样，
光凭截图分不出来。这个脚本按距离把它们全列出来：类、标签、tag、
组件名、点数、长度、首尾坐标。

只读。
用法：py whats_here.py
"""

import traceback

import unreal

ANCHOR = "Intersection_28"   # 以谁为中心
RADIUS = 3500.0                     # 查多大范围
MAX_ROWS = 80

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "whats_here.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    anchor = None
    for a in actors:
        if a.get_actor_label() == ANCHOR:
            anchor = a
            break
    if anchor is None:
        w("!! 找不到 %s。名字相近的：" % ANCHOR)
        key = ANCHOR.split("_")[1] if "_" in ANCHOR else ANCHOR
        for a in actors:
            if key in a.get_actor_label():
                w("   %s" % a.get_actor_label())
                if len(lines) > 40:
                    break
        flush()
        return

    c = anchor.get_actor_location()
    w("以 %s @ (%.0f, %.0f, %.0f) 为中心，半径 %.0f"
      % (ANCHOR, c.x, c.y, c.z, RADIUS))
    w("")

    rows = []
    for a in actors:
        comps = list(a.get_components_by_class(unreal.SplineComponent))
        if not comps:
            continue
        loc = a.get_actor_location()
        d = (loc - c).length()
        if d > RADIUS:
            continue
        tags = ",".join(str(t) for t in a.tags) or "-"
        for sp in comps:
            n = sp.get_number_of_spline_points()
            L = sp.get_spline_length()
            p0 = sp.get_location_at_distance_along_spline(0.0, WS)
            p1 = sp.get_location_at_distance_along_spline(L, WS)
            rows.append((d, a.get_actor_label(), a.get_class().get_name(),
                         sp.get_name(), n, L, p0, p1, tags))

    rows.sort(key=lambda r: r[0])
    w("范围内带样条的组件 %d 个（按距离排序）" % len(rows))
    w("")
    w("%7s %-30s %-28s %-13s %4s %8s  %s"
      % ("距离", "actor", "类", "组件", "点", "长度", "首 -> 尾"))
    w("-" * 145)
    for d, lbl, cls, comp, n, L, p0, p1, tags in rows[:MAX_ROWS]:
        w("%7.0f %-30s %-28s %-13s %4d %8.0f  (%6.0f,%7.0f)->(%6.0f,%7.0f)"
          % (d, lbl[:30], cls[:28], comp[:13], n, L, p0.x, p0.y, p1.x, p1.y))

    w("")
    w("按类小计：")
    by_cls = {}
    for r in rows:
        by_cls[r[2]] = by_cls.get(r[2], 0) + 1
    for k in sorted(by_cls, key=lambda x: -by_cls[x]):
        w("   %-34s %d" % (k, by_cls[k]))

    w("")
    w("按组件名小计（零长的是被压掉的转弯线）：")
    by_comp = {}
    for r in rows:
        key = "%s %s" % (r[3], "(零长)" if r[5] < 1.0 else "")
        by_comp[key] = by_comp.get(key, 0) + 1
    for k in sorted(by_comp, key=lambda x: -by_comp[x]):
        w("   %-24s %d" % (k, by_comp[k]))

    w("")
    w("没有 tag 的（不是脚本生成的，多半是关卡里本来就有的东西）：")
    seen = set()
    for d, lbl, cls, comp, n, L, p0, p1, tags in rows:
        if tags == "-" and lbl not in seen:
            seen.add(lbl)
            w("   %7.0f  %-30s %s" % (d, lbl[:30], cls))
    if not seen:
        w("   没有，范围内全是脚本生成的。")

    flush()
    unreal.log("[whats] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[whats] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常：")
    lines.append(err)
    flush()
