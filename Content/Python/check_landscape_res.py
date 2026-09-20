# -*- coding: utf-8 -*-
"""量地形高度图的分辨率，判断"6cm 厚的路面能不能贴合"这件事有没有物理下限。

地形只能在网格点上表示高度，点之间线性插值。格子越大，斜穿的路面越无法贴合，
这个误差再怎么加密采样也消不掉。如果格子远大于路面厚度(6cm)，
那"既不埋又不飘"在数学上就做不到，只能把路面做厚。

用法：py check_landscape_res.py
"""

import traceback

import unreal

OUT = unreal.Paths.project_saved_dir() + "landscape_res.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    land = None
    for a in eas.get_all_level_actors():
        if a.get_class().get_name() == "Landscape":
            land = a
            break
    if land is None:
        w("找不到 Landscape")
        return

    w("Landscape: %s" % land.get_actor_label())
    sc = land.get_actor_scale3d()
    w("actor 缩放 = (%.3f, %.3f, %.3f)" % (sc.x, sc.y, sc.z))
    w("  -> 一个地形单元格在世界空间 = %.1f x %.1f cm" % (sc.x, sc.y))
    w("  -> 高度量化步长 ~= %.4f cm（Z 缩放 %.4f × 高度图 1/128 单位）"
      % (sc.z / 128.0, sc.z))
    w("")

    w("--- 组件/分段参数 ---")
    for pn in ("component_size_quads", "subsection_size_quads",
               "num_subsections", "landscape_material", "collision_mip_level",
               "simple_collision_mip_level", "static_lighting_lod"):
        try:
            w("   %-30s = %s" % (pn, land.get_editor_property(pn)))
        except Exception as exc:
            w("   %-30s ! %s" % (pn, str(exc)[:60]))

    w("")
    w("--- 可用的尺寸/分辨率相关方法 ---")
    cands = sorted(m for m in dir(land) if not m.startswith("_")
                   and any(k in m.lower() for k in
                           ("quad", "resolution", "size", "extent", "bound", "component")))
    w("   %s" % ", ".join(cands[:24]))

    try:
        o, e = land.get_actor_bounds(False)
        w("")
        w("地形范围: 中心(%.0f, %.0f, %.0f)  半尺寸(%.0f, %.0f, %.0f)"
          % (o.x, o.y, o.z, e.x, e.y, e.z))
        w("  -> 约 %.0f x %.0f 米" % (e.x * 2 / 100.0, e.y * 2 / 100.0))
    except Exception:
        pass

    w("")
    w("=" * 60)
    w("判断依据: 路面厚度 6cm。")
    w("  格子边长 << 6cm  -> 能贴合，继续调偏移量有意义")
    w("  格子边长 >> 6cm  -> 斜坡上无法贴合，必须把路面做厚")

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    unreal.log("[res] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[res] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
