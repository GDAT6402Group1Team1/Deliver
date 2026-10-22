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
unreal.BlueprintEditorLibrary.compile_blueprint(bp)
if not unreal.EditorAssetLibrary.save_loaded_asset(bp, only_if_is_dirty=False):
    raise RuntimeError("Failed to save BP_Axe")
assert unreal.get_default_object(bp.generated_class()).get_editor_property("uses_axe_swing")
unreal.log("AXE_SETUP_OK: BP_Axe native swing enabled; mesh, materials, parent and map untouched")
