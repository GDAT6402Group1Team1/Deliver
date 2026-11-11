"""配置毛毛虫拾取蓝图、外卖显示、第一单倍率和院子中的实例；可重复执行"""
import unreal
from pathlib import Path

assets = unreal.AssetToolsHelpers.get_asset_tools()
mesh_path = '/Game/Blueprint/Item/Caterpillar/SM_Caterpillar'
mesh = unreal.load_asset(mesh_path)
if not mesh:
    mesh = unreal.EditorAssetLibrary.duplicate_asset('/Game/model/caterpillar', mesh_path)
sm = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
if not sm: raise RuntimeError('Run setup_caterpillar.py inside the full editor')
sm.remove_collisions(mesh)
sm.add_simple_collisions(mesh, unreal.ScriptingCollisionShapeType.BOX)
mesh.get_editor_property('body_setup').set_editor_property('collision_trace_flag', unreal.CollisionTraceFlag.CTF_USE_SIMPLE_AND_COMPLEX)
unreal.EditorAssetLibrary.save_loaded_asset(mesh, False)

icon_path = '/Game/UI/Inventory/Textures/T_ItemIconCaterpillar'
icon = unreal.load_asset(icon_path)
if not icon:
    imp = unreal.AssetImportTask()
    imp.filename = str(Path(unreal.Paths.project_content_dir()) / 'UI/Inventory/Source/T_ItemIconCaterpillar.png')
    imp.destination_path = '/Game/UI/Inventory/Textures'
    imp.automated = True
    imp.save = True
    assets.import_asset_tasks([imp])
    icon = unreal.load_asset(icon_path)
    if not icon: raise RuntimeError('Caterpillar icon import failed')
    icon.set_editor_property('compression_settings', unreal.TextureCompressionSettings.TC_EDITOR_ICON)
    icon.set_editor_property('lod_group', unreal.TextureGroup.TEXTUREGROUP_UI)
    unreal.EditorAssetLibrary.save_loaded_asset(icon, False)

bp_path = '/Game/Blueprint/Item/BP_Caterpillar'
bp = unreal.load_asset(bp_path)
if not bp:
    factory = unreal.BlueprintFactory()
    factory.set_editor_property('parent_class', unreal.DeliveryCaterpillarItem)
    bp = assets.create_asset('BP_Caterpillar', '/Game/Blueprint/Item', unreal.Blueprint, factory)
cdo = unreal.get_default_object(bp.generated_class())
cdo.get_item_mesh().set_static_mesh(mesh)
bounds = mesh.get_bounding_box()
size = bounds.max - bounds.min
scale = 22.0 / max(size.x, size.y, size.z, 1.0)
cdo.get_item_mesh().set_relative_scale3d(unreal.Vector(scale, scale, scale))
cdo.get_item_component().set_editor_property('icon', icon)
center = (bounds.min + bounds.max) * .5
cdo.set_editor_property('held_relative_transform', unreal.Transform(location=unreal.Vector(5,0,0)-center*scale))
unreal.BlueprintEditorLibrary.compile_blueprint(bp)
unreal.EditorAssetLibrary.save_loaded_asset(bp, False)

food_bp = unreal.load_asset('/Game/Blueprint/Item/BP_DeliveryPackage')
food = unreal.get_default_object(food_bp.generated_class())
food.get_editor_property('caterpillar_visual').set_static_mesh(mesh)
unreal.BlueprintEditorLibrary.compile_blueprint(food_bp)
unreal.EditorAssetLibrary.save_loaded_asset(food_bp, False)

task = unreal.load_asset('/Game/Task/Definitions/DA_Task_001')
tag = unreal.GameplayTag()
tag.import_text('(TagName="Event.Delivery.Caterpillar")')
rules = list(task.get_editor_property('special_event_rules'))
if not any(r.event_tag == tag for r in rules):
    rule = unreal.DeliverySpecialEventRule()
    rule.set_editor_property('event_tag', tag)
    rule.set_editor_property('multiplier', 1.25)
    rule.set_editor_property('display_name', 'Caterpillar prank')
    rules.append(rule)
    task.set_editor_property('special_event_rules', rules)
unreal.EditorAssetLibrary.save_loaded_asset(task, False)

unreal.EditorLoadingAndSavingUtils.load_map('/Game/Level/TestForCharacter')
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
label = 'SceneObject_Caterpillar_001'
found = [a for a in actors.get_all_level_actors() if a.get_actor_label() == label]
if len(found) > 1: raise RuntimeError('Duplicate caterpillar placement')
actor = found[0] if found else actors.spawn_actor_from_class(bp.generated_class(), unreal.Vector(10000,13250,2050))
actor.set_actor_label(label)
# 门前院子靠右，尺寸 22 cm；保存位置后进游戏以真实物理落地
actor.set_actor_location(unreal.Vector(10000,13250,2050), False, True)
unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()
print('CATERPILLAR_SETUP_OK', scale, bounds, task.get_editor_property('special_event_rules'))
