"""Read-only package/input defaults audit; never saves a map or asset."""
import unreal

for path in ("/Game/Blueprint/Item/Test/BP_TestDeliveryPackage", "/Game/Blueprint/Item/Test/BP_TestAxe"):
    cls = unreal.EditorAssetLibrary.load_blueprint_class(path)
    obj = unreal.get_default_object(cls)
    for comp in obj.get_components_by_class(unreal.ActorComponent):
        if comp.get_name() == "Interactable":
            unreal.log("PACKAGE_AUDIT %s key=%s hold=%s radius=%s" % (path,
                comp.get_editor_property("interaction_key"), comp.get_editor_property("hold_duration"),
                comp.get_editor_property("interact_radius")))
        if comp.get_name() == "DeliveryTaskItem":
            unreal.log("PACKAGE_AUDIT task=%s" % comp.get_editor_property("owning_task"))
action = unreal.EditorAssetLibrary.load_asset("/Game/Input/Actions/IA_Pickup")
unreal.log("PACKAGE_AUDIT triggers=%s" % action.get_editor_property("triggers"))
