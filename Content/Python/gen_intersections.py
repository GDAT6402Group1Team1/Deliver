# -*- coding: utf-8 -*-
"""在每个路口放一个 BP_Intersection，Box 只包住路口段、不碰普通路段。

做法：不再几何重算路口，而是直接读已生成的 Inter_* actor —— 按世界坐标聚类，
用它们样条点的实际包围盒定 Box 尺寸。这样天然贴合，而且能直接验证
有没有误包到 Lane_*（普通路段），有就自动收缩并报告。

依赖：先跑 gen_traffic_lanes.py。
"""

import traceback

import unreal

INTER_BP = "/Game/PS2DEM/BP_Intersection"
LANE_TAG_PREFIX = "ClaudeGenLane"
TAG = "ClaudeGenIntersection"

CLUSTER_DIST = 2500.0    # 世界坐标相距小于此值的路口段归为同一个物理路口
MARGIN = 100.0           # 包围盒外扩，确保完整包住路口段
MIN_CLEAR = 80.0         # 与最近的 Lane_* 点至少保持这么远

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "gen_intersections.txt"

lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[gen_inter] %s" % s)


def spline_points(actor):
    sp = actor.get_component_by_class(unreal.SplineComponent)
    if sp is None:
        return []
    return [sp.get_location_at_spline_point(i, WS)
            for i in range(sp.get_number_of_spline_points())]


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    removed = 0
    for a in actors:
        if TAG in [str(t) for t in a.tags]:
            eas.destroy_actor(a)
            removed += 1
    w("清除旧路口 actor %d 个" % removed)

    inter_actors, lane_actors = [], []
    for a in actors:
        if not any(str(t).startswith(LANE_TAG_PREFIX) for t in a.tags):
            continue
        lbl = a.get_actor_label()
        if lbl.startswith("Inter_"):
            inter_actors.append(a)
        elif lbl.startswith("Lane_"):
            lane_actors.append(a)
    w("路口段 actor %d 个，普通路段 actor %d 个" % (len(inter_actors), len(lane_actors)))
    if not inter_actors:
        w("!! 没有 Inter_* actor，请先跑 gen_traffic_lanes.py")
        return

    # 所有普通路段的点，用来检查有没有被误包
    lane_pts = []
    for a in lane_actors:
        lane_pts.extend(spline_points(a))
    w("普通路段样条点共 %d 个（用于越界检查）" % len(lane_pts))

    # ---- 按世界坐标把路口段聚成物理路口 ----
    clusters = []
    for a in inter_actors:
        pts = spline_points(a)
        if not pts:
            continue
        cx = sum(p.x for p in pts) / len(pts)
        cy = sum(p.y for p in pts) / len(pts)
        cz = sum(p.z for p in pts) / len(pts)
        c = unreal.Vector(cx, cy, cz)
        got = None
        for cl in clusters:
            if (cl["center"] - c).length() < CLUSTER_DIST:
                got = cl
                break
        if got is None:
            clusters.append({"center": c, "pts": list(pts), "names": [a.get_actor_label()]})
        else:
            got["pts"].extend(pts)
            got["names"].append(a.get_actor_label())
            n = len(got["names"])
            got["center"] = unreal.Vector(
                (got["center"].x * (n - 1) + cx) / n,
                (got["center"].y * (n - 1) + cy) / n,
                (got["center"].z * (n - 1) + cz) / n)
    w("聚类出 %d 个物理路口" % len(clusters))
    w("")

    cls = unreal.EditorAssetLibrary.load_asset(INTER_BP).generated_class()
    probe = eas.spawn_actor_from_class(cls, unreal.Vector(0, 0, -300000))
    base = None
    for c in probe.get_components_by_class(unreal.BoxComponent):
        base = c.get_unscaled_box_extent()
        break
    eas.destroy_actor(probe)
    if base is None:
        w("!! 读不到 BP_Intersection 的 Box")
        return
    w("BP_Intersection 默认 Box 半尺寸 (%.1f, %.1f, %.1f)" % (base.x, base.y, base.z))
    w("")

    shrunk = 0
    for i, cl in enumerate(clusters):
        pts = cl["pts"]
        cx = (min(p.x for p in pts) + max(p.x for p in pts)) / 2.0
        cy = (min(p.y for p in pts) + max(p.y for p in pts)) / 2.0
        cz = (min(p.z for p in pts) + max(p.z for p in pts)) / 2.0
        ex = (max(p.x for p in pts) - min(p.x for p in pts)) / 2.0 + MARGIN
        ey = (max(p.y for p in pts) - min(p.y for p in pts)) / 2.0 + MARGIN
        ez = (max(p.z for p in pts) - min(p.z for p in pts)) / 2.0 + MARGIN
        center = unreal.Vector(cx, cy, cz)

        # 收缩直到不再包住任何普通路段的点
        note = ""
        for _ in range(40):
            inside = [p for p in lane_pts
                      if abs(p.x - cx) < ex + MIN_CLEAR
                      and abs(p.y - cy) < ey + MIN_CLEAR
                      and abs(p.z - cz) < ez + MIN_CLEAR]
            if not inside:
                break
            ex *= 0.92
            ey *= 0.92
            note = " (为避开普通路段已收缩)"
            shrunk += 1
        else:
            note = " (!! 收缩 40 次仍与普通路段重叠)"

        a = eas.spawn_actor_from_class(cls, center)
        a.set_actor_label("Intersection_%02d" % i)
        a.tags = [TAG]
        a.set_actor_scale3d(unreal.Vector(ex / base.x, ey / base.y, ez / base.z))
        w("  Intersection_%02d (%.0f, %.0f, %.0f)  半尺寸(%.0f, %.0f, %.0f)  含 %d 段%s"
          % (i, cx, cy, cz, ex, ey, ez, len(cl["names"]), note))

    w("")
    w("共放置 %d 个 BP_Intersection（%d 个做过收缩）" % (len(clusters), shrunk))
    w("Box 是靠 actor 缩放放大的；若 BP 内部按未缩放 extent 判断，需改成直接设 box_extent。")
    w("完成。关卡尚未保存。")

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[gen_inter] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
