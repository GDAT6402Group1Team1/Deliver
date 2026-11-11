"""Opt the production axe into the native swing without reparenting or touching its art."""
import unreal

path = "/Game/Blueprint/Item/BP_Axe"
bp = unreal.EditorAssetLibrary.load_asset(path)
if not bp:
    raise RuntimeError("Missing production BP_Axe")
cdo = unreal.get_default_object(bp.generated_class())
if not isinstance(cdo, unreal.DeliveryHandheldItem):
    raise RuntimeError("BP_Axe must derive from DeliveryHandheldItem")
cdo.set_editor_property("uses_axe_swing", True)
# futou 斧柄为 +Y，刀刃朝 -X；握持空间统一为柄 +Z、刃 +X
# 模型缩放 0.2，握点取柄尾上方 45 cm，折算成游戏空间 9 cm
q = unreal.Quat(0.0, 0.70710678, 0.70710678, 0.0)
cdo.set_editor_property("held_relative_transform", unreal.Transform(location=unreal.Vector(-5.0,0.0,-9.0), rotation=q.rotator(), scale=unreal.Vector(1,1,1)))
unreal.BlueprintEditorLibrary.compile_blueprint(bp)
if not unreal.EditorAssetLibrary.save_loaded_asset(bp, only_if_is_dirty=False):
    raise RuntimeError("Failed to save BP_Axe")
assert unreal.get_default_object(bp.generated_class()).get_editor_property("uses_axe_swing")
unreal.log("AXE_SETUP_OK: BP_Axe native swing enabled; grip calibrated; mesh/material assets preserved")
