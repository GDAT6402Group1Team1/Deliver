import unreal

TARGET = "Inter_BranchRoad_R037_-0180_I03"
WS = unreal.SplineCoordinateSpace.WORLD
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
for actor in actors:
    if actor.get_actor_label() != TARGET:
        continue
    unreal.log("[Inter31Inspect] actor=" + actor.get_path_name())
    for spline in actor.get_components_by_class(unreal.SplineComponent):
        count = spline.get_number_of_spline_points()
        parts = ["name=%s" % spline.get_name(), "path=%s" % spline.get_path_name(),
                 "points=%d" % count]
        if count:
            first = spline.get_location_at_spline_point(0, WS)
            last = spline.get_location_at_spline_point(count-1, WS)
            parts += ["first=(%.1f,%.1f,%.1f)" % (first.x,first.y,first.z),
                      "last=(%.1f,%.1f,%.1f)" % (last.x,last.y,last.z)]
        unreal.log("[Inter31Inspect] " + " ".join(parts))
