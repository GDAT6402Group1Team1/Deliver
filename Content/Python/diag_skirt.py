# -*- coding: utf-8 -*-
"""诊断露在外面的裙边段。

逐段对比裙边和它下方真实路面的关系：
  * 裙边顶 vs 路面顶   -> 判断是不是冒到路面上方
  * 裙边宽 vs 局部路宽 -> 判断是不是比该处的路还宽（整条路只测一个中位半宽，
                          路宽沿线变化时局部就会超宽）
先详细报告 FOCUS 指定的那一段，再扫全部裙边列出同类异常。

用法：py diag_skirt.py
"""

import traceback

import unreal

FOCUS = "Skirt_形状1_361"
TAG = "ClaudeGenSkirt"
MESH_SIZE = 100.0
TOP_OVERLAP = 11.0             # 生成时用的值，用来算预期裙边顶
PROBE_STEP = 50.0
PROBE_MAX = 1400.0
UP = 60000.0
DOWN = 60000.0
MAX_STEPS = 8
SCAN_LIMIT = 4000

OUT = unreal.Paths.project_saved_dir() + "skirt_diag.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def road_hit(world, x, y, z_hint):
    """返回 (路面顶Z, 法线)；打不到返回 (None, None)。"""
    top = z_hint + UP
    for _ in range(MAX_STEPS):
        try:
            hit = unreal.SystemLibrary.line_trace_single(
                world, unreal.Vector(x, y, top), unreal.Vector(x, y, z_hint - DOWN),
                unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, False, [],
                unreal.DrawDebugTrace.NONE, True)
        except Exception:
            return None, None
        if hit is None:
            return None, None
        d = hit.to_dict()
        if not d.get("blocking_hit"):
            return None, None
        comp = d.get("hit_component")
        z = d["impact_point"].z
        cn = comp.get_class().get_name() if comp is not None else "?"
        if "SplineMesh" in cn:
            return z, d.get("impact_normal")
        top = z - 1.0
        if top <= z_hint - DOWN:
            return None, None
    return None, None


def local_half_width(world, a, z_hint):
    """在裙边 actor 的横向方向上探测该处真实路面半宽。"""
    rt = a.get_actor_right_vector()
    h = (rt.x * rt.x + rt.y * rt.y) ** 0.5 or 1.0
    hx, hy = rt.x / h, rt.y / h
    loc = a.get_actor_location()
    res = []
    for sign in (-1.0, 1.0):
        off, last = 0.0, 0.0
        while off <= PROBE_MAX:
            z, _n = road_hit(world, loc.x + hx * off * sign,
                             loc.y + hy * off * sign, z_hint)
            if z is None:
                break
            last = off
            off += PROBE_STEP
        res.append(last)
    return res  # [左, 右]


def report(world, a, detail):
    loc = a.get_actor_location()
    sc = a.get_actor_scale3d()
    rot = a.get_actor_rotation()
    skirt_w = sc.y * MESH_SIZE
    skirt_h = sc.z * MESH_SIZE
    skirt_top = loc.z + skirt_h / 2.0

    rz, nrm = road_hit(world, loc.x, loc.y, loc.z)
    if detail:
        w("  位置 (%.0f, %.0f, %.0f)" % (loc.x, loc.y, loc.z))
        w("  旋转 pitch %.2f  yaw %.2f  roll %.2f" % (rot.pitch, rot.yaw, rot.roll))
        w("  裙边 宽 %.0f  高 %.0f  顶面 Z %.1f" % (skirt_w, skirt_h, skirt_top))
    if rz is None:
        if detail:
            w("  !! 这个位置下方没有路面 —— 裙边铺到路外面去了")
        return ("no-road", 0.0, 0.0)

    over = skirt_top - rz          # >0 = 裙边顶高过路面顶
    lw = local_half_width(world, a, loc.z)
    widest = max(lw) * 2.0
    wide_excess = skirt_w - widest  # >0 = 裙边比该处路面宽

    if detail:
        w("  路面顶 Z %.1f   法线 %s" % (rz, nrm))
        w("  裙边顶 - 路面顶 = %+.1f cm   (预期约 -%.0f)" % (over, TOP_OVERLAP))
        w("  该处实测路宽 %.0f（左 %.0f / 右 %.0f），裙边宽 %.0f  -> 超宽 %+.0f cm"
          % (widest, lw[0], lw[1], skirt_w, wide_excess))
        w("")
        if over > 0:
            w("  >>> 裙边顶高过路面 %.1f cm —— 纵向/倾斜没对齐" % over)
        if wide_excess > 60:
            w("  >>> 裙边比该处路面宽 %.0f cm —— 局部路宽比整条路的中位值窄" % wide_excess)
        if over <= 0 and wide_excess <= 60:
            w("  >>> 这两项都正常，问题可能在别处（朝向、或相邻段）")
    return ("ok", over, wide_excess)


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    actors = eas.get_all_level_actors()
    skirts = [a for a in actors if TAG in [str(t) for t in a.tags]]
    w("裙边 actor 共 %d 个" % len(skirts))
    w("")

    target = None
    for a in skirts:
        if a.get_actor_label() == FOCUS:
            target = a
            break
    w("=" * 62)
    w("详查 %s" % FOCUS)
    w("=" * 62)
    if target is None:
        w("  找不到这个 actor。名字相近的:")
        for a in skirts:
            if "形状1_" in a.get_actor_label():
                w("    %s" % a.get_actor_label())
                if len(lines) > 40:
                    break
    else:
        report(world, target, True)

    w("")
    w("=" * 62)
    w("扫描全部裙边，列出异常")
    w("=" * 62)
    bad_top, bad_wide, no_road = [], [], []
    for a in skirts[:SCAN_LIMIT]:
        try:
            kind, over, wide = report(world, a, False)
        except Exception:
            continue
        lbl = a.get_actor_label()
        if kind == "no-road":
            no_road.append(lbl)
        else:
            if over > 0:
                bad_top.append((over, lbl))
            if wide > 60:
                bad_wide.append((wide, lbl))

    w("顶面冒出路面的: %d 个" % len(bad_top))
    for v, lbl in sorted(bad_top, reverse=True)[:15]:
        w("   +%6.1f cm  %s" % (v, lbl))
    w("")
    w("比局部路面宽的: %d 个" % len(bad_wide))
    for v, lbl in sorted(bad_wide, reverse=True)[:15]:
        w("   +%6.0f cm  %s" % (v, lbl))
    w("")
    w("下方根本没有路面的: %d 个" % len(no_road))
    for lbl in no_road[:15]:
        w("   %s" % lbl)

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    unreal.log("[skirtdiag] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[skirtdiag] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
