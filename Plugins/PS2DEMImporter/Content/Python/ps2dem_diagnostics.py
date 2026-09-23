"""Read-only diagnostics for PS2DEM route and Landscape height mismatches."""

from __future__ import annotations

from datetime import datetime
import json
import math
import os
from pathlib import Path

import unreal

from ps2dem_common import TerrainHeightSampler, exchange_dir, show_message, terrain_dir
import ps2dem_landscape_splines as landscape_tools


SAMPLE_SPACING_CM = 500.0
TRACE_TOP_CM = 1_000_000.0
TRACE_BOTTOM_CM = -1_000_000.0
MEANINGFUL_DELTA_CM = 25.0


def _round(value: float, digits: int = 3) -> float:
    return round(float(value), digits)


def _vector(value: unreal.Vector) -> dict[str, float]:
    return {"x": _round(value.x), "y": _round(value.y), "z": _round(value.z)}


def _file_info(path: Path) -> dict | None:
    if not path.is_file():
        return None
    modified = datetime.fromtimestamp(path.stat().st_mtime).astimezone()
    return {
        "path": str(path),
        "size_bytes": path.stat().st_size,
        "modified_local": modified.isoformat(timespec="seconds"),
        "modified_timestamp": path.stat().st_mtime,
    }


def _stats(values: list[float]) -> dict | None:
    if not values:
        return None
    count = len(values)
    return {
        "count": count,
        "min_cm": _round(min(values)),
        "max_cm": _round(max(values)),
        "mean_cm": _round(sum(values) / count),
        "mean_abs_cm": _round(sum(abs(value) for value in values) / count),
        "rmse_cm": _round(math.sqrt(sum(value * value for value in values) / count)),
    }


def _target_landscape_ignore_list(
    subsystem: unreal.EditorActorSubsystem,
    target: unreal.Landscape,
) -> list[unreal.Actor]:
    """Ignore everything except the selected Landscape and its streaming proxies."""

    target_path = target.get_path_name()
    ignored = []
    for actor in subsystem.get_all_level_actors():
        if isinstance(actor, unreal.LandscapeProxy):
            root = landscape_tools._root_landscape(actor)
            if root and root.get_path_name() == target_path:
                continue
        ignored.append(actor)
    return ignored


def _trace_landscape_z(
    world,
    x_cm: float,
    y_cm: float,
    actors_to_ignore: list[unreal.Actor],
) -> tuple[float | None, str | None]:
    hit = unreal.SystemLibrary.line_trace_single(
        world,
        unreal.Vector(x_cm, y_cm, TRACE_TOP_CM),
        unreal.Vector(x_cm, y_cm, TRACE_BOTTOM_CM),
        unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
        True,
        actors_to_ignore,
        unreal.DrawDebugTrace.NONE,
        True,
    )
    if hit is None:
        return None, None
    data = hit.to_dict()
    impact = data.get("impact_point") or data.get("location")
    actor = data.get("hit_actor")
    if impact is None:
        return None, actor.get_path_name() if actor else None
    return float(impact.z), actor.get_path_name() if actor else None


def _route_sample_distances(spline: unreal.SplineComponent) -> list[float]:
    length_cm = float(spline.get_spline_length())
    segment_count = max(1, int(math.ceil(length_cm / SAMPLE_SPACING_CM)))
    if spline.is_closed_loop():
        return [length_cm * index / segment_count for index in range(segment_count)]
    return [length_cm * index / segment_count for index in range(segment_count + 1)]


def _terrain_file_report() -> tuple[dict, list[str]]:
    folder = terrain_dir()
    files = {
        "metadata": _file_info(folder / "metadata.json"),
        "r16": _file_info(folder / "height_ue.r16"),
        "height_preview": _file_info(folder / "height_preview.png"),
        "manifest": _file_info(folder / "manifest.json"),
    }
    masks = [_file_info(path) for path in sorted(folder.glob("Z_*.png"))]
    files["contour_masks"] = [item for item in masks if item]

    warnings = []
    r16_info = files["r16"]
    if r16_info and files["contour_masks"]:
        newer = [
            item
            for item in files["contour_masks"]
            if item["modified_timestamp"] > r16_info["modified_timestamp"] + 1.0
        ]
        if newer:
            warnings.append(
                f"{len(newer)} contour mask(s) are newer than height_ue.r16. "
                "Regenerate the terrain before importing or applying routes."
            )
    return files, warnings


