"""Import Photoshop route paths as ordinary editable Spline Actors."""

from __future__ import annotations

import json

import unreal

from ps2dem_common import TerrainHeightSampler, show_message, splines_json_path


GENERATED_TAG = "PS2DEM_SPLINE"
BLUEPRINT_PATH = "/Game/PS2DEM/BP_PS2DEMSplineActor_v3"
GROUND_OFFSET_CM = 10.0

TYPE_CONFIG = {
    "MainRoad": {"folder": "PS2DEM/Splines/MainRoad", "color": unreal.LinearColor(1.0, 0.32, 0.02, 1.0)},
    "BranchRoad": {"folder": "PS2DEM/Splines/BranchRoad", "color": unreal.LinearColor(1.0, 0.9, 0.25, 1.0)},
    "River": {"folder": "PS2DEM/Splines/River", "color": unreal.LinearColor(0.0, 0.4, 1.0, 1.0)},
}


def ensure_spline_blueprint() -> unreal.Blueprint:
    """Create a reusable Actor Blueprint whose component survives compilation."""

    blueprint = unreal.EditorAssetLibrary.load_asset(BLUEPRINT_PATH)
    if blueprint:
        return blueprint
    else:
        package_path, asset_name = BLUEPRINT_PATH.rsplit("/", 1)
        factory = unreal.BlueprintFactory()
        factory.set_editor_property("parent_class", unreal.Actor)
        blueprint = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            asset_name, package_path, unreal.Blueprint, factory
        )
        if not blueprint:
            raise RuntimeError(f"Could not create {BLUEPRINT_PATH}")

    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    handles = subsystem.k2_gather_subobject_data_for_blueprint(blueprint)
    if not handles:
        raise RuntimeError("Could not gather Blueprint component data")
    root_handle = handles[0]
    result = subsystem.add_new_subobject(
        unreal.AddNewSubobjectParams(
            parent_handle=root_handle,
            new_class=unreal.SplineComponent,
            blueprint_context=blueprint,
        )
    )
    component_handle, fail_reason = (
        result if isinstance(result, tuple) else (result, "")
    )
    if str(fail_reason):
        raise RuntimeError(f"Could not add SplineComponent: {fail_reason}")

    # add_new_subobject alone only creates a loose template. It must be
    # attached to the Blueprint component tree or compilation discards it.
    subsystem.rename_subobject(component_handle, unreal.Text("Spline"))
    if not subsystem.attach_subobject(root_handle, component_handle):
        raise RuntimeError("Could not attach SplineComponent to Blueprint tree")

    unreal.BlueprintEditorLibrary.compile_blueprint(blueprint)
    unreal.EditorAssetLibrary.save_loaded_asset(blueprint)
    # Do not query the Blueprint class default object here. Components from
    # the Blueprint Components panel are SCS templates and are instantiated
    # when an Actor is spawned; they are not reliably present in the CDO's
    # owned-components array. The spawned Actor is verified in import_splines.
    return blueprint


def delete_previous(subsystem: unreal.EditorActorSubsystem) -> int:
    removed = 0
    for actor in subsystem.get_all_level_actors():
        if GENERATED_TAG in {str(tag) for tag in actor.tags}:
            subsystem.destroy_actor(actor)
            removed += 1
    return removed


def tangent_vector(
    sampler: TerrainHeightSampler,
    anchor: list[float],
    handle: list[float],
    leaving: bool,
) -> unreal.Vector:
    anchor_world = sampler.location(
        float(anchor[0]), float(anchor[1]), GROUND_OFFSET_CM
    )
    handle_world = sampler.location(
        float(handle[0]), float(handle[1]), GROUND_OFFSET_CM
    )
    delta = handle_world - anchor_world if leaving else anchor_world - handle_world
    return delta * 3.0


