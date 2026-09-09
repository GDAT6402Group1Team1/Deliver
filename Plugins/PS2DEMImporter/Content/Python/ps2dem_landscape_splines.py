"""Apply selected PS2DEM ordinary splines to a Landscape edit layer."""

from __future__ import annotations

import math

import unreal

from ps2dem_common import TerrainHeightSampler, show_message


EDIT_LAYER_NAME = "PS2DEM_Splines"
WIDTH_TAG_PREFIX = "WIDTH_M_"
GROUND_SAMPLE_SPACING_CM = 100.0
GROUND_OFFSET_CM = 10.0
MAX_SAFE_HEIGHT_CORRECTION_CM = 200.0
TRACE_TOP_CM = 1_000_000.0
TRACE_BOTTOM_CM = -1_000_000.0

TYPE_CONFIG = {
    "MainRoad": {
        "default_width_m": 6.0,
        "falloff_factor": 1.0,
        "raise_heights": True,
        "lower_heights": True,
    },
    "BranchRoad": {
        "default_width_m": 3.5,
        "falloff_factor": 1.0,
        "raise_heights": True,
        "lower_heights": True,
    },
    "River": {
        "default_width_m": 12.0,
        "falloff_factor": 1.0,
        "raise_heights": False,
        "lower_heights": True,
    },
}


def _tags(actor: unreal.Actor) -> set[str]:
    return {str(tag) for tag in actor.tags}


def _route_type(actor: unreal.Actor) -> str | None:
    tags = _tags(actor)
    for route_type in TYPE_CONFIG:
        if route_type in tags:
            return route_type
    return None


def _route_width_m(actor: unreal.Actor, route_type: str) -> float:
    for tag in _tags(actor):
        if tag.startswith(WIDTH_TAG_PREFIX):
            try:
                width_m = float(tag[len(WIDTH_TAG_PREFIX) :])
            except ValueError:
                break
            if width_m > 0.0:
                return width_m
            break
    return float(TYPE_CONFIG[route_type]["default_width_m"])


def _selected_routes(
    subsystem: unreal.EditorActorSubsystem,
) -> list[tuple[unreal.Actor, unreal.SplineComponent, str, float]]:
    routes = []
    for actor in subsystem.get_selected_level_actors():
        route_type = _route_type(actor)
        if not route_type:
            continue
        spline = actor.get_component_by_class(unreal.SplineComponent)
        if not spline or spline.get_number_of_spline_points() < 2:
            continue
        routes.append((actor, spline, route_type, _route_width_m(actor, route_type)))
    return routes


def _root_landscape(actor: unreal.LandscapeProxy) -> unreal.Landscape | None:
    if isinstance(actor, unreal.Landscape):
        return actor
    root = actor.get_landscape_actor()
    return root if isinstance(root, unreal.Landscape) else None


def _unique_landscapes(actors) -> list[unreal.Landscape]:
    result = {}
    for actor in actors:
        if not isinstance(actor, unreal.LandscapeProxy):
            continue
        root = _root_landscape(actor)
        if root:
            result[root.get_path_name()] = root
    return list(result.values())


def _target_landscape(
    subsystem: unreal.EditorActorSubsystem,
) -> unreal.Landscape:
    selected = _unique_landscapes(subsystem.get_selected_level_actors())
    if len(selected) == 1:
        return selected[0]
    if len(selected) > 1:
        raise RuntimeError("More than one Landscape is selected. Select only one target Landscape.")

    available = _unique_landscapes(subsystem.get_all_level_actors())
    if len(available) == 1:
        return available[0]
    if not available:
        raise RuntimeError("No Landscape was found in the current level.")
    raise RuntimeError(
        "This level contains more than one Landscape. Select the target Landscape "
        "together with the route splines, then run the command again."
    )


def _layer_names(landscape: unreal.Landscape) -> list[str]:
    names = []
    for layer in landscape.get_edit_layers_bp():
        try:
            names.append(str(layer.get_name_bp()))
        except Exception:
            names.append(layer.get_name())
    return names


