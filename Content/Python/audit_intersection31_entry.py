"""Read the upstream turn and start of Lane_MainRoad_R003_-0540_S06."""

import json
import math
import unreal

WS = unreal.SplineCoordinateSpace.WORLD
OUT = "/private/tmp/intersection31_entry.json"
LABELS = ("Inter_BranchRoad_R037_-0180_I03", "Lane_MainRoad_R003_-0540_S06",
          "Inter_MainRoad_R003_-0540_I05")


def vec(v):
    return [v.x, v.y, v.z]


def main():
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if not world or world.get_name() != "TestForCharacter":
        assert levels.load_level("/Game/Level/TestForCharacter")
        world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    actors = {a.get_actor_label():a for a in
        unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()}
    lane_start = next(s for s in actors[LABELS[1]].get_components_by_class(unreal.SplineComponent)
                      if s.get_name() == "Spline").get_location_at_spline_point(0,WS)
    upstream = []
    for actor in actors.values():
        if not actor.get_actor_label().startswith("Inter_"): continue
        for sp in actor.get_components_by_class(unreal.SplineComponent):
            n = sp.get_number_of_spline_points()
            if n < 2: continue
            p = sp.get_location_at_spline_point(n-1,WS)
            if math.hypot(p.x-lane_start.x,p.y-lane_start.y)<600:
                upstream.append((actor.get_actor_label(),sp.get_name(),vec(p)))
    rows = []
    samples = []
    for label in LABELS:
        actor = actors[label]
        for sp in actor.get_components_by_class(unreal.SplineComponent):
            n = sp.get_number_of_spline_points()
            if n < 2: continue
            pts = [sp.get_location_at_spline_point(i,WS) for i in range(n)]
            row = {"actor":label,"component":sp.get_name(),"points":[vec(p) for p in pts],
                   "endpoint_tangents":[vec(sp.get_tangent_at_spline_point(i,WS)) for i in (0,n-1)]}
            rows.append(row)
            if (label == LABELS[0] and sp.get_name() == "SplineLeft") or \
               (label == LABELS[2] and sp.get_name() == "Spline"):
                for i,p in enumerate(pts):
                    hits = unreal.SystemLibrary.line_trace_multi(world,
                        unreal.Vector(p.x,p.y,p.z+300),unreal.Vector(p.x,p.y,p.z-300),
                        unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,False,[],unreal.DrawDebugTrace.NONE,True)
                    roads = [round(h.to_dict()["impact_point"].z,2) for h in hits or []
                             if h.to_dict().get("hit_component") and
                             "SplineMesh" in h.to_dict()["hit_component"].get_class().get_name()]
                    row.setdefault("point_roads",[]).append(roads)
            if (label == LABELS[0] and sp.get_name() == "SplineLeft") or \
               (label == LABELS[2] and sp.get_name() == "Spline") or \
               (label == LABELS[1] and sp.get_name() == "Spline"):
                length = sp.get_spline_length()
                distances = ([length-500,length-250,length-100,length] if label != LABELS[1]
                             else [0,100,250,500,750])
                for distance in distances:
                    p = sp.get_location_at_distance_along_spline(distance,WS)
                    hits = unreal.SystemLibrary.line_trace_multi(world,
                        unreal.Vector(p.x,p.y,p.z+300),unreal.Vector(p.x,p.y,p.z-300),
                        unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,False,[],unreal.DrawDebugTrace.NONE,True)
                    roads = [round(h.to_dict()["impact_point"].z,2) for h in hits or []
                             if h.to_dict().get("hit_component") and
                             "SplineMesh" in h.to_dict()["hit_component"].get_class().get_name()]
                    samples.append({"actor":label,"component":sp.get_name(),
                                    "distance":distance,"point":vec(p),"roads":roads})
    with open(OUT,"w",encoding="utf-8") as f:
        json.dump({"splines":rows,"samples":samples,"nearby_upstream":upstream},f,indent=2)
    unreal.log("[Inter31Entry] wrote " + OUT)


main()
