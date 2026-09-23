"""Read-only validation for the core delivery-loop assets and E/F input separation."""

import unreal


def fail(message):
    raise RuntimeError("Delivery core validation failed: " + message)


for asset_path in (
    "/Game/Input/Actions/IA_Pickup",
    "/Game/Blueprint/Item/Test/BP_TestDeliveryPackage",
    "/Game/Blueprint/Task/BP_TestDeliveryTarget",
    "/Game/Task/Definitions/DA_Task_001",
    "/Game/UI/Inventory/Textures/T_HotbarConnectedCartoon",
):
    if not unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        fail("missing " + asset_path)

imc = unreal.EditorAssetLibrary.load_asset("/Game/Input/IMC_Default")
pairs = []
for mapping in imc.get_editor_property("default_key_mappings").get_editor_property("mappings"):
    action = mapping.get_editor_property("action")
    key = mapping.get_editor_property("key")
    pairs.append((action.get_name() if action else "None", str(key.get_editor_property("key_name"))))
if ("IA_Pickup", "E") not in pairs:
    fail("E -> IA_Pickup is missing")
if ("IA_Interact", "F") not in pairs:
    fail("F -> IA_Interact is missing")

unreal.log("[DeliveryCoreValidation] PASS: E/F, drag-and-drop package BP, target BP, task asset and connected hotbar texture")
