"""Add the shared gamepad bindings to IMC_Default without replacing keyboard/mouse mappings.

Run in Unreal Editor:
    py "D:/Udev/Delivery/Content/Python/setup_gamepad_input.py"
"""

import unreal


IMC_DEFAULT = "/Game/Input/IMC_Default"
ACTION_ROOT = "/Game/Input/Actions/"

# Left/right sticks and the bottom face button already live in the same IMC. These
# entries complete the actions that were keyboard/mouse-only.
GAMEPAD_BINDINGS = (
    ("IA_AttackLeft", "Gamepad_LeftTrigger"),
    ("IA_AttackRight", "Gamepad_RightTrigger"),
    ("IA_Pickup", "Gamepad_FaceButton_Left"),
    ("IA_Interact", "Gamepad_FaceButton_Top"),
    ("IA_TogglePhone", "Gamepad_DPad_Up"),
)


def log(message):
    unreal.log("[GamepadInputSetup] " + message)


def make_key(name):
    for factory in (lambda: unreal.Key(name), lambda: unreal.Key(key_name=name), lambda: unreal.Key()):
        try:
            key = factory()
            if str(key.get_editor_property("key_name")) != name:
                key.set_editor_property("key_name", name)
            return key
        except Exception:
            pass
    raise RuntimeError("Could not create input key: " + name)


def mapping_pairs(context):
    result = set()
    mapping_data = context.get_editor_property("default_key_mappings")
    for entry in mapping_data.get_editor_property("mappings"):
        action = entry.get_editor_property("action")
        key = entry.get_editor_property("key")
        if action:
            result.add((action.get_name(), str(key.get_editor_property("key_name"))))
    return result


context = unreal.EditorAssetLibrary.load_asset(IMC_DEFAULT)
if not context:
    raise RuntimeError("Could not load " + IMC_DEFAULT)

context.modify(True)
for action_name, key_name in GAMEPAD_BINDINGS:
    action = unreal.EditorAssetLibrary.load_asset(ACTION_ROOT + action_name)
    if not action:
        raise RuntimeError("Could not load input action: " + action_name)
    key = make_key(key_name)
    try:
        context.unmap_key(action, key)
    except Exception:
        pass
    context.map_key(action, key)

# UE 5.8 stores the serialized mappings inside this wrapper. Assign it back so
# commandlet/editor reloads retain every new entry.
mapping_data = context.get_editor_property("default_key_mappings")
context.set_editor_property("default_key_mappings", mapping_data)
unreal.EditorAssetLibrary.save_loaded_asset(context)

actual = mapping_pairs(context)
missing = [pair for pair in GAMEPAD_BINDINGS if pair not in actual]
if missing:
    raise RuntimeError("Bindings missing after save: " + repr(missing))

log("Saved gamepad bindings: " + ", ".join("%s=%s" % pair for pair in GAMEPAD_BINDINGS))
