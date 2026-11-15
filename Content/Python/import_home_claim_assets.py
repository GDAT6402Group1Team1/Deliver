# -*- coding: utf-8 -*-
"""导入房屋解锁反馈用的星光贴图、门口标记和确认音，已有资产保持不动"""
from pathlib import Path

import unreal


ROOT = Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())).resolve()
SELECTED = ROOT / "Design/Art/VFX/HomeClaim/Selected"
DESTINATION = "/Game/UI/HomeClaim"
# 确认音从四条里先用第二条，试听后想换就改这里并删掉旧资产重跑
ASSETS = (
    ("T_HomeClaim_Star01", "star_01.png"),
    ("T_HomeClaim_Star02", "star_02.png"),
    ("T_HomeClaim_Marker", "home_marker.png"),
    ("SFX_HomeClaim_Confirm", "confirmation_002.wav"),
)


def _on_disk(name):
    return (ROOT / "Content/UI/HomeClaim" / (name + ".uasset")).is_file()


def run():
    tasks = []
    for name, source in ASSETS:
        path = SELECTED / source
        if _on_disk(name):
            continue
        if not path.is_file():
            unreal.log_warning(f"房屋解锁素材缺失：{path}")
            continue
        task = unreal.AssetImportTask()
        task.filename = str(path)
        task.destination_path = DESTINATION
        task.destination_name = name
        task.automated = True
        task.replace_existing = True
        task.save = True
        tasks.append(task)
    if tasks:
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)
    for name, _ in ASSETS:
        asset = unreal.load_asset(f"{DESTINATION}/{name}")
        if isinstance(asset, unreal.Texture2D):
            # 界面贴图不压缩、不生成多级纹理，星光边缘才干净
            asset.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_EDITOR_ICON)
            asset.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
            asset.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI)
            asset.set_editor_property("srgb", True)
        if asset and not _on_disk(name):
            unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False)
    missing = [name for name, _ in ASSETS if not _on_disk(name)]
    if missing:
        unreal.log_warning(f"房屋解锁素材仍未导入：{missing}")
    else:
        unreal.log("房屋解锁素材已就绪：/Game/UI/HomeClaim")
    return not missing


if __name__ == "__main__":
    run()
