"""Import buildings.json as colored, landscape-snapped whiteboxes."""

from __future__ import annotations

import json

import unreal

from ps2dem_common import TerrainHeightSampler, buildings_json_path, show_message


REPLACE_PREVIOUS = True
CONTENT_ROOT = "/Game/PS2DEM"

CATEGORY_CONFIG = {
    "A": {"folder": "PS2DEM/Residential", "color": unreal.LinearColor(1.0, 0.75, 0.05, 1.0)},
    "B": {"folder": "PS2DEM/Commercial", "color": unreal.LinearColor(1.0, 0.25, 0.02, 1.0)},
    "C": {"folder": "PS2DEM/Public", "color": unreal.LinearColor(0.45, 0.08, 0.8, 1.0)},
}

GENERATED_TAG = "PS2DEM_BUILDING"
CUBE_PATH = "/Engine/BasicShapes/Cube.Cube"
BASIC_MATERIAL_PATH = "/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"


def load_buildings() -> tuple[dict, object]:
    path = buildings_json_path()
    if not path.is_file():
        raise RuntimeError(f"buildings.json not found: {path}")
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    if data.get("schema_version") != 1 or not isinstance(data.get("buildings"), list):
        raise RuntimeError("Unsupported buildings.json format")
    return data, path


def ensure_color_material(type_code: str) -> unreal.MaterialInterface:
    config = CATEGORY_CONFIG[type_code]
    package_path = f"{CONTENT_ROOT}/Materials"
    asset_name = f"MI_PS2DEM_{type_code}"
    asset_path = f"{package_path}/{asset_name}"
    existing = unreal.EditorAssetLibrary.load_asset(asset_path)
    if existing:
        return existing
    parent = unreal.EditorAssetLibrary.load_asset(BASIC_MATERIAL_PATH)
    if not parent:
        raise RuntimeError(f"Could not load {BASIC_MATERIAL_PATH}")
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        asset_name,
        package_path,
        unreal.MaterialInstanceConstant,
        unreal.MaterialInstanceConstantFactoryNew(),
    )
    if not material:
        raise RuntimeError(f"Could not create material {asset_path}")
    unreal.MaterialEditingLibrary.set_material_instance_parent(material, parent)
    unreal.MaterialEditingLibrary.set_material_instance_vector_parameter_value(
        material, "Color", config["color"]
    )
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    return material


def delete_previous(subsystem: unreal.EditorActorSubsystem) -> int:
    removed = 0
    for actor in subsystem.get_all_level_actors():
        if GENERATED_TAG in {str(tag) for tag in actor.tags}:
            subsystem.destroy_actor(actor)
            removed += 1
    return removed


def import_whiteboxes() -> None:
    try:
        data, json_path = load_buildings()
        cube = unreal.EditorAssetLibrary.load_asset(CUBE_PATH)
        if not cube:
            raise RuntimeError(f"Could not load {CUBE_PATH}")
        subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        sampler = TerrainHeightSampler()
        materials = {code: ensure_color_material(code) for code in CATEGORY_CONFIG}
        removed = delete_previous(subsystem) if REPLACE_PREVIOUS else 0
        created = 0
        with unreal.ScopedEditorTransaction("Import PS2DEM building whiteboxes"):
            for building in data["buildings"]:
                type_code = str(building["type"]).upper()
                if type_code not in CATEGORY_CONFIG:
                    unreal.log_warning(f"Skipped {building.get('name')}: unknown type")
                    continue
                center_x_m, center_y_m = building["center_m"]
                height_m = float(building["height_m"])
                x_cm, y_cm = float(center_x_m) * 100.0, float(center_y_m) * 100.0
                ground_z = sampler.location(
                    float(center_x_m), float(center_y_m)
                ).z
                actor = subsystem.spawn_actor_from_object(
                    cube,
                    unreal.Vector(x_cm, y_cm, ground_z + height_m * 50.0),
                    unreal.Rotator(pitch=0.0, yaw=float(building["yaw_deg"]), roll=0.0),
                )
                if not actor:
                    continue
                actor.set_actor_scale3d(
                    unreal.Vector(float(building["length_m"]), float(building["width_m"]), height_m)
                )
                actor.set_actor_label(str(building["name"]))
                actor.set_folder_path(CATEGORY_CONFIG[type_code]["folder"])
                actor.set_editor_property(
                    "tags", [unreal.Name(GENERATED_TAG), unreal.Name(f"TYPE_{type_code}")]
                )
                actor.static_mesh_component.set_material(0, materials[type_code])
                created += 1
        unreal.EditorLevelLibrary.save_current_level()
        terrain_summary = "All buildings were inside the terrain extent."
        if sampler.outside_points:
            terrain_summary = (
                f"Buildings clamped to the nearest terrain edge: "
                f"{len(sampler.outside_points)}"
            )
        message = (
            f"Created {created} buildings; replaced {removed}.\n"
            f"{terrain_summary}\n\nSource:\n{json_path}"
        )
        unreal.log(message)
        show_message("PS2DEM Building Import", message)
    except Exception as exc:
        unreal.log_error(str(exc))
        show_message("PS2DEM Building Import Failed", str(exc))
        raise
