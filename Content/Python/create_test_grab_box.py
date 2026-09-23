"""Create a draggable two-hand grab test box without touching any level."""

import unreal


FOLDER = "/Game/Blueprint/Item/Test"
ASSET_PATH = FOLDER + "/BP_TestGrabBox"
PARENT_PATH = "/Script/Delivery.DeliveryGrabbableProp"


if unreal.EditorAssetLibrary.does_asset_exist(ASSET_PATH):
    blueprint = unreal.EditorAssetLibrary.load_asset(ASSET_PATH)
    unreal.log("[GrabBoxSetup] Existing asset: " + ASSET_PATH)
else:
    parent_class = unreal.load_class(None, PARENT_PATH)
    if not parent_class:
        raise RuntimeError("Compile DeliveryEditor first: " + PARENT_PATH)

    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", parent_class)
    blueprint = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        "BP_TestGrabBox", FOLDER, unreal.Blueprint, factory)
    if not blueprint:
        raise RuntimeError("Could not create " + ASSET_PATH)
    unreal.BlueprintEditorLibrary.compile_blueprint(blueprint)
    if not unreal.EditorAssetLibrary.save_loaded_asset(blueprint):
        raise RuntimeError("Could not save " + ASSET_PATH)
    unreal.log("[GrabBoxSetup] Created: " + ASSET_PATH)

if not blueprint or not blueprint.generated_class():
    raise RuntimeError("Test grab box has no generated class")
unreal.log("[GrabBoxSetup] PASS: drag BP_TestGrabBox into a level to test two-hand grab")
