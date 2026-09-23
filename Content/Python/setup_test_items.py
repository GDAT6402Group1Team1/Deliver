"""Create drag-and-drop test items and import the connected cartoon hotbar artwork."""

import os
import unreal


TEST_FOLDER = "/Game/Blueprint/Item/Test"
UI_TEXTURE_FOLDER = "/Game/UI/Inventory/Textures"


def create_blueprint(name, parent_class_path):
    asset_path = TEST_FOLDER + "/" + name
    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        unreal.log("[TestItemsSetup] Exists: " + asset_path)
        return

    parent_class = unreal.load_class(None, parent_class_path)
    if not parent_class:
        raise RuntimeError("Missing compiled class: " + parent_class_path)

    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", parent_class)
    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, TEST_FOLDER, unreal.Blueprint, factory)
    if not asset:
        raise RuntimeError("Failed to create " + asset_path)
    unreal.EditorAssetLibrary.save_loaded_asset(asset)
    unreal.log("[TestItemsSetup] Created: " + asset_path)


def import_hotbar_texture():
    source = os.path.join(
        unreal.Paths.project_content_dir(), "UI", "Inventory", "Source",
        "T_HotbarConnectedCartoon.png")
    if not os.path.isfile(source):
        raise RuntimeError("Missing generated hotbar texture: " + source)

    task = unreal.AssetImportTask()
    task.set_editor_property("filename", source)
    task.set_editor_property("destination_path", UI_TEXTURE_FOLDER)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    if not task.get_editor_property("imported_object_paths"):
        raise RuntimeError("Failed to import connected hotbar texture")
    unreal.log("[TestItemsSetup] Imported connected cartoon hotbar texture")


create_blueprint("BP_TestAxe", "/Script/Delivery.DeliveryTestAxe")
create_blueprint("BP_TestDeliveryPackage", "/Script/Delivery.DeliveryTestPackage")
import_hotbar_texture()
unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
unreal.log("[TestItemsSetup] PASS: test items and connected hotbar artwork are ready")