def import_splines() -> None:
    try:
        json_path = splines_json_path()
        if not json_path.is_file():
            raise RuntimeError(f"splines.json not found: {json_path}")
        data = json.loads(json_path.read_text(encoding="utf-8-sig"))
        if data.get("schema_version") != 1 or not isinstance(data.get("routes"), list):
            raise RuntimeError("Unsupported splines.json format")
        blueprint = ensure_spline_blueprint()
        actor_class = blueprint.generated_class()
        sampler = TerrainHeightSampler()
        subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        removed = delete_previous(subsystem)
        created = 0

        with unreal.ScopedEditorTransaction("Import PS2DEM route splines"):
            for route in data["routes"]:
                route_type = str(route["type"])
                if route_type not in TYPE_CONFIG:
                    unreal.log_warning(f"Skipped {route.get('name')}: unknown type {route_type}")
                    continue
                points = route.get("points", [])
                if len(points) < 2:
                    continue
                world_locations = []
                for point in points:
                    anchor = point["anchor"]
                    world_locations.append(
                        sampler.location(
                            float(anchor[0]), float(anchor[1]), GROUND_OFFSET_CM
                        )
                    )
                actor_origin = world_locations[0]
                actor = subsystem.spawn_actor_from_class(
                    actor_class, actor_origin, unreal.Rotator()
                )
                if not actor:
                    raise RuntimeError("UE could not spawn the route Blueprint Actor")
                spline = actor.get_component_by_class(unreal.SplineComponent)
                if not spline:
                    subsystem.destroy_actor(actor)
                    raise RuntimeError(
                        "Spawned route Actor has no SplineComponent; "
                        "the Blueprint component tree was not instantiated"
                    )
                width_m = float(route["width_m"])

                # Actor property edits can rerun a Blueprint construction
                # script, so finish them before writing per-instance points.
                actor.set_actor_label(str(route["name"]))
                actor.set_folder_path(TYPE_CONFIG[route_type]["folder"])
                actor.set_editor_property(
                    "tags",
                    [unreal.Name(GENERATED_TAG), unreal.Name(route_type), unreal.Name(f"WIDTH_M_{width_m:g}")],
                )

                # Setting Actor tags triggers RerunConstructionScripts in UE
                # 5.8. That destroys the SCS-created component and replaces
                # it with a new instance, so the previous Python reference is
                # stale and silently ignores all spline point mutations.
                spline = actor.get_component_by_class(unreal.SplineComponent)
                if not spline:
                    subsystem.destroy_actor(actor)
                    raise RuntimeError(
                        f"{route['name']}: SplineComponent was lost after "
                        "setting Actor tags"
                    )

                # Every editor-property change must happen before point data
                # is written. These setters can trigger PostEditChange and a
                # Blueprint construction rerun, which restores the SCS
                # template and clears per-instance spline curves.
                spline.set_editor_property(
                    "editor_unselected_spline_segment_color",
                    TYPE_CONFIG[route_type]["color"],
                )
                spline = actor.get_component_by_class(unreal.SplineComponent)
                spline.set_editor_property(
                    "editor_selected_spline_segment_color",
                    TYPE_CONFIG[route_type]["color"],
                )
                spline = actor.get_component_by_class(unreal.SplineComponent)
                spline.set_editor_property(
                    "input_spline_points_to_construction_script", False
                )
                spline = actor.get_component_by_class(unreal.SplineComponent)
                try:
                    spline.set_override_construction_script(True)
                except AttributeError:
                    spline.set_editor_property("spline_has_been_edited", True)
                spline = actor.get_component_by_class(unreal.SplineComponent)
                if not spline:
                    subsystem.destroy_actor(actor)
                    raise RuntimeError(
                        f"{route['name']}: SplineComponent was lost while "
                        "configuring editor properties"
                    )

                # Keep each Actor's pivot at its own first control point. This
                # makes the Actor transform meaningful and avoids every route
                # sharing the world origin.
                locations = [location - actor_origin for location in world_locations]
                spline.set_spline_points(
                    locations, unreal.SplineCoordinateSpace.LOCAL, False
                )
                for index, point in enumerate(points):
                    anchor = point["anchor"]
                    spline.set_spline_point_type(index, unreal.SplinePointType.CURVE_CUSTOM_TANGENT, False)
                    spline.set_tangents_at_spline_point(
                        index,
                        tangent_vector(sampler, anchor, point["backward"], False),
                        tangent_vector(sampler, anchor, point["forward"], True),
                        unreal.SplineCoordinateSpace.LOCAL,
                        False,
                    )
                    spline.set_scale_at_spline_point(index, unreal.Vector(1.0, width_m, 1.0), False)
                spline.set_closed_loop(bool(route.get("closed", False)), False)
                spline.update_spline()
                actual_points = spline.get_number_of_spline_points()
                if actual_points != len(points):
                    subsystem.destroy_actor(actor)
                    raise RuntimeError(
                        f"{route['name']}: expected {len(points)} spline points, "
                        f"but UE retained {actual_points}"
                    )
                unreal.log(
                    f"PS2DEM spline {route['name']}: JSON={len(points)}, "
                    f"UE={actual_points}, first={points[0]['anchor']}, "
                    f"last={points[-1]['anchor']}"
                )
                created += 1

        unreal.EditorLevelLibrary.save_current_level()
        point_counts = [
            len(route.get("points", []))
            for route in data["routes"]
            if isinstance(route.get("points", []), list)
        ]
        count_summary = "No route point data"
        if point_counts:
            two_point_count = sum(count == 2 for count in point_counts)
            count_summary = (
                f"JSON points per route: {min(point_counts)}-{max(point_counts)}\n"
                f"Routes containing exactly 2 points: "
                f"{two_point_count}/{len(point_counts)}"
            )
        terrain_summary = "All sampled points were inside the terrain extent."
        if sampler.outside_points:
            terrain_summary = (
                f"Terrain samples clamped to the nearest edge: "
                f"{len(sampler.outside_points)}"
            )
            unreal.log_warning(
                "PS2DEM spline points outside terrain extent: "
                + repr(sorted(sampler.outside_points))
            )
        message = (
            f"Created {created} route splines; replaced {removed}.\n\n"
            f"{count_summary}\n{terrain_summary}\n\nSource:\n{json_path}"
        )
        unreal.log(message)
        show_message("PS2DEM Spline Import", message)
    except Exception as exc:
        unreal.log_error(str(exc))
        show_message("PS2DEM Spline Import Failed", str(exc))
        raise
