# -*- coding: utf-8 -*-
"""查现有交通线 / 路口线还贴不贴现在的路面。

存在的理由：车道线是按**当时**的路面几何生成的（逐点打射线测高、
横向按标称半宽给偏移）。队友重建道路、手动调交叉口控制点之后，
路面可能整体挪了位、宽度变了、路口形状变了，而 Lane_* / Inter_*
的点坐标是死的，不会跟着动。

两个方向分开查，因为修法完全不同：
  垂直  —— 车道点相对路面顶面应当高 Z_OFFSET(15cm)。偏高=悬空，偏低=埋进路里。
           只是高度不对的话重跑 gen_traffic_lanes.py 就能修。
  横向  —— 从车道点沿法线往两侧走，road 的 SplineMesh 在哪一步消失，
           就是路缘。两侧余量都很小或某侧为 0 = 车道压边/整个跑到路外面了。
           左右余量差得多 = 路横向挪过位，车道没跟上（重跑也能修）。
           两侧都探不到路面 = 那段路本身被删了/改道了，重跑也补不回来，得看关卡。

只读，什么都不改。
用法：py diag_lane_fit.py
"""

import traceback

import unreal

TAG_PREFIX = "ClaudeGenLane"
Z_OFFSET = 2.0           # 和 gen_traffic_lanes.py 保持一致（2026-09-24 从 15 降到 2）
SAMPLE_STEP = 500.0      # 沿每条样条每隔多远采一个点
MAX_SAMPLES_PER = 24     # 单条样条最多采几个点，防止长路段把预算吃光

SURFACE_MAX_LAYERS = 40  # 与生成器一致
SLAB_SKIP = 30.0
SLAB_MERGE = 60.0

SIDE_STEP = 100.0        # 横向每步走多远
SIDE_MAX = 1400.0        # 最远探到哪
SIDE_WINDOW = 300.0      # 侧向探测时，只认落在期望路面高度 ±这个范围内的 SplineMesh

DZ_WARN = 60.0           # 高度偏差超过这个值就算不贴
EDGE_WARN = 80.0         # 离路缘近于这个值就算压边
WORST_ROWS = 25

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "lane_fit.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def surface_layers(world, x, y, z_hint):
    """这一竖线上所有互不相同的路面（SplineMesh）高度，由高到低。

    同一块板会被连续命中二十几次（射线每次只降 1cm，板厚 26cm），
    所以命中路面后直接跳 SLAB_SKIP 跨过整块板。
    """
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


def has_road_at(world, x, y, z_expect):
    """(x,y) 处、期望高度附近有没有路面。一次射线，窗口卡在 ±SIDE_WINDOW。

    不复用 surface_layers 是为了省预算：横向探测每个采样点要打二十几次，
    用全量分层扫会慢一个数量级。窄窗口也顺带避开了路口下层那条路的面。
    """
    top = z_expect + SIDE_WINDOW
    bottom = z_expect - SIDE_WINDOW
    for _ in range(6):
        try:
            hit = unreal.SystemLibrary.line_trace_single(
                world, unreal.Vector(x, y, top), unreal.Vector(x, y, bottom),
                unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
                False, [], unreal.DrawDebugTrace.NONE, True)
        except Exception:
            return False
        if hit is None:
            return False
        d = hit.to_dict()
        if not d.get("blocking_hit"):
            return False
        comp = d.get("hit_component")
        cn = comp.get_class().get_name() if comp is not None else "?"
        if "SplineMesh" in cn:
            return True
        top = d["impact_point"].z - 1.0   # 裙边/地形挡在前面，继续往下找
        if top <= bottom:
            return False
    return False


