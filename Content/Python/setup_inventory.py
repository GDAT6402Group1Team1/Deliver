"""Create inventory editor assets without touching the existing F interaction mapping.

Run in Unreal Editor:
    py "D:/Udev/Delivery/Content/Python/setup_inventory.py"
"""

import unreal

PICKUP_IA = "/Game/Input/Actions/IA_Pickup"
TEMPLATE_IA = "/Game/Input/Actions/IA_Interact"
IMC_DEFAULT = "/Game/Input/IMC_Default"
AXE_BP = "/Game/Blueprint/Inventory/BP_TestAxe"
HOTBAR_WBP = "/Game/UI/Inventory/WBP_DeliveryHotbar"
TEST_MAP = "/Game/Level/TestForCharacter"


def log(message):
    unreal.log("[InventorySetup] " + message)


def make_key(name):
    for factory in (lambda: unreal.Key(name), lambda: unreal.Key(key_name=name), lambda: unreal.Key()):
        try:
            key = factory()
            if str(key.get_editor_property("key_name")) != name:
                key.set_editor_property("key_name", name)
            return key
        except Exception:
            pass
    return None


def create_blueprint(path, parent_class, widget=False):
    existing = unreal.EditorAssetLibrary.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
    if existing:
        log("Asset already exists: " + path)
        return existing
    package, name = path.rsplit("/", 1)
    factory = unreal.WidgetBlueprintFactory() if widget else unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", parent_class)
    asset_class = unreal.WidgetBlueprint if widget else unreal.Blueprint
    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, package, asset_class, factory)
    if asset:
        unreal.EditorAssetLibrary.save_loaded_asset(asset)
        log("Created: " + path)
    else:
        unreal.log_error("[InventorySetup] Failed to create: " + path)
    return asset


def setup_input():
    pickup = unreal.EditorAssetLibrary.load_asset(PICKUP_IA)
    if not pickup:
        pickup = unreal.EditorAssetLibrary.duplicate_asset(TEMPLATE_IA, PICKUP_IA)
    if not pickup:
        raise RuntimeError("Could not create IA_Pickup")
    pickup.set_editor_property("value_type", unreal.InputActionValueType.BOOLEAN)
    pickup.set_editor_property("triggers", [])
    pickup.set_editor_property("modifiers", [])
    unreal.EditorAssetLibrary.save_loaded_asset(pickup)

    imc = unreal.EditorAssetLibrary.load_asset(IMC_DEFAULT)
    key = make_key("E")
    if not imc or not key:
        raise RuntimeError("Could not load IMC_Default or create E key")
    imc.modify(True)
    try:
        imc.unmap_key(pickup, key)
    except Exception:
        pass
    imc.map_key(pickup, key)

    # F remains the independent general interaction path (motorbike, doors, etc.).
    interact = unreal.EditorAssetLibrary.load_asset(TEMPLATE_IA)
    f_key = make_key("F")
    if not interact or not f_key:
        raise RuntimeError("Could not preserve F -> IA_Interact")
    try:
        imc.unmap_key(interact, f_key)
    except Exception:
        pass
    mapped_f = imc.map_key(interact, f_key)
    mapping_data = imc.get_editor_property("default_key_mappings")
    in_memory = []
    for entry in mapping_data.get_editor_property("mappings"):
        action = entry.get_editor_property("action")
        entry_key = entry.get_editor_property("key")
        in_memory.append((action.get_name() if action else "None", str(entry_key.get_editor_property("key_name"))))
    log("F mapping result=%s; in-memory=%s" % (mapped_f, in_memory))
    # UE 5.8 keeps the serializable mappings inside this wrapper struct. Assign it back so
    # the second mapping is not lost when the package is reloaded.
    imc.set_editor_property("default_key_mappings", mapping_data)
    unreal.EditorAssetLibrary.save_loaded_asset(imc)
    log("Mapped E -> IA_Pickup and preserved independent F -> IA_Interact.")


def setup_assets():
    axe_parent = unreal.load_class(None, "/Script/Delivery.DeliveryTestAxe")
    hotbar_parent = unreal.load_class(None, "/Script/Delivery.DeliveryHotbarWidget")
    if not axe_parent or not hotbar_parent:
        raise RuntimeError("Inventory C++ classes are unavailable; compile the editor target first")
    axe = create_blueprint(AXE_BP, axe_parent)
    create_blueprint(HOTBAR_WBP, hotbar_parent, widget=True)
    return axe


def place_test_axes(axe_bp):
    unreal.EditorLoadingAndSavingUtils.load_map(TEST_MAP)
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = actor_subsystem.get_all_level_actors()
    existing = [a for a in actors if a.get_actor_label().startswith("InventoryTestAxe_")]

    starts = [a for a in actors if isinstance(a, unreal.PlayerStart)]
    origin = starts[0].get_actor_location() if starts else unreal.Vector(0.0, 0.0, 180.0)
    forward = starts[0].get_actor_forward_vector() if starts else unreal.Vector(1.0, 0.0, 0.0)
    right = starts[0].get_actor_right_vector() if starts else unreal.Vector(0.0, 1.0, 0.0)
    axe_class = axe_bp.generated_class()
    # Six instances fill all five slots and leave one extra for the red "full" feedback check.
    for index in range(len(existing), 6):
        offset = forward * 180.0 + right * ((index - 2) * 55.0) + unreal.Vector(0.0, 0.0, 50.0)
        actor = actor_subsystem.spawn_actor_from_class(axe_class, origin + offset, unreal.Rotator(0.0, 0.0, 0.0))
        actor.set_actor_label("InventoryTestAxe_%02d" % (index + 1))
    unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
    log("Test axes ready in TestForCharacter: 6")


setup_input()
axe_blueprint = setup_assets()
place_test_axes(axe_blueprint)
log("Inventory setup complete")