def _landscape_report(landscape: unreal.Landscape) -> dict:
    return {
        "label": landscape.get_actor_label(),
        "path": landscape.get_path_name(),
        "location_cm": _vector(landscape.get_actor_location()),
        "scale": _vector(landscape.get_actor_scale3d()),
        "rotation": {
            "pitch": _round(landscape.get_actor_rotation().pitch),
            "yaw": _round(landscape.get_actor_rotation().yaw),
            "roll": _round(landscape.get_actor_rotation().roll),
        },
        "edit_layers": landscape_tools._layer_names(landscape),
    }


def _summary_text(report: dict) -> str:
    summary = report["summary"]
    delta = summary.get("r16_target_minus_landscape")
    lines = [
        f"Report: {report['report_path']}",
        f"Routes: {summary['route_count']}",
        f"Samples: {summary['landscape_hits']} hit, {summary['landscape_misses']} missed",
    ]
    if delta:
        lines.extend(
            [
                "R16 target - current Landscape:",
                f"  min {delta['min_cm']:.1f} cm, max {delta['max_cm']:.1f} cm, "
                f"mean abs {delta['mean_abs_cm']:.1f} cm",
                f"  would raise {summary['would_raise_samples']}, "
                f"lower {summary['would_lower_samples']}, "
                f"within +/-{MEANINGFUL_DELTA_CM:g} cm {summary['near_samples']}",
            ]
        )
    cross_section = summary.get("road_cross_section_target_minus_landscape")
    if cross_section:
        lines.extend(
            [
                "Inside road width (centre target - side terrain):",
                f"  min {cross_section['min_cm']:.1f} cm, "
                f"max {cross_section['max_cm']:.1f} cm, "
                f"mean abs {cross_section['mean_abs_cm']:.1f} cm",
                f"  would raise {summary['cross_section_would_raise_samples']}, "
                f"lower {summary['cross_section_would_lower_samples']}",
            ]
        )
    orientation = summary.get("orientation_check")
    if orientation:
        lines.append(
            "Y orientation MAE: "
            f"normal {orientation['normal_y_mean_abs_cm']:.1f} cm, "
            f"flipped {orientation['flipped_y_mean_abs_cm']:.1f} cm"
        )
    if report["warnings"]:
        lines.append("Warnings:")
        lines.extend(f"- {warning}" for warning in report["warnings"])
    return "\n".join(lines)


