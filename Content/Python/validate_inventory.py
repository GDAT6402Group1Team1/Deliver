"""Read-only editor validation for the first inventory milestone."""

import unreal


def fail(message):
    raise RuntimeError("Inventory validation failed: " + message)


pickup = unreal.EditorAssetLibrary.load_asset("/Game/Input/Actions/IA_Pickup")
interact = unreal.EditorAssetLibrary.load_asset("/Game/Input/Actions/IA_Interact")
imc = unreal.EditorAssetLibrary.load_asset("/Game/Input/IMC_Default")
if not pickup or not interact or not imc:
    fail("input assets are missing")

mapping_pairs = []
mapping_data = imc.get_editor_property("default_key_mappings")
for mapping in mapping_data.get_editor_property("mappings"):
    action = mapping.get_editor_property("action")
    key = mapping.get_editor_property("key")
    action_name = action.get_name() if action else "None"
    key_name = str(key.get_editor_property("key_name"))
    mapping_pairs.append((action_name, key_name))

unreal.log("[InventoryValidation] mappings=" + repr(mapping_pairs))

if ("IA_Pickup", "E") not in mapping_pairs:
    fail("E -> IA_Pickup is missing")
if ("IA_Interact", "F") not in mapping_pairs:
    fail("F -> IA_Interact was lost")

for asset_path in (
    "/Game/Blueprint/Item/Test/BP_TestAxe",
    "/Game/UI/Inventory/WBP_DeliveryHotbar",
):
    if not unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        fail("missing " + asset_path)

unreal.log("[InventoryValidation] PASS: E pickup, F interaction, UMG and drag-and-drop axe BP")
