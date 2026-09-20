# -*- coding: utf-8 -*-
"""查两件事，决定 Deform Landscape to Splines 能不能放心点。

1) 地形有没有启用编辑图层（Edit Layers）
   有 -> 样条变形写进 ULandscapeEditLayerSplines 保留图层，可清除、可逆
   没有 -> 直接写基础高度图，不可逆，动手前必须先提交备份

2) 现有地形样条的 Width / SideFalloff
   单侧影响宽度 = Width(半宽) + SideFalloff，据此算出实际被改动的地形带有多宽

用法：py check_landscape_layers.py
"""

import traceback

import unreal

OUT = unreal.Paths.project_saved_dir() + "landscape_layers.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def try_props(obj, names, indent="    "):
    got = {}
    for n in names:
        try:
            got[n] = obj.get_editor_property(n)
        except Exception as exc:
            got[n] = "!%s" % str(exc)[:50]
    for k in names:
        w("%s%-34s = %s" % (indent, k, got[k]))
    return got


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    lands = [a for a in actors if "Landscape" in a.get_class().get_name()]
    w("找到 %d 个 Landscape 类 actor" % len(lands))
    w("")

    for a in lands:
        w("=" * 66)
        w("%s   类 %s" % (a.get_actor_label(), a.get_class().get_name()))
        w("=" * 66)

        w("  --- 编辑图层相关 ---")
        try_props(a, ["can_have_layers_content", "b_can_have_layers_content",
                      "landscape_guid", "use_generated_landscape_split_mesh_actors"])

        # 枚举编辑图层
        for meth in ("get_edit_layers", "get_layers", "get_layer_count"):
            if hasattr(a, meth):
                try:
                    r = getattr(a, meth)()
                    w("  %s() -> %s" % (meth, str(r)[:200]))
                except Exception as exc:
                    w("  %s() ! %s" % (meth, str(exc)[:70]))

        # 有没有样条保留图层
        w("  --- 可用方法里和图层/样条有关的 ---")
        cands = sorted(m for m in dir(a)
                       if not m.startswith("_")
                       and ("layer" in m.lower() or "spline" in m.lower()))
        w("    %s" % (", ".join(cands[:30]) if cands else "(无)"))

        # 样条组件与控制点
        w("  --- 地形样条组件 ---")
        found = False
        for c in a.get_components_by_class(unreal.ActorComponent):
            cn = c.get_class().get_name()
            if "Spline" not in cn:
                continue
            found = True
            w("    %-40s %s" % (c.get_name()[:40], cn))
            if "LandscapeSplinesComponent" in cn:
                for pn in ("control_points", "segments"):
                    try:
                        arr = c.get_editor_property(pn)
                        w("       %s: %d 个" % (pn, len(arr)))
                        for item in list(arr)[:3]:
                            w("         --- 样例 ---")
                            try_props(item,
                                      ["width", "side_falloff", "end_falloff",
                                       "left_side_falloff_factor",
                                       "right_side_falloff_factor",
                                       "raise_terrain", "lower_terrain",
                                       "b_raise_terrain", "b_lower_terrain"],
                                      indent="           ")
                    except Exception as exc:
                        w("       %s ! %s" % (pn, str(exc)[:70]))
        if not found:
            w("    (这个 actor 上没有样条组件)")
        w("")

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    unreal.log("[check_layers] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[check_layers] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
