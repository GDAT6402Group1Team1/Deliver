# -*- coding: utf-8 -*-
"""把手机图标导入 `/Game/UI/Phone/Textures` 并配好 UI 贴图参数。

在编辑器控制台（Cmd 模式）跑：
    py import_phone_icon.py

源图由 `generate_phone_icon.py` 离线生成，**那一步不在编辑器里跑**
（纯 Python 的逐像素循环会把游戏线程卡住）。

—— 四个参数一个都不能漏，漏哪个的症状都不一样 ——

照搬 `import_compass_icons.py` 里已经验证过的那套：

  * 不设 `TEXTUREGROUP_UI` → 引擎按默认组压缩，描边上出现脏边
  * 不设 `TC_EDITOR_ICON` → 走 DXT 有损压缩，大色块之间出现色带
  * 不关 mip（`NoMipmaps`）→ 在 HUD 上缩小显示时糊成一团
  * 不勾 `srgb` → 整张图发暗，而且**暗得很均匀**，看着像颜色就是这么设计的
"""

import os

import unreal


HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.normpath(
    os.path.join(HERE, "..", "UI", "Phone", "Source", "T_IconPhone.png"))
DEST = "/Game/UI/Phone/Textures"
NAME = "T_IconPhone"


def _configure(texture) -> None:
    texture.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI)
    texture.set_editor_property(
        "compression_settings", unreal.TextureCompressionSettings.TC_EDITOR_ICON)
    texture.set_editor_property(
        "mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    texture.set_editor_property("srgb", True)
    texture.set_editor_property("never_stream", True)


def run() -> None:
    if not os.path.exists(SOURCE):
        unreal.log_error(
            "[手机图标] 找不到源图 %s。先在命令行跑 "
            "`python Content/Python/generate_phone_icon.py`。" % SOURCE)

        return

    task = unreal.AssetImportTask()
    task.set_editor_property("filename", SOURCE)
    task.set_editor_property("destination_path", DEST)
    task.set_editor_property("destination_name", NAME)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    # **必须显式指定 factory。** UE 5.8 默认走 Interchange，那条路不读这里的设置；
    # 和 setup_motorbike.py 里 FBX 那个坑是同一件事。
    task.factory = unreal.TextureFactory()

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

    path = "%s/%s" % (DEST, NAME)
    texture = unreal.load_asset(path)
    if not texture:
        unreal.log_error(
            "[手机图标] %s 导入后载不进来；空产出多半是 factory 没生效。" % path)

        return

    _configure(texture)
    unreal.EditorAssetLibrary.save_loaded_asset(texture, only_if_is_dirty=False)

    # 回读验证。set_editor_property 失败时不一定抛异常（本项目已经栽过几次），
    # "调用过了"证明不了"写进去了"。
    unreal.log("[手机图标] 已导入 %s" % path)
    for prop in ("lod_group", "compression_settings", "mip_gen_settings", "srgb"):
        unreal.log("[手机图标]   回读 %-22s = %s" % (prop, texture.get_editor_property(prop)))


if __name__ == "__main__":
    run()
