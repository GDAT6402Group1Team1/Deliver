"""Apply selected PS2DEM ordinary splines to a Landscape edit layer."""

from __future__ import annotations

import math

import unreal

from ps2dem_common import show_message


EDIT_LAYER_NAME = "PS2DEM_Splines"
GENERATED_TAG = "PS2DEM_SPLINE"
WIDTH_TAG_PREFIX = "WIDTH_M_"

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


def _subdivision_count(spline: unreal.SplineComponent) -> int:
    # Roughly one subdivision per five metres, bounded to avoid very slow or
    # artifact-prone values on unusually long routes.
    length_cm = float(spline.get_spline_length())
    return max(20, min(256, int(math.ceil(length_cm / 500.0))))


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
            "Applying again accumulates changes; delete and recreate the edit "
            "layer first when you want a clean rebuild.",
            unreal.AppMsgType.YES_NO,
        )
        if confirmation != unreal.AppReturnType.YES:
            unreal.log("PS2DEM Landscape spline application cancelled")
            return

        applied = []
        with unreal.ScopedEditorTransaction("Apply PS2DEM splines to Landscape"):
            landscape.modify()
            for actor, spline, route_type, width_m in routes:
                config = TYPE_CONFIG[route_type]
                half_width_cm = width_m * 50.0
                falloff_cm = max(
                    200.0,
                    half_width_cm * float(config["falloff_factor"]),
                )
                subdivisions = _subdivision_count(spline)
                landscape.editor_apply_spline(
                    spline,
                    start_width=half_width_cm,
                    end_width=half_width_cm,
                    start_side_falloff=falloff_cm,
                    end_side_falloff=falloff_cm,
                    start_roll=0.0,
                    end_roll=0.0,
                    num_subdivisions=subdivisions,
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
                    f"subdivisions={subdivisions}"
                )
            landscape.force_layers_full_update()

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

