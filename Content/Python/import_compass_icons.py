# -*- coding: utf-8 -*-
"""把罗盘图钉的 PNG 导入成 UI 贴图。

在编辑器控制台跑：
    py import_compass_icons.py

**这个脚本掉过两次**（未跟踪文件在 `git pull` 的 autostash 恢复失败里没保住），
所以要跟着一起提交，别再留在工作区。
"""

import os

import unreal


SOURCE_DIR = os.path.join(
    unreal.Paths.project_content_dir(), "UI", "Minimap", "Source")
DEST = "/Game/UI/Minimap/Source"

# (源文件, 资产名)
ICONS = [
    ("T_CompassTaskTarget.png", "T_CompassTaskTarget"),
    ("T_CompassTaskDelivery.png", "T_CompassTaskDelivery"),
]


def _apply_ui_settings(texture):
    """UI 贴图的三项设置，漏任一项的表现各不相同，都见过：

      * 不设 `UserInterface2D` → 走默认压缩，描边出现脏边
      * 不关 mip → 罗盘上缩小显示时糊成一团
      * 不开 sRGB → 整张发暗
    """
    texture.set_editor_property(
        "lod_group", unreal.TextureGroup.TEXTUREGROUP_UI)
    texture.set_editor_property(
        "compression_settings", unreal.TextureCompressionSettings.TC_EDITOR_ICON)
    texture.set_editor_property("mip_gen_settings",
                                unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    texture.set_editor_property("srgb", True)
    texture.set_editor_property("never_stream", True)


def run():
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    done = []

    unreal.log("[图标] 源目录: %s（存在=%s）" % (SOURCE_DIR, os.path.isdir(SOURCE_DIR)))

    for file_name, asset_name in ICONS:
        source = os.path.join(SOURCE_DIR, file_name)
        if not os.path.isfile(source):
            unreal.log_error(
                "[图标] 找不到 %s。送达图标要先在编辑器外跑 "
                "python Content/Python/generate_delivery_icon.py" % source)
            continue

        asset_path = "%s/%s" % (DEST, asset_name)

        task = unreal.AssetImportTask()
        task.filename = source
        task.destination_path = DEST
        task.destination_name = asset_name
        task.automated = True
        task.replace_existing = True
        task.save = True

        # **必须显式指定 factory**。UE 5.8 默认走 Interchange：
        # `AssetTools::ImportAssetTasks` 里
        # `bUseInterchangeFramework = IsInterchangeImportEnabled() && (SpecifiedFactory == nullptr)`
        # ——只要不指定就被路由过去，而那条路会**静默什么都不做**：
        # 没有报错、没有资产，只有一行看不出问题的日志。FBX 导入踩过一模一样的坑。
        task.factory = unreal.TextureFactory()

        tools.import_asset_tasks([task])

        # 别只看"函数调用过了"就当成功：import_asset_tasks 失败时不抛异常，
        # imported_object_paths 才是它实际产出了什么的唯一凭据
        produced = list(task.get_editor_property("imported_object_paths") or [])
        unreal.log("[图标] %s → 产出 %s" % (file_name, produced if produced else "（空！）"))

        texture = unreal.EditorAssetLibrary.load_asset(asset_path)
        if not texture:
            unreal.log_error(
                "[图标] %s 导入后载不进来。产出=%s；空产出多半是 factory 没生效"
                % (asset_path, produced))
            continue

        _apply_ui_settings(texture)
        unreal.EditorAssetLibrary.save_asset(asset_path)
        done.append(asset_path)
        unreal.log("[图标] %s 已导入并配好 UI 参数" % asset_path)

    unreal.log("[图标] 完成 %d 个" % len(done))


if __name__ == "__main__":
    run()