def _require_edit_layer(landscape: unreal.Landscape) -> None:
    layer = landscape.get_edit_layer_by_name_bp(unreal.Name(EDIT_LAYER_NAME))
    if layer:
        return

    world = unreal.EditorLevelLibrary.get_editor_world()
    unreal.SystemLibrary.execute_console_command(world, "MODE LANDSCAPE")
    existing = _layer_names(landscape)
    existing_text = ", ".join(existing) if existing else "none"
    raise RuntimeError(
        f"Landscape edit layer '{EDIT_LAYER_NAME}' was not found.\n\n"
        "In Landscape Mode, open Sculpt > Edit Layers, create a regular Edit "
        f"Layer named exactly '{EDIT_LAYER_NAME}', then run this command again.\n"
        "Do not create a reserved Spline Edit Layer for this command.\n\n"
        f"Existing edit layers: {existing_text}"
    )


def _grounded_world_points(
    spline: unreal.SplineComponent,
    sampler: TerrainHeightSampler,
    landscape: unreal.Landscape,
    actors_to_ignore: list[unreal.Actor],
) -> tuple[list[unreal.Vector], float, int]:
    """Encode the desired final DEM height as a regular Edit Layer delta."""

    length_cm = float(spline.get_spline_length())
    segment_count = max(1, int(math.ceil(length_cm / GROUND_SAMPLE_SPACING_CM)))
    closed = bool(spline.is_closed_loop())
    point_count = max(3, segment_count) if closed else segment_count + 1
    denominator = point_count if closed else point_count - 1
    points = []
    max_correction_cm = 0.0
    missed_surface_samples = 0
    landscape_origin_z = float(landscape.get_actor_location().z)
    world = unreal.EditorLevelLibrary.get_editor_world()
    for index in range(point_count):
        distance = length_cm * index / denominator
        route_location = spline.get_location_at_distance_along_spline(
            distance, unreal.SplineCoordinateSpace.WORLD
        )
        desired = sampler.location(
            route_location.x / 100.0,
            route_location.y / 100.0,
            GROUND_OFFSET_CM,
        )
        hit = unreal.SystemLibrary.line_trace_single(
            world,
            unreal.Vector(route_location.x, route_location.y, TRACE_TOP_CM),
            unreal.Vector(route_location.x, route_location.y, TRACE_BOTTOM_CM),
            unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
            True,
            actors_to_ignore,
            unreal.DrawDebugTrace.NONE,
            True,
        )
        if hit is None:
            # Outside the Landscape there is nothing to deform. A zero-delta
            # proxy height keeps the boundary interpolation safe.
            encoded_z = landscape_origin_z
            missed_surface_samples += 1
        else:
            hit_data = hit.to_dict()
            impact = hit_data.get("impact_point") or hit_data.get("location")
            if impact is None:
                encoded_z = landscape_origin_z
                missed_surface_samples += 1
            else:
                correction_cm = float(desired.z) - float(impact.z)
                max_correction_cm = max(max_correction_cm, abs(correction_cm))
                if abs(correction_cm) > MAX_SAFE_HEIGHT_CORRECTION_CM:
                    raise RuntimeError(
                        "The current Landscape differs from the R16 target by "
                        f"{correction_cm:.1f} cm near "
                        f"({route_location.x / 100.0:.1f} m, "
                        f"{route_location.y / 100.0:.1f} m).\n\n"
                        "The PS2DEM_Splines layer may already contain an older "
                        "application, or the Landscape and R16 may be from "
                        "different terrain exports. Reset the deformation layer "
                        "or reimport the matching terrain before applying again."
                    )
                # On a normal Landscape Edit Layer, Editor Apply Spline writes
                # (spline Z - Landscape actor Z) as an additive height. Passing
                # the desired absolute world Z would therefore add the terrain
                # elevation a second time. Encode only the required correction.
                encoded_z = landscape_origin_z + correction_cm
        points.append(unreal.Vector(route_location.x, route_location.y, encoded_z))
    return points, max_correction_cm, missed_surface_samples


