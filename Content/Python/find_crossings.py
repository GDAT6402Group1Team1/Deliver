# -*- coding: utf-8 -*-
"""① 查 BP_TrafficLine1_IntersectionChild 的结构  ② 找 形状13 与其它道路的交汇点。

交汇检测用 find_location_closest_to_world_location（原生调用，快且精确），
对 形状13 的每个采样点问"其它每条路上离它最近的点有多远"。
XY 距离近但 Z 差很大 = 立交桥，不算路口，要排除。
"""

import traceback

import unreal

ROAD_LABEL = "形状 13"
CHILD_BP = "/Game/PS2DEM/BP_TrafficLine1_IntersectionChild"
INTER_BP = "/Game/PS2DEM/BP_Intersection"
STEP = 250.0
XY_NEAR = 900.0      # 路宽 1000，半宽 500，留点余量
Z_NEAR = 600.0       # Z 差超过这个视为立交，不是平面路口
OUT = unreal.Paths.project_saved_dir() + "crossings.txt"
WS = unreal.SplineCoordinateSpace.WORLD

lines = []


def w(s=""):
    lines.append(str(s))


def dump(actor, indent="    "):
    for c in actor.get_components_by_class(unreal.ActorComponent):
        try:
            w("%s%-30s %s" % (indent, c.get_name(), type(c).__name__))
            if isinstance(c, unreal.BoxComponent):
                w("%s    extent=%s objtype=%s" % (
                    indent, c.get_editor_property("box_extent"),
                    c.get_editor_property("collision_object_type")))
            elif isinstance(c, unreal.SplineComponent):
                n = c.get_number_of_spline_points()
                w("%s    样条 %d 点  长 %.0f" % (indent, n, c.get_spline_length()))
                for i in range(min(n, 8)):
                    p = c.get_location_at_spline_point(i, unreal.SplineCoordinateSpace.LOCAL)
                    w("%s      [%d] (%.0f, %.0f, %.0f)" % (indent, i, p.x, p.y, p.z))
        except Exception as exc:
            w("%s(失败 %s)" % (indent, exc))


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    # ---- 1. 路口相关蓝图的结构 ----
    for path, name in ((CHILD_BP, "BP_TrafficLine1_IntersectionChild"), (INTER_BP, "BP_Intersection")):
        w("=" * 70)
        w("结构: %s" % name)
        w("=" * 70)
        try:
            asset = unreal.EditorAssetLibrary.load_asset(path)
            if asset is None:
                w("  资产加载不到")
                continue
            tmp = eas.spawn_actor_from_class(asset.generated_class(),
                                             unreal.Vector(0, 0, -300000))
            dump(tmp)
            eas.destroy_actor(tmp)
        except Exception:
            w(traceback.format_exc())
        w("")

    # ---- 2. 交汇检测 ----
    w("=" * 70)
    w("形状 13 与其它道路样条的交汇点")
    w("=" * 70)
    target = None
    roads = []
    for a in actors:
        if a.get_class().get_name() != "BP_PS2DEMSplineActor_v3_C":
            continue
        if a.get_actor_label().strip() == ROAD_LABEL:
            target = a
        else:
            sp = a.get_component_by_class(unreal.SplineComponent)
            if sp:
                roads.append((a.get_actor_label(), sp))
    if target is None:
        w("找不到 %s" % ROAD_LABEL)
        return
    tsp = target.get_component_by_class(unreal.SplineComponent)
    total = tsp.get_spline_length()
    w("主路总长 %.0f cm，其它道路 %d 条，采样间距 %.0f" % (total, len(roads), STEP))

    raw = []
    d = 0.0
    while d <= total:
        p = tsp.get_location_at_distance_along_spline(d, WS)
        for name, osp in roads:
            q = osp.find_location_closest_to_world_location(p, WS)
            dxy = ((q.x - p.x) ** 2 + (q.y - p.y) ** 2) ** 0.5
            if dxy < XY_NEAR:
                dz = abs(q.z - p.z)
                raw.append((d, name, dxy, dz, p, q))
        d += STEP

    # 聚类：同一条路上连续的采样点算一个路口
    w("")
    clusters = []
    for item in raw:
        placed = False
        for c in clusters:
            if c["road"] == item[1] and item[0] - c["dmax"] <= STEP * 3:
                c["dmax"] = item[0]
                c["items"].append(item)
                placed = True
                break
        if not placed:
            clusters.append({"road": item[1], "dmin": item[0], "dmax": item[0], "items": [item]})

    w("检测到 %d 处交汇（XY < %.0f cm）" % (len(clusters), XY_NEAR))
    w("")
    w("%4s %-12s %10s %10s %9s %9s  %s" %
      ("#", "对向道路", "起(沿线)", "止(沿线)", "最近XY", "Z差", "判定"))
    for i, c in enumerate(clusters):
        best = min(c["items"], key=lambda t: t[2])
        verdict = "平面路口" if best[3] < Z_NEAR else "立交(Z差大，跳过)"
        w("%4d %-12s %10.0f %10.0f %9.0f %9.0f  %s"
          % (i + 1, c["road"], c["dmin"], c["dmax"], best[2], best[3], verdict))
        p = best[4]
        w("       世界坐标 (%.0f, %.0f, %.0f)" % (p.x, p.y, p.z))

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    unreal.log("[find_crossings] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[find_crossings] 失败:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("失败:\n" + err)
