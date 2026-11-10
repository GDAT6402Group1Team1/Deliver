"""Place twelve roaming pedestrians near central crosswalks in TestForCharacter.

These area routes are separate from the long spline routes.
Rerunning replaces only actors tagged DeliveryCentralPedestrianAreaV1.
"""

import json
import math

import unreal


LEVEL = "/Game/Level/TestForCharacter"
TAG = "DeliveryCentralPedestrianAreaV1"
FOLDER = "Pedestrians/CrosswalkAreas"
RADIUS = 500.0
ROAD_CLEARANCE = 120.0  # keep the whole roaming circle off road meshes
AREAS = (
    ("Crosswalk_C", 1300.0, 5500.0, 2, "Crosswalk_MainRoad_R003_2"),
    ("Crosswalk_D", 900.0, 5500.0, 2, "Crosswalk_BranchRoad_R024_Merged_0"),
    ("Crosswalk_E", 4300.0, 2300.0, 1, "Crosswalk_MainRoad_R003_3"),
    ("Crosswalk_F", 4700.0, 2500.0, 1, "Crosswalk_BranchRoad_R024_Merged_1"),
    ("Crosswalk_G", -700.0, -6000.0, 2, "Crosswalk_MainRoad_R003_18"),
)
ROAD_MESH = "/Game/model/road_0.road_0"
KINDS = (("SK_NpcBoy", 1.0, 0.60), ("SK_NpcGirl", 1.0, 0.60),
         ("SK_NpcMan", 1.0, 1.00), ("SK_NpcWoman", 1.0, 1.00),
         ("SK_NpcGrandpa", 1.0, 0.80), ("SK_NpcGrandma", 1.0, 0.80))
REPORT = "/private/tmp/delivery_central_pedestrians.json"


def xy_distance_to_bounds(x, y, origin, extent):
    return math.hypot(max(abs(x - origin.x) - extent.x, 0.0),
                      max(abs(y - origin.y) - extent.y, 0.0))


def terrain_height(world, x, y):
    hits = unreal.SystemLibrary.line_trace_multi(
        world, unreal.Vector(x, y, 7000), unreal.Vector(x, y, -1000),
        unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, False, [],
        unreal.DrawDebugTrace.NONE, True)
    if not hits:
        raise RuntimeError("No ground at %.0f, %.0f" % (x, y))
    hit = hits[0].to_dict()
    actor = hit.get("hit_actor")
    component = hit.get("hit_component")
    if not actor or actor.get_actor_label() != "Landscape" or not component or \
            not component.get_name().startswith("LandscapeHeightfieldCollisionComponent"):
        raise RuntimeError("Not open terrain at %.0f, %.0f: %s" %
                           (x, y, actor.get_actor_label() if actor else "no actor"))
    return float(hit["impact_point"].z)


def main():
    level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not level.load_level(LEVEL):
        raise RuntimeError("Cannot load " + LEVEL)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()
    route_class = getattr(unreal, "DeliveryPedestrianRoute", None)
    if route_class is None:
        raise RuntimeError("DeliveryPedestrianRoute is unavailable; build DeliveryEditor first")

    kinds = []
    for asset_name, weight, height in KINDS:
        path = "/Game/Characters/NPC/%s.%s" % (asset_name, asset_name)
        mesh = unreal.load_asset(path)
        if not isinstance(mesh, unreal.SkeletalMesh):
            raise RuntimeError("Missing pedestrian mesh: " + path)
        kind = unreal.DeliveryPedestrianKind()
        kind.set_editor_property("mesh", mesh)
        kind.set_editor_property("weight", weight)
        kind.set_editor_property("height_scale", height)
        kinds.append(kind)

    road_bounds = []
    building_bounds = []
    crosswalks = {actor.get_actor_label(): actor for actor in actors
                  if actor.get_actor_label().startswith("Crosswalk_")}
    for actor in actors:
        label = actor.get_actor_label().lower()
        is_building = any(word in label for word in
                          ("house", "shop", "building", "wall", "fence", "park"))
        for component in actor.get_components_by_class(unreal.StaticMeshComponent):
            mesh = component.get_editor_property("static_mesh")
            is_building |= bool(mesh and "/buildings/" in mesh.get_path_name().lower())
            if mesh and mesh.get_path_name() == ROAD_MESH:
                origin, extent, _ = unreal.SystemLibrary.get_component_bounds(component)
                road_bounds.append((origin, extent))
        if is_building:
            building_bounds.append(actor.get_actor_bounds(False))
    if len(road_bounds) < 100 or len(building_bounds) < 15:
        raise RuntimeError("Road or building geometry is missing; do not place unsafely")

    checked = []
    for name, x, y, count, crosswalk_label in AREAS:
        crosswalk = crosswalks.get(crosswalk_label)
        if crosswalk is None:
            raise RuntimeError("Missing crosswalk " + crosswalk_label)
        crosswalk_distance = math.hypot(x - crosswalk.get_actor_location().x,
                                        y - crosswalk.get_actor_location().y)
        if crosswalk_distance > 2200.0:
            raise RuntimeError("Area too far from crosswalk: " + name)
        road_distance = min(xy_distance_to_bounds(x, y, o, e) for o, e in road_bounds)
        building_distance = min(xy_distance_to_bounds(x, y, o, e)
                                for o, e in building_bounds)
        if road_distance < RADIUS + ROAD_CLEARANCE:
            raise RuntimeError("Road too close to " + name)
        if building_distance <= 0.0:
            raise RuntimeError("Area center inside a building: " + name)
        z = terrain_height(world, x, y)
        checked.append((name, x, y, count, z, road_distance, building_distance,
                        crosswalk_label, crosswalk_distance))

    for actor in actors:
        if TAG in [str(tag) for tag in actor.tags]:
            if not eas.destroy_actor(actor):
                raise RuntimeError("Cannot replace old pedestrian area")

    result = []
    for (name, x, y, count, z, road_distance, building_distance,
         crosswalk_label, crosswalk_distance) in checked:
        route = eas.spawn_actor_from_class(route_class, unreal.Vector(x, y, z + 2.0))
        if route is None:
            raise RuntimeError("Cannot spawn " + name)
        route.set_actor_label("PedestrianRoute_" + name)
        route.set_folder_path(FOLDER)
        route.set_editor_property("tags", [TAG])
        route.set_editor_property("kinds", kinds)
        route.set_editor_property("roam_in_area", True)
        route.set_editor_property("roam_radius", RADIUS)
        route.set_editor_property("allow_handoff", False)
        route.set_editor_property("max_alive", count)
        route.set_editor_property("prewarm_on_begin_play", True)
        route.set_editor_property("roam_spawn_spacing", 90.0)
        route.set_editor_property("roam_pause_distance_min", 120.0)
        route.set_editor_property("roam_pause_distance_max", 320.0)
        result.append({"label": route.get_actor_label(), "x": x, "y": y, "z": z + 2,
                       "radius": RADIUS, "max_alive": count,
                       "road_edge_distance": road_distance,
                       "building_bounds_distance": building_distance,
                       "crosswalk": crosswalk_label,
                       "crosswalk_center_distance": crosswalk_distance})
    if not level.save_current_level():
        raise RuntimeError("Cannot save TestForCharacter")
    with open(REPORT, "w", encoding="utf-8") as handle:
        json.dump(result, handle, ensure_ascii=False, indent=2)
    unreal.log("[CentralPedestrians] Saved %d areas, %d pedestrians maximum" %
               (len(result), sum(row["max_alive"] for row in result)))


if __name__ == "__main__":
    main()
