"""Shared project-relative paths and UI helpers."""

import json
import math
import struct
from pathlib import Path

import unreal


def exchange_dir() -> Path:
    path = Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir())) / "PS2DEMImporter"
    path.mkdir(parents=True, exist_ok=True)
    return path


def buildings_json_path() -> Path:
    return exchange_dir() / "buildings.json"


def splines_json_path() -> Path:
    return exchange_dir() / "splines.json"


def terrain_dir() -> Path:
    preferred = exchange_dir() / "Terrain"
    preferred.mkdir(parents=True, exist_ok=True)
    return preferred


class TerrainHeightSampler:
    """Sample the R16 data used to create the current project's Landscape."""

    def __init__(self) -> None:
        metadata_path = terrain_dir() / "metadata.json"
        r16_path = terrain_dir() / "height_ue.r16"
        if not metadata_path.is_file() or not r16_path.is_file():
            raise RuntimeError(
                "Terrain metadata.json and height_ue.r16 are required:\n"
                f"{terrain_dir()}"
            )
        metadata = json.loads(metadata_path.read_text(encoding="utf-8-sig"))
        resolution = metadata.get("output_resolution")
        if not isinstance(resolution, list) or len(resolution) != 2:
            raise RuntimeError("Terrain metadata has no valid output_resolution")
        self.width = int(resolution[0])
        self.height = int(resolution[1])
        self.map_width_m = float(metadata["map_width_m"])
        self.map_height_m = float(metadata["map_height_m"])
        self.min_elevation_m = float(metadata["min_elevation_m"])
        self.height_range_m = float(metadata["height_range_m"])
        self.data = r16_path.read_bytes()
        expected_bytes = self.width * self.height * 2
        if len(self.data) != expected_bytes:
            raise RuntimeError(
                f"Terrain R16 size mismatch: expected {expected_bytes} bytes, "
                f"got {len(self.data)}"
            )
        self.outside_points: set[tuple[float, float]] = set()

    def _value(self, x: int, y: int) -> int:
        offset = (y * self.width + x) * 2
        return struct.unpack_from("<H", self.data, offset)[0]

    def location(
        self, x_m: float, y_m: float, z_offset_cm: float = 0.0
    ) -> unreal.Vector:
        u = (x_m / self.map_width_m + 0.5) * (self.width - 1)
        v = (y_m / self.map_height_m + 0.5) * (self.height - 1)
        if u < 0.0 or u > self.width - 1 or v < 0.0 or v > self.height - 1:
            self.outside_points.add((round(x_m, 4), round(y_m, 4)))
        u = min(max(u, 0.0), self.width - 1.0)
        v = min(max(v, 0.0), self.height - 1.0)
        x0, y0 = int(math.floor(u)), int(math.floor(v))
        x1, y1 = min(x0 + 1, self.width - 1), min(y0 + 1, self.height - 1)
        tx, ty = u - x0, v - y0
        top = self._value(x0, y0) * (1.0 - tx) + self._value(x1, y0) * tx
        bottom = self._value(x0, y1) * (1.0 - tx) + self._value(x1, y1) * tx
        raw_height = top * (1.0 - ty) + bottom * ty
        elevation_m = (
            self.min_elevation_m + raw_height / 65535.0 * self.height_range_m
        )
        return unreal.Vector(
            x_m * 100.0,
            y_m * 100.0,
            elevation_m * 100.0 + z_offset_cm,
        )


def show_message(title: str, message: str) -> None:
    unreal.EditorDialog.show_message(title, message, unreal.AppMsgType.OK)


def show_exchange_folder() -> None:
    path = exchange_dir()
    try:
        import os

        os.startfile(str(path))
    except Exception as exc:
        show_message("PS2DEM Exchange Folder", f"{path}\n\nCould not open Explorer: {exc}")
