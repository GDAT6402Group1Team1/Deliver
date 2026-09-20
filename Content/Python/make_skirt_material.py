# -*- coding: utf-8 -*-
"""创建路面裙边用的纯色材质 M_RoadSkirt。

思路来源：与其压地形或加厚灰色路面，不如在路面下方补一圈和地面同色的 mesh，
把压地形后露出的空隙挡住。纯加法，地形和路面都不用动，不满意删掉即可。

颜色做成 VectorParameter("Color")，之后可以直接在材质里拖颜色对齐地面，
不用改脚本重跑。

用法：py make_skirt_material.py
"""

import traceback

import unreal

PKG_PATH = "/Game/materials"
ASSET_NAME = "M_RoadSkirt"
# 先给一个偏黄绿的默认值，和截图里的地面接近；之后在材质里微调
DEFAULT_COLOR = (0.35, 0.62, 0.12)
ROUGHNESS = 0.95

OUT = unreal.Paths.project_saved_dir() + "skirt_material.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[skirt_mat] %s" % s)


def run():
    full = "%s/%s" % (PKG_PATH, ASSET_NAME)
    if unreal.EditorAssetLibrary.does_asset_exist(full):
        w("已存在: %s（不重复创建）" % full)
        mat = unreal.EditorAssetLibrary.load_asset(full)
    else:
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        mat = tools.create_asset(ASSET_NAME, PKG_PATH,
                                 unreal.Material, unreal.MaterialFactoryNew())
        if mat is None:
            w("!! 创建失败")
            return
        w("已创建: %s" % full)

        mel = unreal.MaterialEditingLibrary

        col = mel.create_material_expression(
            mat, unreal.MaterialExpressionVectorParameter, -400, 0)
        col.set_editor_property("parameter_name", "Color")
        col.set_editor_property("default_value", unreal.LinearColor(
            DEFAULT_COLOR[0], DEFAULT_COLOR[1], DEFAULT_COLOR[2], 1.0))
        mel.connect_material_property(col, "", unreal.MaterialProperty.MP_BASE_COLOR)

        rough = mel.create_material_expression(
            mat, unreal.MaterialExpressionScalarParameter, -400, 200)
        rough.set_editor_property("parameter_name", "Roughness")
        rough.set_editor_property("default_value", ROUGHNESS)
        mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)

        mel.recompile_material(mat)
        unreal.EditorAssetLibrary.save_asset(full)
        w("参数: Color=%s  Roughness=%.2f" % (str(DEFAULT_COLOR), ROUGHNESS))

    w("")
    w("颜色不对的话，直接双击 %s 改 Color 参数的默认值即可，" % full)
    w("不需要重跑脚本、也不用重新生成裙边。")

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[skirt_mat] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
