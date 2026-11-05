"""Convert the two named PCG instance groups into individually editable actors.

Run in the Unreal Editor Python console or with -run=pythonscript. The script
only edits /Game/Level/TestForCharacter and retains every instance's world
transform. It refuses to run if the source counts differ from the audited map.
"""

import json

import unreal


LEVEL = "/Game/Level/TestForCharacter"
REPORT = "/private/tmp/delivery_pcg_split_report.json"
GROUPS = (
    ("ROAD-CREATE", "ISM_roadlight_1", 522, "RoadLights", "RoadLight"),
    ("GRASSBED-CREATE1", "ISM_GRASSBED2_0", 507, "GrassBeds", "GrassBed2"),
    ("GRASSBED-CREATE1", "ISM_GRASSBED_7", 511, "GrassBeds", "GrassBed"),
)


def vector_values(value):
    return [float(value.x), float(value.y), float(value.z)]


def quat_values(value):
    return [float(value.x), float(value.y), float(value.z), float(value.w)]


def transform_values(value):
    return {
        "translation": vector_values(value.translation),
        "rotation": quat_values(value.rotation),
        "scale": vector_values(value.scale3d),
    }


def same_transform(before, after):
    a, b = transform_values(before), transform_values(after)
    if any(abs(x - y) > 0.001 for x, y in zip(a["translation"], b["translation"])):
        return False
    if any(abs(x - y) > 0.00001 for x, y in zip(a["scale"], b["scale"])):
        return False
    # q and -q encode the same orientation.
    qa, qb = a["rotation"], b["rotation"]
    return min(max(abs(x - y) for x, y in zip(qa, qb)),
               max(abs(x + y) for x, y in zip(qa, qb))) < 0.00001


def main():
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors_api = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if not levels.load_level(LEVEL):
        raise RuntimeError("Cannot load " + LEVEL)

    actors = actors_api.get_all_level_actors()
    sources = {}
    for actor in actors:
        label = actor.get_actor_label()
        if label in ("ROAD-CREATE", "GRASSBED-CREATE1"):
            if label in sources:
                raise RuntimeError("Duplicate source actor: " + label)
            sources[label] = actor
    if len(sources) != 2:
        raise RuntimeError("Expected both original PCG actors, found " + repr(list(sources)))

    # Snapshot all source data before changing the level. Never derive a later
    # placement from a newly spawned actor or the PCG volume's own transform.
    plan = []
    for source_label, component_name, expected, folder, prefix in GROUPS:
        matching = [c for c in sources[source_label].get_components_by_class(
            unreal.InstancedStaticMeshComponent) if c.get_name() == component_name]
        if len(matching) != 1:
            raise RuntimeError("Expected one component " + component_name)
        component = matching[0]
        count = component.get_instance_count()
        if count != expected:
            raise RuntimeError("Unexpected %s count: %d (expected %d)" %
                               (component_name, count, expected))
        mesh = component.get_editor_property("static_mesh")
        if mesh is None:
            raise RuntimeError("No mesh on " + component_name)
        materials = [component.get_material(i) for i in range(component.get_num_materials())]
        for index in range(count):
            plan.append({
                "source": source_label,
                "component": component_name,
                "folder": folder,
                "label": "%s_%04d" % (prefix, index + 1),
                "mesh": mesh,
                "materials": materials,
                "profile": component.get_collision_profile_name(),
                "collision": component.get_collision_enabled(),
                "shadow": component.get_editor_property("cast_shadow"),
                "transform": component.get_instance_transform(index, world_space=True),
            })

    made = []
    report = {"level": LEVEL, "status": "started", "actors": []}
    try:
        for index, row in enumerate(plan):
            world = row["transform"]
            actor = actors_api.spawn_actor_from_class(unreal.StaticMeshActor,
                                                       world.translation)
            if actor is None:
                raise RuntimeError("Could not spawn " + row["label"])
            made.append(actor)
            component = actor.get_editor_property("static_mesh_component")
            component.set_static_mesh(row["mesh"])
            for slot, material in enumerate(row["materials"]):
                if material is not None:
                    component.set_material(slot, material)
            component.set_collision_profile_name(row["profile"])
            component.set_collision_enabled(row["collision"])
            component.set_editor_property("cast_shadow", row["shadow"])
            if not actor.set_actor_transform(world, False, True):
                raise RuntimeError("Could not place " + row["label"])
            actor.set_actor_label(row["label"])
            actor.set_folder_path(row["folder"])
            if not same_transform(world, actor.get_actor_transform()):
                raise RuntimeError("Transform changed for " + row["label"])
            report["actors"].append({
                "label": row["label"],
                "folder": row["folder"],
                "mesh": row["mesh"].get_path_name(),
                "transform": transform_values(world),
            })
            if (index + 1) % 200 == 0:
                unreal.log("PCG split: %d/%d actors placed" % (index + 1, len(plan)))

        for source in sources.values():
            if not actors_api.destroy_actor(source):
                raise RuntimeError("Could not remove source " + source.get_actor_label())
        if not levels.save_current_level():
            raise RuntimeError("Level save failed")
        report["status"] = "saved"
        report["counts"] = {"RoadLights": 522, "GrassBeds": 1018}
    finally:
        # A failed run is intentionally not saved. The original map on disk
        # remains available, including changes that predated this script.
        with open(REPORT, "w", encoding="utf-8") as handle:
            json.dump(report, handle, ensure_ascii=False, indent=2)
    unreal.log("PCG split completed: 522 road lights, 1018 grass beds")


if __name__ == "__main__":
    main()
