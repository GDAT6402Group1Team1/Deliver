"""Import the generated axe and package hotbar icons without changing any level."""

import os
import unreal


SOURCE_DIR = os.path.join(unreal.Paths.project_content_dir(), "UI", "Inventory", "Source")
DESTINATION = "/Game/UI/Inventory/Textures"
ICON_NAMES = ("T_ItemIconAxe", "T_ItemIconPackage")


for name in ICON_NAMES:
    asset_path = DESTINATION + "/" + name
    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        unreal.log("[HotbarIcons] Existing: " + asset_path)
        continue

    source = os.path.join(SOURCE_DIR, name + ".png")
    if not os.path.isfile(source):
        raise RuntimeError("Missing generated icon: " + source)

    task = unreal.AssetImportTask()
    task.set_editor_property("filename", source)
    task.set_editor_property("destination_path", DESTINATION)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", False)
    task.set_editor_property("save", True)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    if not unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        raise RuntimeError("Could not import: " + asset_path)
    unreal.log("[HotbarIcons] Imported: " + asset_path)

for blueprint_name, icon_name in (
    ("BP_TestAxe", "T_ItemIconAxe"),
    ("BP_TestDeliveryPackage", "T_ItemIconPackage"),
):
    blueprint_path = "/Game/Blueprint/Item/Test/" + blueprint_name
    blueprint = unreal.EditorAssetLibrary.load_asset(blueprint_path)
    if not blueprint or not blueprint.generated_class():
        raise RuntimeError("Missing test item Blueprint: " + blueprint_path)
    default_item = unreal.get_default_object(blueprint.generated_class()).get_item_component()
    if not default_item or default_item.get_editor_property("icon") != unreal.EditorAssetLibrary.load_asset(
        DESTINATION + "/" + icon_name
    ):
        raise RuntimeError("Test item has no expected hotbar icon: " + blueprint_name)
    unreal.log("[HotbarIcons] Bound: " + blueprint_name + " -> " + icon_name)

unreal.log("[HotbarIcons] PASS: axe and package image assets are ready")