def _target_landscape_ignore_list(
    subsystem: unreal.EditorActorSubsystem,
    target: unreal.Landscape,
) -> list[unreal.Actor]:
    """Ignore all Actors except the target Landscape and its streaming proxies."""

    target_path = target.get_path_name()
    ignored = []
    for actor in subsystem.get_all_level_actors():
        if isinstance(actor, unreal.LandscapeProxy):
            root = _root_landscape(actor)
            if root and root.get_path_name() == target_path:
                continue
        ignored.append(actor)
    return ignored


def _spawn_grounded_proxy(
    subsystem: unreal.EditorActorSubsystem,
    source_actor: unreal.Actor,
    source_spline: unreal.SplineComponent,
    sampler: TerrainHeightSampler,
    landscape: unreal.Landscape,
    actors_to_ignore: list[unreal.Actor],
) -> tuple[unreal.Actor, unreal.SplineComponent, int, float, int]:
    """Create a short-lived, densely sampled spline that follows the source DEM."""

    world_points, max_correction_cm, missed_surface_samples = _grounded_world_points(
        source_spline, sampler, landscape, actors_to_ignore
    )
    origin = world_points[0]
    proxy_actor = subsystem.spawn_actor_from_class(
        source_actor.get_class(), origin, unreal.Rotator()
    )
    if not proxy_actor:
        raise RuntimeError(
            f"Could not create grounded proxy for {source_actor.get_actor_label()}"
        )
    proxy_spline = proxy_actor.get_component_by_class(unreal.SplineComponent)
    if not proxy_spline:
        subsystem.destroy_actor(proxy_actor)
        raise RuntimeError(
            f"Grounded proxy for {source_actor.get_actor_label()} has no SplineComponent"
        )

    local_points = [point - origin for point in world_points]
    proxy_spline.set_spline_points(
        local_points, unreal.SplineCoordinateSpace.LOCAL, False
    )
    for index in range(len(local_points)):
        # Linear segments cannot overshoot vertically between grounded samples.
        proxy_spline.set_spline_point_type(
            index, unreal.SplinePointType.LINEAR, False
        )
    proxy_spline.set_closed_loop(bool(source_spline.is_closed_loop()), False)
    proxy_spline.update_spline()
    actual_origin = proxy_spline.get_location_at_spline_point(
        0, unreal.SplineCoordinateSpace.WORLD
    )
    origin_delta = actual_origin - world_points[0]
    origin_error_cm = math.sqrt(
        origin_delta.x * origin_delta.x
        + origin_delta.y * origin_delta.y
        + origin_delta.z * origin_delta.z
    )
    if origin_error_cm > 0.1:
        subsystem.destroy_actor(proxy_actor)
        raise RuntimeError(
            f"Grounded proxy transform mismatch for {source_actor.get_actor_label()}: "
            f"{origin_error_cm:.3f} cm"
        )
    return (
        proxy_actor,
        proxy_spline,
        len(world_points),
        max_correction_cm,
        missed_surface_samples,
    )


