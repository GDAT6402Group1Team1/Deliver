"""Reprofile the two paths feeding Lane_MainRoad_R003_-0540_S06.

Only Z values on the upstream intersection splines change. XY, lane points,
other turns, traffic boxes, and actor transforms remain untouched.
"""

import json
import math
import unreal

LEVEL = "/Game/Level/TestForCharacter"
AUDIT = "/private/tmp/intersection31_entry.json"
REPORT = "/private/tmp/intersection31_entry_fix.json"
WS = unreal.SplineCoordinateSpace.WORLD
LEFT = "Inter_BranchRoad_R037_-0180_I03"
STRAIGHT = "Inter_MainRoad_R003_-0540_I05"
LANE = "Lane_MainRoad_R003_-0540_S06"


def grade(a, b):
    return 100.0 * (b.z-a.z) / math.hypot(b.x-a.x,b.y-a.y)


def main():
    audit = json.load(open(AUDIT, encoding="utf-8"))
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    assert levels.load_level(LEVEL)
    actors = {a.get_actor_label(): a for a in
              unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()}
    assert LEFT in actors and STRAIGHT in actors and LANE in actors
    originals = {(r["actor"],r["component"]):r for r in audit["splines"]}
    edited = []
    endpoint_z = None
    for label,names in ((LEFT,("SplineLeft",)),
                        (STRAIGHT,("Spline","SplineLeft","SplineRight"))):
        actor = actors[label]
        components = {s.get_name():s for s in actor.get_components_by_class(unreal.SplineComponent)}
        for name in names:
            sp = components[name]
            old = originals[(label,name)]["points"]
            assert sp.get_number_of_spline_points() == len(old)
            before = [sp.get_location_at_spline_point(i,WS) for i in range(len(old))]
            for p,q in zip(before,old):
                assert max(abs(p.x-q[0]),abs(p.y-q[1]),abs(p.z-q[2])) < 0.1, \
                    "Spline changed since audit: %s.%s" % (label,name)
            if label == LEFT:
                surfaces = originals[(label,name)]["point_roads"]
                assert len(surfaces)==len(before) and all(s for s in surfaces[1:])
                zs = [p.z for p in before]
                for i in range(1,len(zs)):
                    zs[i] = surfaces[i][0]+2.0
                # One road-mesh sample dips 2 cm at point 15; smooth that local kink.
                zs[15] = 0.5*(zs[14]+zs[16])
                endpoint_z = zs[-1]
            else:
                assert endpoint_z is not None
                zs = [p.z for p in before]
                zs[-2] = 3650.8  # measured road top 3648.8 + 2 cm
                zs[-1] = endpoint_z
            for i,(p,z) in enumerate(zip(before,zs)):
                if abs(z-p.z) > 0.01:
                    sp.set_location_at_spline_point(i,unreal.Vector(p.x,p.y,z),WS,False)
            sp.update_spline()
            sp.set_editor_property("spline_has_been_edited",True)
            after = [sp.get_location_at_spline_point(i,WS) for i in range(len(old))]
            assert all(math.hypot(a.x-b.x,a.y-b.y)<0.05 for a,b in zip(after,before))
            edited.append({"actor":label,"component":name,"changed_points":
                           [i for i,(a,b) in enumerate(zip(after,before)) if abs(a.z-b.z)>0.01],
                           "last_grades_pct":[grade(after[-3],after[-2]),grade(after[-2],after[-1])]})
    lane_sp = next(s for s in actors[LANE].get_components_by_class(unreal.SplineComponent)
                   if s.get_name()=="Spline")
    lane_start = lane_sp.get_location_at_spline_point(0,WS)
    lane_next = lane_sp.get_location_at_spline_point(1,WS)
    for label,name in ((LEFT,"SplineLeft"),(STRAIGHT,"Spline")):
        sp = next(s for s in actors[label].get_components_by_class(unreal.SplineComponent)
                  if s.get_name()==name)
        end = sp.get_location_at_spline_point(sp.get_number_of_spline_points()-1,WS)
        gap = math.hypot(end.x-lane_start.x,end.y-lane_start.y)
        assert 140 < gap < 160
        assert abs(grade(end,lane_start)-grade(lane_start,lane_next)) < 1.0
    report={"edited":edited,"exit_z":endpoint_z,"gap_grade_pct":grade(end,lane_start),
            "lane_first_grade_pct":grade(lane_start,lane_next)}
    assert levels.save_current_level()
    report["saved"] = True
    with open(REPORT,"w",encoding="utf-8") as f:json.dump(report,f,indent=2)
    unreal.log("[Inter31] saved entry slope repair: " + str(report))


main()