def build_diagnostic_report(
    routes,
    landscape: unreal.Landscape,
) -> dict:
    """Measure the current UE surface against the R16 without changing the level."""

    sampler = TerrainHeightSampler()
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.EditorLevelLibrary.get_editor_world()
    ignored = _target_landscape_ignore_list(subsystem, landscape)
    terrain_files, warnings = _terrain_file_report()

    normal_deltas = []
    flipped_deltas = []
    source_deltas = []
    cross_section_deltas = []
    route_reports = []
    hit_count = 0
    miss_count = 0
    outside_count = 0

    for actor, spline, route_type, width_m in routes:
        samples = []
        for distance_cm in _route_sample_distances(spline):
            source = spline.get_location_at_distance_along_spline(
                distance_cm, unreal.SplineCoordinateSpace.WORLD
            )
            x_m = source.x / 100.0
            y_m = source.y / 100.0
            inside = (
                abs(x_m) <= sampler.map_width_m * 0.5
                and abs(y_m) <= sampler.map_height_m * 0.5
            )
            if not inside:
                outside_count += 1

            r16 = sampler.location(x_m, y_m)
            r16_flipped = sampler.location(x_m, -y_m)
            landscape_z, hit_actor = _trace_landscape_z(
                world, source.x, source.y, ignored
            )
            direction = spline.get_direction_at_distance_along_spline(
                distance_cm, unreal.SplineCoordinateSpace.WORLD
            )
            direction_length_xy = math.hypot(direction.x, direction.y)
            cross_section = []
            if direction_length_xy > 1e-6:
                perpendicular_x = -direction.y / direction_length_xy
                perpendicular_y = direction.x / direction_length_xy
                # Sample just inside the area that Editor Apply Spline flattens
                # to the centre-line height. This reveals cross-slope cut/fill.
                edge_offset_cm = width_m * 50.0 * 0.9
                for side, sign in (("left", -1.0), ("right", 1.0)):
                    edge_x = source.x + perpendicular_x * edge_offset_cm * sign
                    edge_y = source.y + perpendicular_y * edge_offset_cm * sign
                    edge_z, edge_hit_actor = _trace_landscape_z(
                        world, edge_x, edge_y, ignored
                    )
                    edge_delta = None
                    if edge_z is not None:
                        edge_delta = (
                            r16.z
                            + landscape_tools.GROUND_OFFSET_CM
                            - edge_z
                        )
                        cross_section_deltas.append(edge_delta)
                    cross_section.append(
                        {
                            "side": side,
                            "offset_from_center_cm": _round(edge_offset_cm * sign),
                            "world_x_cm": _round(edge_x),
                            "world_y_cm": _round(edge_y),
                            "landscape_z_cm": (
                                _round(edge_z) if edge_z is not None else None
                            ),
                            "apply_target_minus_landscape_cm": (
                                _round(edge_delta) if edge_delta is not None else None
                            ),
                            "hit_actor": edge_hit_actor,
                        }
                    )
            sample = {
                "distance_cm": _round(distance_cm),
                "inside_dem_extent": inside,
                "world_x_cm": _round(source.x),
                "world_y_cm": _round(source.y),
                "source_spline_z_cm": _round(source.z),
                "r16_z_cm": _round(r16.z),
                "apply_target_z_cm": _round(
                    r16.z + landscape_tools.GROUND_OFFSET_CM
                ),
                "r16_flipped_y_z_cm": _round(r16_flipped.z),
                "landscape_z_cm": None,
                "hit_actor": hit_actor,
                "r16_target_minus_landscape_cm": None,
                "r16_flipped_y_minus_landscape_cm": None,
                "source_spline_minus_landscape_cm": None,
                "cross_section_inside_road_width": cross_section,
            }
            if landscape_z is None:
                miss_count += 1
            else:
                hit_count += 1
                normal_delta = r16.z + landscape_tools.GROUND_OFFSET_CM - landscape_z
                flipped_delta = r16_flipped.z - landscape_z
                source_delta = source.z - landscape_z
                normal_deltas.append(normal_delta)
                flipped_deltas.append(flipped_delta)
                source_deltas.append(source_delta)
                sample.update(
                    {
                        "landscape_z_cm": _round(landscape_z),
                        "r16_target_minus_landscape_cm": _round(normal_delta),
                        "r16_flipped_y_minus_landscape_cm": _round(flipped_delta),
                        "source_spline_minus_landscape_cm": _round(source_delta),
                    }
                )
            samples.append(sample)

        route_reports.append(
            {
                "label": actor.get_actor_label(),
                "actor_path": actor.get_path_name(),
                "route_type": route_type,
                "width_m": width_m,
                "length_cm": _round(spline.get_spline_length()),
                "control_point_count": spline.get_number_of_spline_points(),
                "sample_count": len(samples),
                "samples": samples,
            }
        )

    raised = sum(value > MEANINGFUL_DELTA_CM for value in normal_deltas)
    lowered = sum(value < -MEANINGFUL_DELTA_CM for value in normal_deltas)
    near = len(normal_deltas) - raised - lowered
    normal_stats = _stats(normal_deltas)
    flipped_stats = _stats(flipped_deltas)
    cross_section_stats = _stats(cross_section_deltas)
    if (
        normal_stats
        and cross_section_stats
        and normal_stats["mean_abs_cm"] <= MEANINGFUL_DELTA_CM
        and cross_section_stats["min_cm"] < -MEANINGFUL_DELTA_CM
        and cross_section_stats["max_cm"] > MEANINGFUL_DELTA_CM
    ):
        warnings.append(
            "The route centre line matches the Landscape, but flattening the full "
            "road width cuts the uphill side and fills the downhill side. This "
            "cross-slope cut/fill can look like alternating trenches and ridges."
        )
    orientation = None
    if normal_stats and flipped_stats:
        orientation = {
            "normal_y_mean_abs_cm": normal_stats["mean_abs_cm"],
            "flipped_y_mean_abs_cm": flipped_stats["mean_abs_cm"],
            "likely_flipped_y": (
                flipped_stats["mean_abs_cm"] + MEANINGFUL_DELTA_CM
                < normal_stats["mean_abs_cm"]
            ),
        }
        if orientation["likely_flipped_y"]:
            warnings.append(
                "The current Landscape matches a Y-flipped R16 more closely than "
                "the normal orientation. Check the Landscape import Flip Y Axis setting."
            )
    if outside_count:
        warnings.append(
            f"{outside_count} sample(s) are outside the DEM extent and R16 sampling "
            "is clamped to the nearest edge."
        )

    generated = datetime.now().astimezone()
    report = {
        "format_version": 1,
        "generated_local": generated.isoformat(timespec="seconds"),
        "project_file": unreal.Paths.get_project_file_path(),
        "world": world.get_path_name() if world else None,
        "sample_spacing_cm": SAMPLE_SPACING_CM,
        "meaningful_delta_cm": MEANINGFUL_DELTA_CM,
        "terrain_files": terrain_files,
        "landscape": _landscape_report(landscape),
        "summary": {
            "route_count": len(routes),
            "landscape_hits": hit_count,
            "landscape_misses": miss_count,
            "outside_dem_samples": outside_count,
            "would_raise_samples": raised,
            "would_lower_samples": lowered,
            "near_samples": near,
            "r16_target_minus_landscape": normal_stats,
            "source_spline_minus_landscape": _stats(source_deltas),
            "road_cross_section_target_minus_landscape": cross_section_stats,
            "cross_section_would_raise_samples": sum(
                value > MEANINGFUL_DELTA_CM for value in cross_section_deltas
            ),
            "cross_section_would_lower_samples": sum(
                value < -MEANINGFUL_DELTA_CM for value in cross_section_deltas
            ),
            "orientation_check": orientation,
        },
        "warnings": warnings,
        "routes": route_reports,
    }

    output_dir = exchange_dir() / "Diagnostics"
    output_dir.mkdir(parents=True, exist_ok=True)
    output_path = output_dir / generated.strftime(
        "landscape_spline_diagnostic_%Y%m%d_%H%M%S.json"
    )
    report["report_path"] = str(output_path)
    output_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    return report


def diagnose_selected_splines() -> None:
    """Write and show a read-only report for selected imported route splines."""

    try:
        subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        routes = landscape_tools._selected_routes(subsystem)
        if not routes:
            raise RuntimeError(
                "Select one or more imported MainRoad, BranchRoad or River "
                "Spline Actors, then run this command again."
            )
        landscape = landscape_tools._target_landscape(subsystem)
        report = build_diagnostic_report(routes, landscape)
        summary = _summary_text(report)
        unreal.log("PS2DEM Landscape diagnostic\n" + summary)
        try:
            os.startfile(str(Path(report["report_path"]).parent))
        except Exception:
            pass
        show_message("PS2DEM Landscape Diagnostic", summary)
    except Exception as exc:
        unreal.log_error(str(exc))
        show_message("PS2DEM Landscape Diagnostic Failed", str(exc))
        raise