def apply_selected_splines_to_landscape() -> None:
    """Deform the target Landscape using selected tagged route splines."""

    try:
        subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        routes = _selected_routes(subsystem)
        if not routes:
            raise RuntimeError(
                "Select one or more imported MainRoad, BranchRoad or River "
                "Spline Actors, then run this command again."
            )

        landscape = _target_landscape(subsystem)
        _require_edit_layer(landscape)
        sampler = TerrainHeightSampler()
        actors_to_ignore = _target_landscape_ignore_list(subsystem, landscape)

        route_lines = "\n".join(
            f"- {actor.get_actor_label()} ({route_type}, {width_m:g} m)"
            for actor, _spline, route_type, width_m in routes
        )
        confirmation = unreal.EditorDialog.show_message(
            "Apply PS2DEM Splines to Landscape",
            f"Apply {len(routes)} selected route(s) to Landscape "
            f"'{landscape.get_actor_label()}'?\n\n"
            f"Target edit layer: {EDIT_LAYER_NAME}\n\n"
            f"{route_lines}\n\n"
            "Roads can raise and lower terrain. Rivers only lower terrain.\n"
            "A temporary ground-following spline will be sampled every 1 m; "
            "the editable source routes will not gain extra points.\n"
            "Applying again accumulates changes; delete and recreate the edit "
            "layer first when you want a clean rebuild.",
            unreal.AppMsgType.YES_NO,
        )
        if confirmation != unreal.AppReturnType.YES:
            unreal.log("PS2DEM Landscape spline application cancelled")
            return

        applied = []
        proxy_actors = []
        try:
            with unreal.ScopedEditorTransaction("Apply PS2DEM splines to Landscape"):
                landscape.modify()
                for actor, spline, route_type, width_m in routes:
                    config = TYPE_CONFIG[route_type]
                    half_width_cm = width_m * 50.0
                    falloff_cm = max(
                        200.0,
                        half_width_cm * float(config["falloff_factor"]),
                    )
                    (
                        proxy_actor,
                        proxy_spline,
                        ground_point_count,
                        max_correction_cm,
                        missed_surface_samples,
                    ) = _spawn_grounded_proxy(
                        subsystem,
                        actor,
                        spline,
                        sampler,
                        landscape,
                        actors_to_ignore,
                    )
                    proxy_actors.append(proxy_actor)
                    landscape.editor_apply_spline(
                        proxy_spline,
                        start_width=half_width_cm,
                        end_width=half_width_cm,
                        start_side_falloff=falloff_cm,
                        end_side_falloff=falloff_cm,
                        start_roll=0.0,
                        end_roll=0.0,
                        # The proxy already has one linear point every metre.
                        num_subdivisions=1,
                        raise_heights=bool(config["raise_heights"]),
                        lower_heights=bool(config["lower_heights"]),
                        paint_layer=None,
                        edit_layer_name=unreal.Name(EDIT_LAYER_NAME),
                    )
                    applied.append(actor.get_actor_label())
                    unreal.log(
                        f"PS2DEM applied {actor.get_actor_label()} to Landscape: "
                        f"type={route_type}, total_width={width_m:g}m, "
                        f"half_width={half_width_cm:g}cm, falloff={falloff_cm:g}cm, "
                        f"ground_samples={ground_point_count}, "
                        f"max_height_correction={max_correction_cm:.2f}cm, "
                        f"surface_misses={missed_surface_samples}"
                    )
                landscape.force_layers_full_update()
        finally:
            for proxy_actor in proxy_actors:
                subsystem.destroy_actor(proxy_actor)

        message = (
            f"Applied {len(applied)} route(s) to Landscape "
            f"'{landscape.get_actor_label()}'.\n\n"
            f"Edit layer: {EDIT_LAYER_NAME}\n\n"
            "The level was not saved automatically. Inspect the result, then "
            "save the level when satisfied. Use Ctrl+Z immediately, or delete "
            "and recreate the edit layer, to rebuild from a clean state."
        )
        unreal.log(message)
        show_message("PS2DEM Landscape Deformation", message)
    except Exception as exc:
        unreal.log_error(str(exc))
        show_message("PS2DEM Landscape Deformation Failed", str(exc))
        raise


def show_reset_instructions() -> None:
    """Open Landscape mode and explain the safe layer reset workflow."""

    world = unreal.EditorLevelLibrary.get_editor_world()
    unreal.SystemLibrary.execute_console_command(world, "MODE LANDSCAPE")
    show_message(
        "Reset PS2DEM Landscape Deformation",
        "Landscape Mode has been opened.\n\n"
        "To remove all terrain changes made by this tool:\n"
        f"1. Open Sculpt > Edit Layers.\n"
        f"2. Delete the regular edit layer '{EDIT_LAYER_NAME}'.\n"
        f"3. Create a new regular edit layer with the same name before applying "
        "routes again.\n\n"
        "UE 5.8 does not expose stable Python methods for creating or deleting "
        "Landscape Edit Layers, so these two layer operations remain manual.",
    )
