# -*- coding: utf-8 -*-
"""只读：检查 R024_Merged 两个方向的 S00/S01 端点，供定点合并前核对。"""

import math
import unreal

WS = unreal.SplineCoordinateSpace.WORLD
PREFIX = "Lane_BranchRoad_R024_Merged_"
OUT = unreal.Paths.project_saved_dir() + "diag_merge_r024.txt"


def xyz(v):
    return "(%.1f, %.1f, %.1f)" % (v.x, v.y, v.z)


def distance(a, b):
    return math.sqrt((a.x-b.x)**2 + (a.y-b.y)**2 + (a.z-b.z)**2)


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    rows = ["关卡: %s" % world.get_name(), "只读检查；未修改 Actor。"]
    actors = {a.get_actor_label(): a for a in eas.get_all_level_actors()}
    for sign in ("+0180", "-0180"):
        rows.append("\n方向 %s" % sign)
        endpoints = {}
        for suffix in ("S00", "S01", "S02"):
            label = PREFIX + sign + "_" + suffix
            a = actors.get(label)
            if a is None:
                rows.append("  缺失: %s" % label)
                continue
            sp = a.get_component_by_class(unreal.SplineComponent)
            if sp is None:
                rows.append("  没有 SplineComponent: %s" % label)
                continue
            n = sp.get_number_of_spline_points()
            points = [sp.get_location_at_spline_point(i, WS) for i in range(n)]
            endpoints[suffix] = points
            rows.append("  %s: %d 点, 长 %.1f cm" % (label, n, sp.get_spline_length()))
            for i, p in enumerate(points):
                rows.append("    [%02d] %s" % (i, xyz(p)))
        if "S00" in endpoints and "S01" in endpoints:
            p, q = endpoints["S00"], endpoints["S01"]
            for name, a, b in (("S00末→S01首", p[-1], q[0]),
                               ("S01末→S00首", q[-1], p[0]),
                               ("S00末→S01末", p[-1], q[-1]),
                               ("S00首→S01首", p[0], q[0])):
                rows.append("  %s: %.1f cm (XY %.1f cm, ΔZ %.1f cm)" % (
                    name, distance(a, b), math.hypot(a.x-b.x, a.y-b.y), b.z-a.z))
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(rows) + "\n")
    unreal.log("[diag_merge_r024] 写入 %s" % OUT)


run()
