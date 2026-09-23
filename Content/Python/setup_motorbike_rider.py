"""只迁移骑手动画资产，不导入模型、不改输入、不保存关卡。可重复执行。"""
import unreal

ROOT = "/Game/Vehicle/Motorbike"
mesh = unreal.load_asset(ROOT + "/SK_MotorbikeRider")
bp = unreal.load_asset(ROOT + "/BP_Motorbike")
assert mesh and bp, "Missing motorbike assets"
parent = unreal.load_class(None, "/Script/Delivery.DeliveryRiderAnimInstance")
anim = unreal.load_asset(ROOT + "/ABP_MotorbikeRider")
if not anim:
    factory = unreal.AnimBlueprintFactory()
    factory.set_editor_property("parent_class", parent)
    factory.set_editor_property("target_skeleton", mesh.get_editor_property("skeleton"))
    anim = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        "ABP_MotorbikeRider", ROOT, unreal.AnimBlueprint, factory)
assert anim
unreal.BlueprintEditorLibrary.compile_blueprint(anim)
bp.modify(True)
unreal.BlueprintEditorLibrary.compile_blueprint(bp)
cdo = unreal.get_default_object(bp.generated_class())
cdo.modify(True)
cdo.set_editor_property("rider_animation_class", anim.generated_class())
component = cdo.get_editor_property("rider_mesh")
component.modify(True)
assert isinstance(component, unreal.SkeletalMeshComponent), str(component)
component.set_skeletal_mesh_asset(mesh)
component.set_anim_instance_class(anim.generated_class())
unreal.EditorAssetLibrary.save_loaded_asset(anim, False)
unreal.EditorAssetLibrary.save_loaded_asset(bp, False)
unreal.log("RIDER_MIGRATION_OK: " + component.get_class().get_name())
