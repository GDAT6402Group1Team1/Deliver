"""Create/import and place the core delivery-loop test assets. Run after compiling DeliveryEditor."""

import os
import unreal

PACKAGE_BP = "/Game/Blueprint/Item/Test/BP_TestDeliveryPackage"
TARGET_BP = "/Game/Blueprint/Task/BP_TestDeliveryTarget"
TEST_MAP = "/Game/Level/TestForCharacter"
TEXTURE_DESTINATION = "/Game/UI/Inventory/Textures"


def log(message):
    unreal.log("[DeliveryCoreSetup] " + message)


def create_blueprint(path, parent_class):
    existing = unreal.EditorAssetLibrary.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
    if existing:
        return existing
    package, name = path.rsplit("/", 1)
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", parent_class)
    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, package, unreal.Blueprint, factory)
    if not asset:
        raise RuntimeError("Failed to create " + path)
    unreal.EditorAssetLibrary.save_loaded_asset(asset)
    log("Created " + path)
    return asset


def import_hotbar_texture(filename):
    source = os.path.join(unreal.Paths.project_content_dir(), "UI", "Inventory", "Source", filename)
    if not os.path.isfile(source):
        raise RuntimeError("Missing generated hotbar source texture: " + source)
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", source)
    task.set_editor_property("destination_path", TEXTURE_DESTINATION)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    if not task.get_editor_property("imported_object_paths"):
        raise RuntimeError("Failed to import " + source)
    log("Imported " + filename)


import_hotbar_texture("T_HotbarConnectedCartoon.png")


package_parent = unreal.load_class(None, "/Script/Delivery.DeliveryTestPackage")
target_parent = unreal.load_class(None, "/Script/Delivery.DeliveryTestTarget")
if not package_parent or not target_parent:
    raise RuntimeError("Delivery core C++ classes are unavailable; compile DeliveryEditor first")

package_bp = create_blueprint(PACKAGE_BP, package_parent)
target_bp = create_blueprint(TARGET_BP, target_parent)

unreal.EditorLoadingAndSavingUtils.load_map(TEST_MAP)
actors_api = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
actors = actors_api.get_all_level_actors()
starts = [actor for actor in actors if isinstance(actor, unreal.PlayerStart)]
origin = starts[0].get_actor_location() if starts else unreal.Vector(0.0, 0.0, 180.0)
forward = starts[0].get_actor_forward_vector() if starts else unreal.Vector(1.0, 0.0, 0.0)
right = starts[0].get_actor_right_vector() if starts else unreal.Vector(0.0, 1.0, 0.0)

labels = {actor.get_actor_label(): actor for actor in actors}
if "DeliveryTestPackage_01" not in labels:
    package_location = origin + forward * 250.0 - right * 130.0 + unreal.Vector(0.0, 0.0, 35.0)
    package = actors_api.spawn_actor_from_class(
        package_bp.generated_class(), package_location, unreal.Rotator(0.0, 0.0, 0.0))
    package.set_actor_label("DeliveryTestPackage_01")

if "DeliveryTestTarget_01" not in labels:
    target_location = origin + forward * 700.0 + unreal.Vector(0.0, 0.0, 65.0)
    target = actors_api.spawn_actor_from_class(
        target_bp.generated_class(), target_location, unreal.Rotator(0.0, 0.0, 0.0))
    target.set_actor_label("DeliveryTestTarget_01")

unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
log("PASS: package and delivery target are ready in TestForCharacter")
