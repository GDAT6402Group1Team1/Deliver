# -*- coding: utf-8 -*-
"""列出地形材质的目标图层，判断"把路刷进地形"这条路要不要先改材质。

EditorApplySpline 的 PaintLayer 参数需要一个 ULandscapeLayerInfoObject。
如果地形材质里已经有道路图层，直接传进去就能边压平边刷路；
如果只有草地图层，就得先在材质里加一层道路，并建对应的 LayerInfo 资产。

用法：py check_landscape_target_layers.py
"""

import traceback

import unreal

OUT = unreal.Paths.project_saved_dir() + "target_layers.txt"
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
    w("")

    w("--- 地形材质的目标图层 (get_target_layer_names) ---")
    try:
        names = land.get_target_layer_names()
        w("共 %d 个: %s" % (len(names), [str(n) for n in names]))
    except Exception as exc:
        w("调用失败: %s" % str(exc)[:100])
    w("")

    w("--- 地形材质 ---")
    for pn in ("landscape_material", "landscape_hole_material"):
        try:
            m = land.get_editor_property(pn)
            w("   %-26s = %s" % (pn, m.get_name() if m else None))
        except Exception as exc:
            w("   %-26s ! %s" % (pn, str(exc)[:60]))
    w("")

    w("--- 项目里现有的 LandscapeLayerInfoObject 资产 ---")
    try:
        ar = unreal.AssetRegistryHelpers.get_asset_registry()
        found = 0
        for d in ar.get_assets_by_path("/Game", recursive=True):
            cn = str(d.asset_class_path.asset_name) if hasattr(d, "asset_class_path") else ""
            if "LandscapeLayerInfo" in cn:
                w("   %s   (%s)" % (d.asset_name, d.package_name))
                found += 1
        if not found:
            w("   （一个都没有）")
    except Exception:
        w(traceback.format_exc())

    w("")
    w("=" * 62)
    w("判断：目标图层里如果已有 road/asphalt 之类的图层，")
    w("      就能直接用 EditorApplySpline 的 PaintLayer 参数边压平边刷路；")
    w("      只有 grass 的话，得先给地形材质加一层道路 + 建 LayerInfo 资产。")

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    unreal.log("[layers] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[layers] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
