"""Validate PS2DEM terrain output and prepare Unreal's Landscape import UI."""

from __future__ import annotations

import json
from pathlib import Path

import unreal

from ps2dem_common import exchange_dir, show_message, terrain_dir


def find_metadata() -> Path:
    candidates = [terrain_dir() / "metadata.json", exchange_dir() / "metadata.json"]
    for path in candidates:
        if path.is_file():
            return path
    raise RuntimeError(
        "metadata.json was not found. Copy the complete DEM output into:\n"
        f"{terrain_dir()}"
    )


def import_terrain() -> None:
    try:
        metadata_path = find_metadata()
        data = json.loads(metadata_path.read_text(encoding="utf-8-sig"))
        resolution = data.get("output_resolution")
        if not isinstance(resolution, list) or len(resolution) != 2:
            raise RuntimeError("metadata.json has no valid output_resolution")
        r16 = metadata_path.with_name("height_ue.r16")
        if not r16.is_file():
            raise RuntimeError(f"height_ue.r16 was not found beside metadata.json:\n{r16}")
        expected_bytes = int(resolution[0]) * int(resolution[1]) * 2
        if r16.stat().st_size != expected_bytes:
            raise RuntimeError(
                f"R16 size mismatch: expected {expected_bytes} bytes, got {r16.stat().st_size}"
            )

        # UE 5.8 documents the Landscape UI import workflow, but does not expose
        # a stable public Python API for constructing a new Landscape from R16.
        # Enter Landscape mode and present the validated values in one click.
        world = unreal.EditorLevelLibrary.get_editor_world()
        unreal.SystemLibrary.execute_console_command(world, "MODE LANDSCAPE")
        message = (
            "Terrain files validated. Landscape mode has been opened.\n\n"
            f"Heightmap: {r16}\n"
            f"Resolution: {resolution[0]} x {resolution[1]}\n"
            f"X Scale: {data['ue_x_scale_cm']}\n"
            f"Y Scale: {data['ue_y_scale_cm']}\n"
            f"Z Scale: {data['ue_z_scale']}\n"
            f"Actor Z after import: {data['recommended_landscape_actor_z_cm']}\n\n"
            "Choose Import from File, select the R16 above, enter the scales, then click Import."
        )
        unreal.log(message)
        show_message("PS2DEM Terrain Import", message)
    except Exception as exc:
        unreal.log_error(str(exc))
        show_message("PS2DEM Terrain Import Failed", str(exc))
        raise
