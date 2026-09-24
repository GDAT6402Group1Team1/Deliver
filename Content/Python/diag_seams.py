# -*- coding: utf-8 -*-
"""逐点查落差大的接缝：车道点的高度和它正下方真实路面差多少。

背景：74 个接缝里中位落差只有 16cm，但有 6 个到了 100~250cm。
怀疑是高度剖面里长段打空处的全局插值偏离了真实路面，
接回真实命中时就产生台阶——但这只是怀疑，得逐点看。

对每个坏接缝，把上一段末尾和下一段开头的点都列出来，每点回答：
    点的 Z            生成时定下来的高度
    正下方路面 Z      现在重新打射线量到的
    差值              接近 Z_OFFSET(15) = 正常贴着路面
                      差很多        = 这个点是插值/兜底出来的，没贴着路
    命中了什么        路面网格？地形？还是压根没打到
这样能直接分辨：是插值漂移，还是路面本身真有台阶。

只读。
用法：py diag_seams.py
"""

import traceback

import unreal

TAG_PREFIX = "ClaudeGenLane"
NEIGHBOUR_MAX = 800.0
Z_WARN = 100.0          # 落差超过这个值才详查
EDGE_POINTS = 4         # 接缝两侧各详查几个点
Z_OFFSET = 15.0         # 生成时点比路面高出的量
SURFACE_MAX_LAYERS = 40   # 裙边能叠很多层，16 不够用（实测有点位耗尽预算）

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "seams.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def surface_probe(world, x, y, z_hint):
    """往下逐层扫，返回 (最上层路面Z, 描述, 所有路面层的Z列表)。

    关键是**所有**层而不只是最上面那层：路口重叠处两条路的路面叠在一起，
    生成器只取最上面的，于是路口内的点爬到交叉路的面上、路口外的点在本路的面上。
    只看最上层看不出这件事，得把层都列出来才能对上。
    """
    top = z_hint + 60000.0
    seen = []
    roads = []
    for _ in range(SURFACE_MAX_LAYERS):
        try:
            hit = unreal.SystemLibrary.line_trace_single(
                world, unreal.Vector(x, y, top),
                unreal.Vector(x, y, z_hint - 60000.0),
                unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
                False, [], unreal.DrawDebugTrace.NONE, True)
        except Exception as exc:
            return None, "异常 %s" % str(exc)[:30], []
        if hit is None:
            break
        d = hit.to_dict()
        if not d.get("blocking_hit"):
            break
        comp = d.get("hit_component")
        z = d["impact_point"].z
        cn = comp.get_class().get_name() if comp is not None else "?"
        if "SplineMesh" in cn:
            roads.append(z)
        else:
            seen.append(cn.replace("Component", "")[:12])
        top = z - 1.0
        if top <= z_hint - 60000.0:
            break
    if roads:
        return roads[0], "路面 %d 层" % len(roads), roads
    return None, "没路面 经过[%s]" % ",".join(seen[:4]), []


def parse(label):
    parts = str(label).split("_")
    return (parts[1], parts[2]) if len(parts) >= 3 else (str(label), "")


def detail(world, a, which):
    """列出一段的头部或尾部若干点。which = 'tail' / 'head'。"""
    sp = a.get_component_by_class(unreal.SplineComponent)
    n = sp.get_number_of_spline_points()
    idx = range(max(0, n - EDGE_POINTS), n) if which == "tail" \
        else range(0, min(n, EDGE_POINTS))
    w("    %s  (%s，共 %d 点)" % (a.get_actor_label(), which, n))
    for i in idx:
        p = sp.get_location_at_spline_point(i, WS)
        sz, desc, layers = surface_probe(world, p.x, p.y, p.z)
        if sz is None:
            w("      点%-2d (%7.0f,%7.0f)  Z %8.1f   下方没路面：%s"
              % (i, p.x, p.y, p.z, desc))
            continue
        gap = p.z - sz
        extra = ""
        if len(layers) > 1:
            extra = "   多层路面: %s   层差 %.0f  <<<" % (
                " / ".join("%.0f" % z for z in layers[:4]),
                layers[0] - layers[-1])
        w("      点%-2d (%7.0f,%7.0f)  Z %8.1f   路面 %8.1f   高出 %+7.1f%s"
          % (i, p.x, p.y, p.z, sz, gap, extra))


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()

    groups, by_label = {}, {}
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
        by_label[lbl] = a
        groups.setdefault(parse(lbl), []).append({
            "label": lbl,
            "p0": sp.get_location_at_distance_along_spline(0.0, WS),
            "p1": sp.get_location_at_distance_along_spline(L, WS),
        })

    bad = []
    for _key, items in groups.items():
        for x in items:
            for y in items:
                if x is y:
                    continue
                d = (y["p0"] - x["p1"]).length()
                if d < NEIGHBOUR_MAX:
                    dz = y["p0"].z - x["p1"].z
                    if abs(dz) > Z_WARN:
                        bad.append((abs(dz), dz, d, x["label"], y["label"]))
    bad.sort(reverse=True)

    w("落差超过 %.0fcm 的接缝 %d 处" % (Z_WARN, len(bad)))
    w("判读：'高出' 接近 %.0f 表示点正常贴在路面上；" % Z_OFFSET)
    w("      差很多说明这个点的高度是插值或兜底来的，没有真实路面支撑。")
    w("")
    flush()

    for _abs, dz, gap, la, lb in bad:
        w("=" * 92)
        w("%s  ->  %s    水平 %.0fcm   落差 %+.0fcm" % (la, lb, gap, dz))
        w("=" * 92)
        if la in by_label:
            detail(world, by_label[la], "tail")
        if lb in by_label:
            detail(world, by_label[lb], "head")
        w("")
        flush()

    w("只读脚本，没有改动任何东西。")
    flush()
    unreal.log("[seams] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[seams] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
