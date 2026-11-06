"""Split POLE-CREATE's 511 instances into independent StaticMeshActors.

Run in Unreal Editor Python or with -run=pythonscript. The source volume is
removed only after all replacement actors pass transform validation.
"""

import json
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(__file__))
from split_pcg_lamps_grassbeds import same_transform, transform_values


LEVEL = "/Game/Level/TestForCharacter"
SOURCE = "POLE-CREATE"
COMPONENT = "ISM_pole2_1"
COUNT = 511
FOLDER = "Poles"
REPORT = "/private/tmp/delivery_pole_split_report.json"


def main():
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if not levels.load_level(LEVEL):
        raise RuntimeError("Cannot load " + LEVEL)
    sources = [a for a in eas.get_all_level_actors() if a.get_actor_label() == SOURCE]
    if len(sources) != 1:
        raise RuntimeError("Expected one POLE-CREATE, found %d" % len(sources))
    source = sources[0]
    components = [c for c in source.get_components_by_class(unreal.InstancedStaticMeshComponent)
                  if c.get_name() == COMPONENT]
    if len(components) != 1 or components[0].get_instance_count() != COUNT:
        raise RuntimeError("Unexpected pole instance component or count")
    component = components[0]
    mesh = component.get_editor_property("static_mesh")
    if mesh is None or mesh.get_path_name() != "/Game/model/pole.pole":
        raise RuntimeError("Unexpected pole mesh")
    materials = [component.get_material(i) for i in range(component.get_num_materials())]
    profile = component.get_collision_profile_name()
    collision = component.get_collision_enabled()
    shadow = component.get_editor_property("cast_shadow")
    transforms = [component.get_instance_transform(i, world_space=True) for i in range(COUNT)]
    report = {"level": LEVEL, "status": "started", "actors": []}
    try:
        for index, world in enumerate(transforms):
            actor = eas.spawn_actor_from_class(unreal.StaticMeshActor, world.translation)
            if actor is None:
                raise RuntimeError("Could not spawn pole %d" % index)
            body = actor.get_editor_property("static_mesh_component")
            body.set_static_mesh(mesh)
            for slot, material in enumerate(materials):
                if material is not None:
                    body.set_material(slot, material)
            body.set_collision_profile_name(profile)
            body.set_collision_enabled(collision)
            body.set_editor_property("cast_shadow", shadow)
            if not actor.set_actor_transform(world, False, True):
                raise RuntimeError("Could not place pole %d" % index)
            label = "Pole_%04d" % (index + 1)
            actor.set_actor_label(label)
            actor.set_folder_path(FOLDER)
            if not same_transform(world, actor.get_actor_transform()):
                raise RuntimeError("Pole moved during conversion: " + label)
            report["actors"].append({"label": label, "folder": FOLDER,
                                     "mesh": mesh.get_path_name(),
                                     "transform": transform_values(world)})
        if not eas.destroy_actor(source):
            raise RuntimeError("Could not remove POLE-CREATE")
        if not levels.save_current_level():
            raise RuntimeError("Could not save level")
        report["status"] = "saved"
    finally:
        with open(REPORT, "w", encoding="utf-8") as handle:
            json.dump(report, handle, ensure_ascii=False, indent=2)
    unreal.log("[PoleSplit] saved 511 independent poles")


if __name__ == "__main__":
    main()
