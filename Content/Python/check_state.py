# -*- coding: utf-8 -*-
"""快速盘点当前关卡状态，决定要从哪一步开始重跑。

重跑前必须知道：地形变形在不在（重复跑可能叠加）、路面厚度是多少、
裙边/板子还有没有残留、有没有被隐藏的路面段。

用法：py check_state.py
"""

import traceback

import unreal

OUT = unreal.Paths.project_saved_dir() + "state.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()
    w("关卡 actor 总数 %d" % len(actors))
    w("")

    # ---- 脚本生成物 ----
    w("--- 脚本生成的 actor（按 tag）---")
    counts = {}
    for a in actors:
        for t in a.tags:
            t = str(t)
            if t.startswith("ClaudeGen"):
                counts[t] = counts.get(t, 0) + 1
    if counts:
        for k in sorted(counts):
            w("   %-28s %d" % (k, counts[k]))
    else:
        w("   （没有任何 ClaudeGen* 标记的 actor）")
    w("")

    # ---- 路面 ----
    land = None
    for a in actors:
        if a.get_class().get_name() == "Landscape":
            land = a
            break
    if land is None:
        w("!! 找不到 Landscape")
        return
    sms = list(land.get_components_by_class(unreal.SplineMeshComponent))
    hidden = 0
    for c in sms:
        try:
            if not c.is_visible():
                hidden += 1
        except Exception:
            pass
    w("--- 路面 ---")
    w("   SplineMeshComponent %d 个，其中被隐藏 %d 个" % (len(sms), hidden))

    thick = []
    for c in sms[:400]:
        try:
            p = c.get_editor_property("spline_params")
            sc = p.get_editor_property("start_scale")
            thick.append(sc.y * 100.0)
        except Exception:
            pass
    if thick:
        thick.sort()
        w("   厚度(前400段采样): 最小 %.1f  中位 %.1f  最大 %.1f cm"
          % (thick[0], thick[len(thick) // 2], thick[-1]))
        w("   -> 6cm 左右 = 原始未加厚；14cm = 已加厚")
    w("")

    # ---- 地形编辑图层 ----
    w("--- 地形编辑图层 ---")
    try:
        for lay in land.get_edit_layers_bp():
            nm = "?"
            for meth in ("get_name_bp", "get_name"):
                if hasattr(lay, meth):
                    try:
                        nm = str(getattr(lay, meth)())
                        break
                    except Exception:
                        pass
            w("   %-24s %s" % (nm, type(lay).__name__))
    except Exception as exc:
        w("   读取失败: %s" % str(exc)[:80])
    w("   （地形变形写在 PS2DEM_Splines 层里，从这里看不出压过几次，")
    w("     要判断压没压过，看下面几条路的埋没情况）")
    w("")

    # ---- 抽查几条路是否已压平 ----
    w("--- 抽查埋没情况（判断地形压过没有）---")
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    roads = []
    for a in actors:
        if a.get_class().get_name() == "BP_PS2DEMSplineActor_v3_C":
            lbl = a.get_actor_label().strip()
            if "River" in lbl:
                continue
            sp = a.get_component_by_class(unreal.SplineComponent)
            if sp:
                roads.append((lbl, sp))
    roads.sort()
    pick = [r for r in roads if r[0] in ("形状 13", "形状 1", "形状 15", "形状 16")]
    for lbl, sp in pick or roads[:4]:
        tot = sp.get_spline_length()
        nb = nt = 0
        d = 0.0
        while d <= tot:
            loc = sp.get_location_at_distance_along_spline(d, WS)
            r = sp.get_right_vector_at_distance_along_spline(d, WS)
            h = (r.x * r.x + r.y * r.y) ** 0.5 or 1.0
            for off in (-400.0, 0.0, 400.0):
                x = loc.x + r.x / h * off
                y = loc.y + r.y / h * off
                try:
                    hit = unreal.SystemLibrary.line_trace_single(
                        world, unreal.Vector(x, y, loc.z + 60000.0),
                        unreal.Vector(x, y, loc.z - 60000.0),
                        unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, False, [],
                        unreal.DrawDebugTrace.NONE, True)
                    if hit is None:
                        continue
                    dd = hit.to_dict()
                    if not dd.get("blocking_hit"):
                        continue
                    comp = dd.get("hit_component")
                    cn = comp.get_class().get_name() if comp is not None else "?"
                    nt += 1
                    if "Landscape" in cn:
                        nb += 1
                except Exception:
                    pass
            d += 2000.0
        w("   %-10s 采样 %3d  地形在最上面 %3d 处 (%.0f%%)"
          % (lbl, nt, nb, 100.0 * nb / nt if nt else 0))
    w("   -> 接近 0% = 已压平；30~45% = 还没压")

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    unreal.log("[state] 写入 %s" % OUT)


WS = unreal.SplineCoordinateSpace.WORLD

try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[state] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