def edge_dist(world, p, rx, ry, z_surf):
    """沿 (rx,ry) 方向走到路面消失为止，返回余量 cm；一步都走不动返回 0。"""
    d = SIDE_STEP
    while d <= SIDE_MAX:
        if not has_road_at(world, p.x + rx * d, p.y + ry * d, z_surf):
            return d - SIDE_STEP
        d += SIDE_STEP
    return SIDE_MAX


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(
        unreal.UnrealEditorSubsystem).get_editor_world()

    items = []
    for a in eas.get_all_level_actors():
        if not any(str(t).startswith(TAG_PREFIX) for t in a.tags):
            continue
        lbl = a.get_actor_label()
        if not (lbl.startswith("Lane_") or lbl.startswith("Inter_")):
            continue
        sp = a.get_component_by_class(unreal.SplineComponent)
        if sp is None or sp.get_number_of_spline_points() < 2:
            continue
        items.append((lbl, a, sp))
    items.sort(key=lambda t: t[0])

    w("交通线贴合度检查")
    w("车道段 %d 条，路口段 %d 条"
      % (sum(1 for i in items if i[0].startswith("Lane_")),
         sum(1 for i in items if i[0].startswith("Inter_"))))
    w("每 %.0fcm 采一点；期望车道点高于路面 %.0fcm" % (SAMPLE_STEP, Z_OFFSET))
    w("")
    flush()
    if not items:
        w("!! 一条都没有。")
        flush()
        return

    rows = []
    g_n = g_air = g_dz = g_edge = 0
    for lbl, a, sp in items:
        total = sp.get_spline_length()
        step = max(SAMPLE_STEP, total / MAX_SAMPLES_PER) if total > 0 else SAMPLE_STEP
        n = air = 0
        dzs = []
        emin = None
        eL = eR = None
        d = 0.0
        while d <= total + 1e-3:
            p = sp.get_location_at_distance_along_spline(d, WS)
            cur = d
            d += step
            n += 1
            layers = surface_layers(world, p.x, p.y, p.z)
            if not layers:
                air += 1
                continue
            # 挑离"车道点减去应有高差"最近的那层，而不是最上面那层：
            # 路口处两条路的面叠着，挑最上面的会挑到交叉路上去。
            zs = min(layers, key=lambda z: abs(z - (p.z - Z_OFFSET)))
            dzs.append(p.z - Z_OFFSET - zs)
            r = sp.get_right_vector_at_distance_along_spline(cur, WS)
            h = (r.x * r.x + r.y * r.y) ** 0.5
            if h < 1e-4:
                continue
            rx, ry = r.x / h, r.y / h
            dr = edge_dist(world, p, rx, ry, zs)
            dl = edge_dist(world, p, -rx, -ry, zs)
            if eR is None or dr < eR:
                eR = dr
            if eL is None or dl < eL:
                eL = dl
            m = min(dl, dr)
            if emin is None or m < emin:
                emin = m

        mdz = max((abs(v) for v in dzs), default=0.0)
        adz = (sum(dzs) / len(dzs)) if dzs else 0.0
        g_n += n
        g_air += air
        if mdz > DZ_WARN:
            g_dz += 1
        if emin is not None and emin < EDGE_WARN:
            g_edge += 1
        rows.append((lbl, n, air, adz, mdz, eL, eR, emin))
        flush()

    def fmt(v):
        return "  --" if v is None else "%4.0f" % v

    w("=" * 104)
    w("按问题严重程度排序（悬空点数 > 最大高差 > 最小路缘余量）")
    w("=" * 104)
    w("%-34s %4s %4s %8s %8s %6s %6s %6s"
      % ("段", "采样", "悬空", "平均高差", "最大高差", "左余", "右余", "最小余"))
    w("-" * 104)

    def badness(r):
        _l, n, air, _a, mdz, _el, _er, emin = r
        return (-air, -mdz, (emin if emin is not None else 9999))

    for r in sorted(rows, key=badness)[:WORST_ROWS]:
        lbl, n, air, adz, mdz, eL, eR, emin = r
        w("%-34s %4d %4d %+8.0f %8.0f %6s %6s %6s"
          % (lbl[:34], n, air, adz, mdz, fmt(eL), fmt(eR), fmt(emin)))

    w("")
    w("=" * 104)
    w("汇总")
    w("=" * 104)
    w("采样点合计 %d，其中 %d 个点脚下完全没有路面（%.1f%%）"
      % (g_n, g_air, 100.0 * g_air / max(1, g_n)))
    w("最大高差超过 %.0fcm 的段：%d / %d" % (DZ_WARN, g_dz, len(rows)))
    w("有一侧路缘余量小于 %.0fcm 的段：%d / %d" % (EDGE_WARN, g_edge, len(rows)))
    w("")
    ok = [r for r in rows if r[2] == 0 and r[4] <= DZ_WARN
          and (r[7] is None or r[7] >= EDGE_WARN)]
    w("完全贴合的段：%d / %d（%.0f%%）"
      % (len(ok), len(rows), 100.0 * len(ok) / max(1, len(rows))))
    w("")
    # 左右余量的系统性差值 = 路整体横向挪过位，车道没跟上
    both = [r for r in rows if r[5] is not None and r[6] is not None]
    if both:
        bias = sum(r[6] - r[5] for r in both) / len(both)
        w("左右路缘余量平均差 %+.0fcm（右余 - 左余）。" % bias)
        w("  接近 0 = 车道还在路中间；显著非 0 = 路横向挪了位，车道整体偏向一侧。")
    w("")
    w("怎么读：")
    w("  悬空点 —— 那个位置压根没有路面。多半是那段路被队友删了或改了走向，")
    w("             重跑生成器也补不回来，要先看关卡。")
    w("  高差   —— 只是 Z 不对，重跑 rebuild_traffic.py 就能重新贴回去。")
    w("  路缘余量 —— 两侧都够宽才算在路中间。某侧为 0 表示车道已经压在路缘上")
    w("             甚至跑到路外了；横向位置是按标称半宽给的，路变宽/变窄都会中招。")
    w("只读脚本，没有改动任何东西。")
    flush()
    unreal.log("[fit] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[fit] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
