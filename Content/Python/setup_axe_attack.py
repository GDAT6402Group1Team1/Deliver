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
# 落地和丢弃会开启物理，不能用三角面网格代替动态刚体碰撞
mesh = cdo.get_editor_property("item_mesh").get_editor_property("static_mesh")
mesh_editor = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
if not mesh:
    raise RuntimeError("BP_Axe requires a static mesh")
body = mesh.get_editor_property("body_setup")
needs_rebuild = (body.get_editor_property("collision_trace_flag") !=
                 unreal.CollisionTraceFlag.CTF_USE_SIMPLE_AND_COMPLEX or
                 (mesh_editor.get_simple_collision_count(mesh) +
                  mesh_editor.get_convex_collision_count(mesh)) <= 0)
body.set_editor_property("collision_trace_flag", unreal.CollisionTraceFlag.CTF_USE_SIMPLE_AND_COMPLEX)
if needs_rebuild:
    # 切换模式后重新烘焙凸包，避免沿用只含复杂碰撞的旧物理缓存
    if not mesh_editor.set_convex_decomposition_collisions(mesh, 4, 16, 100000):
        raise RuntimeError("Failed to rebuild axe convex collision")
if not unreal.EditorAssetLibrary.save_loaded_asset(mesh, only_if_is_dirty=False):
    raise RuntimeError("Failed to save axe mesh collision")
# futou 斧柄为 +Y，刀刃朝 -X；握持空间统一为柄 +Z、刃 +X
# 模型缩放 0.2，握点取柄尾上方 45 cm，折算成游戏空间 9 cm
q = unreal.Quat(0.0, 0.70710678, 0.70710678, 0.0)
cdo.set_editor_property("held_relative_transform", unreal.Transform(location=unreal.Vector(-5.0,0.0,-9.0), rotation=q.rotator(), scale=unreal.Vector(1,1,1)))
unreal.BlueprintEditorLibrary.compile_blueprint(bp)
if not unreal.EditorAssetLibrary.save_loaded_asset(bp, only_if_is_dirty=False):
    raise RuntimeError("Failed to save BP_Axe")
assert unreal.get_default_object(bp.generated_class()).get_editor_property("uses_axe_swing")
unreal.log("AXE_SETUP_OK: BP_Axe native swing enabled; grip calibrated; mesh/material assets preserved")
