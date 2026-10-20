# -*- coding: utf-8 -*-
"""将在用的小地图绑定到原生圆形地图；不删除主 HUD 内的引用。"""
import unreal

path = "/Game/UI/Minimap/WBP_Minimap"
bp = unreal.EditorAssetLibrary.load_asset(path)
parent = unreal.load_class(None, "/Script/Delivery.DeliveryMinimapWidget")
if not bp or not parent:
    raise RuntimeError("找不到小地图或新父类，请先完整编译 C++")
unreal.BlueprintEditorLibrary.reparent_blueprint(bp, parent)
unreal.BlueprintEditorLibrary.compile_blueprint(bp)
status = str(bp.get_editor_property("status"))
if "UP_TO_DATE" not in status:
    raise RuntimeError("小地图编译未通过：" + status)
if not unreal.EditorAssetLibrary.save_loaded_asset(bp, only_if_is_dirty=False):
    raise RuntimeError("小地图存盘失败")
cdo = unreal.get_default_object(bp.generated_class())
if not isinstance(cdo, unreal.DeliveryMinimapWidget):
    raise RuntimeError("小地图没有继承新的原生父类")
unreal.log("MINIMAP_UPGRADE_OK: " + parent.get_path_name() + " status=" + status
           + " diameter=" + str(cdo.get_editor_property("map_diameter"))
           + " world_width=" + str(cdo.get_editor_property("map_world_width")))
# 主 HUD 仍使用同一份 WBP_Minimap；重编译验证嵌套引用。
main = unreal.EditorAssetLibrary.load_asset("/Game/UI/WBP_Main")
unreal.BlueprintEditorLibrary.compile_blueprint(main)
if "UP_TO_DATE" not in str(main.get_editor_property("status")):
    raise RuntimeError("主 HUD 编译未通过")
unreal.log("MINIMAP_MAIN_HUD_OK")
