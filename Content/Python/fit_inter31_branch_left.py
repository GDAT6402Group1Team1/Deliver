# -*- coding: utf-8 -*-
"""重铺 31 号路口的左转路径：均匀布点、路面测高、保住两端接缝。

编辑器里打开 TestForCharacter 后运行：py fit_inter31_branch_left.py
只修改 Inter_BranchRoad_R037_-0180_I03.SplineLeft，不自动保存关卡。
"""

import bisect
import math
import traceback

import unreal

ACTOR = "Inter_BranchRoad_R037_-0180_I03"
COMPONENT = "SplineLeft"
DOWNSTREAM = "Lane_MainRoad_R003_-0540_S06"
SPACING = 40.0           # 按 XY 弧长约每 40 cm 一个点，覆盖旧线首段 770 cm 的空档
SURFACE_OFFSET = 2.0
XY_TOLERANCE = 5.0       # 重铺后轨迹对原 XY 轨迹的最大允许偏差
MAX_GRADE = 0.30         # 异常路面跳层时拒绝超过 30% 的相邻坡度
SAMPLE_COUNT = 1000
WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "fit_inter31_branch_left.txt"


def xy_dist(a, b):
    return math.hypot(a.x - b.x, a.y - b.y)


def point_at(samples, distances, value):
    j = max(1, min(bisect.bisect_left(distances, value), len(samples) - 1))
    a, b = samples[j - 1], samples[j]
    span = distances[j] - distances[j - 1]
    t = (value - distances[j - 1]) / span if span > 1e-6 else 0.0
    return unreal.Vector(a.x + (b.x-a.x)*t,
                         a.y + (b.y-a.y)*t,
                         a.z + (b.z-a.z)*t)


def road_layers(world, p):
    """沿竖线查全部 SplineMesh 路面，跨过同一板的厚度。"""
    top, bottom = p.z + 60000.0, p.z - 60000.0
    layers = []
    for _ in range(40):
        hit = unreal.SystemLibrary.line_trace_single(
            world, unreal.Vector(p.x, p.y, top), unreal.Vector(p.x, p.y, bottom),
            unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, False, [],
            unreal.DrawDebugTrace.NONE, True)
        if hit is None:
            break
        d = hit.to_dict()
        if not d.get("blocking_hit"):
            break
        z = d["impact_point"].z
        component = d.get("hit_component")
        if component and "SplineMesh" in component.get_class().get_name():
            if not layers or layers[-1] - z > 60.0:
                layers.append(z + SURFACE_OFFSET)
            top = z - 30.0
        else:
            top = z - 1.0
        if top <= bottom:
            break
    return layers


def choose_heights(points, distances, all_layers):
    """两端固定；中段选连续的路面层，缺路面时在相邻实测点间桥接。"""
    n = len(points)
    bridge = [points[0].z + (points[-1].z-points[0].z)*d/distances[-1]
              for d in distances]
    candidates = []
    for i, layers in enumerate(all_layers):
        if i in (0, n-1):
            candidates.append([points[i].z])
        else:
            plausible = [z for z in layers if abs(z-bridge[i]) < 500.0]
            candidates.append(plausible or [None])

    # 动态规划避免在路口重叠的两层路面之间逐点跳来跳去。
    costs, parents = [], []
    for i, values in enumerate(candidates):
        row, prev_row = [], []
        for z in values:
            if z is None:
                unary = 2500.0
            else:
                unary = 0.04*(z-bridge[i])**2 + 0.02*(z-points[i].z)**2
            if i == 0:
                row.append(unary)
                prev_row.append(None)
                continue
            choices = []
            for k, previous in enumerate(candidates[i-1]):
                z0 = bridge[i-1] if previous is None else previous
                z1 = bridge[i] if z is None else z
                delta = abs(z1-z0)
                step = distances[i]-distances[i-1]
                excess = max(0.0, delta-MAX_GRADE*step)
                choices.append((costs[i-1][k] + 0.3*delta*delta + 100.0*excess*excess, k))
            best, parent = min(choices)
            row.append(unary+best)
            prev_row.append(parent)
        costs.append(row)
        parents.append(prev_row)
    index = min(range(len(costs[-1])), key=lambda k: costs[-1][k])
    chosen = [None]*n
    for i in range(n-1, -1, -1):
        chosen[i] = candidates[i][index]
        index = parents[i][index] if i else 0

    # 只有射线没有命中路面的点才插值；命中的点贴路面。
    known = [i for i, z in enumerate(chosen) if z is not None]
    for left, right in zip(known, known[1:]):
        if right-left <= 1:
            continue
        for i in range(left+1, right):
            t = (distances[i]-distances[left])/(distances[right]-distances[left])
            chosen[i] = chosen[left]*(1-t)+chosen[right]*t
    return chosen


def mark_before_edit(sp):
    # 组件属性变更会重跑蓝图构造；必须在拿点和写点之前先标记实例覆盖。
    sp.set_editor_property("spline_has_been_edited", True)


def write_spline(sp, points, tangents):
    sp.clear_spline_points(False)
    for p in points:
        sp.add_spline_point(p, WS, False)
    for i in range(len(points)):
        sp.set_spline_point_type(i, unreal.SplinePointType.CURVE, False)
    sp.update_spline()
    for i, tangent in ((0, tangents[0]), (len(points)-1, tangents[1])):
        adjacent = points[1] if i==0 else points[-2]
        spacing = xy_dist(points[i], adjacent)
        magnitude = math.sqrt(tangent.x*tangent.x+tangent.y*tangent.y+tangent.z*tangent.z)
        if magnitude<1e-4:
            raise RuntimeError("端点原始切线为零")
        scale = spacing/magnitude
        sp.set_tangent_at_spline_point(
            i, unreal.Vector(tangent.x*scale,tangent.y*scale,tangent.z*scale), WS, False)
    sp.update_spline()


def angle(a, b):
    dot = a.x*b.x+a.y*b.y+a.z*b.z
    la = math.sqrt(a.x*a.x+a.y*a.y+a.z*a.z)
    lb = math.sqrt(b.x*b.x+b.y*b.y+b.z*b.z)
    if la < 1e-4 or lb < 1e-4:
        raise RuntimeError("接缝切线长度为零")
    return math.degrees(math.acos(max(-1.0, min(1.0, dot/(la*lb)))))


def connection_report(all_actors, start, end, start_tangent, end_tangent):
    """核对实际 Lane 端点；端点由上游/下游连接确定，不凭路面射线移动。"""
    lanes = []
    for a in all_actors:
        label = a.get_actor_label()
        if not label.startswith("Lane_"):
            continue
        for s in a.get_components_by_class(unreal.SplineComponent):
            if s.get_name() not in ("Spline", "SplineComponent") or s.get_number_of_spline_points()<2:
                continue
            lanes.append((label,s))
    upstream = []
    downstream = []
    for label,s in lanes:
        first = s.get_location_at_spline_point(0,WS)
        last = s.get_location_at_spline_point(s.get_number_of_spline_points()-1,WS)
        if xy_dist(last,start)<600.0:
            direction = s.get_tangent_at_spline_point(s.get_number_of_spline_points()-1,WS)
            if angle(direction,start_tangent)<65.0:
                upstream.append((xy_dist(last,start),label,last,direction))
        if label==DOWNSTREAM:
            downstream.append((xy_dist(end,first),label,first,s.get_tangent_at_spline_point(0,WS)))
    if not upstream:
        raise RuntimeError("找不到同向且 600 cm 内的上游 Lane，拒绝重铺")
    if len(downstream)!=1 or downstream[0][0]>250.0:
        raise RuntimeError("指定下游 Lane 起点不在 250 cm 内，拒绝重铺")
    upstream.sort(key=lambda item:item[0])
    if angle(end_tangent,downstream[0][3])>65.0:
        raise RuntimeError("终点切线与下游车道方向不一致，拒绝重铺")
    return ["上游 %s: XY gap %.1f cm, Z gap %.1f cm" %
            (upstream[0][1],upstream[0][0],start.z-upstream[0][2].z),
            "下游 %s: XY gap %.1f cm, Z gap %.1f cm" %
            (downstream[0][1],downstream[0][0],downstream[0][2].z-end.z)]


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if world is None or world.get_name() != "TestForCharacter":
        raise RuntimeError("请先打开 TestForCharacter 关卡")
    all_actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    actors = [a for a in all_actors
              if a.get_actor_label() == ACTOR]
    if len(actors) != 1:
        raise RuntimeError("目标 Actor 数量不是 1: %d" % len(actors))
    components = [s for s in actors[0].get_components_by_class(unreal.SplineComponent)
                  if s.get_name() == COMPONENT]
    if len(components) != 1:
        raise RuntimeError("目标 SplineLeft 数量不是 1: %d" % len(components))
    sp = components[0]
    count = sp.get_number_of_spline_points()
    if count < 2:
        raise RuntimeError("样条点不足")
    old = [sp.get_location_at_spline_point(i, WS) for i in range(count)]
    end_tangents = [sp.get_tangent_at_spline_point(i, WS) for i in (0,count-1)]
    seams = connection_report(all_actors,old[0],old[-1],end_tangents[0],end_tangents[1])
    length = sp.get_spline_length()
    samples = [sp.get_location_at_distance_along_spline(length*i/SAMPLE_COUNT, WS)
               for i in range(SAMPLE_COUNT+1)]
    arc = [0.0]
    for a,b in zip(samples,samples[1:]):
        arc.append(arc[-1]+xy_dist(a,b))
    n = max(3, round(arc[-1]/SPACING)+1)
    distances = [arc[-1]*i/(n-1) for i in range(n)]
    points = [point_at(samples,arc,d) for d in distances]
    points[0], points[-1] = old[0], old[-1]
    layers = [road_layers(world,p) for p in points]
    road_hits = sum(bool(v) for v in layers)
    if road_hits < 0.8*n:
        raise RuntimeError("只有 %d/%d 个点测到路面；拒绝用猜测高度重铺" % (road_hits,n))
    heights = choose_heights(points,distances,layers)
    rebuilt = [unreal.Vector(p.x,p.y,z) for p,z in zip(points,heights)]
    report = ["%s.%s: %d -> %d points; XY length %.1f cm" %
              (ACTOR,COMPONENT,count,n,arc[-1])]
    report.extend(seams)
    report.append("路面命中 %d/%d" % (road_hits,n))
    for i,(p,z,roads) in enumerate(zip(points,heights,layers)):
        report.append("%02d old %.1f new %.1f road %s" %
                      (i,p.z,z,[round(v,1) for v in roads]))
    # 保存与恢复都需要原始每个点的切线，不只保存两端。
    old_tangents = [sp.get_tangent_at_spline_point(i, WS) for i in range(count)]
    original_types = [sp.get_spline_point_type(i) for i in range(count)]
    mark_before_edit(sp)
    # 构造脚本可能重建组件；重新取同名组件再写。
    sp = next(s for s in actors[0].get_components_by_class(unreal.SplineComponent)
              if s.get_name() == COMPONENT)
    if sp.get_number_of_spline_points()!=count or any(
            xy_dist(sp.get_location_at_spline_point(i,WS),p)>0.05 or
            abs(sp.get_location_at_spline_point(i,WS).z-p.z)>0.05
            for i,p in enumerate(old)):
        raise RuntimeError("标记实例编辑后，蓝图构造重置了样条；未写入，请先确认实例覆盖")
    sp.modify(True)
    actors[0].modify(True)
    try:
        write_spline(sp,rebuilt,end_tangents)
        for i,old_point,old_tangent in ((0,old[0],end_tangents[0]),
                                        (n-1,old[-1],end_tangents[1])):
            p = sp.get_location_at_spline_point(i,WS)
            t = sp.get_tangent_at_spline_point(i,WS)
            if xy_dist(p,old_point)>0.05 or abs(p.z-old_point.z)>0.05 or angle(t,old_tangent)>0.2:
                raise RuntimeError("起点或终点的高度、位置、方向发生变化")
        new_length = sp.get_spline_length()
        new_samples = [sp.get_location_at_distance_along_spline(new_length*i/SAMPLE_COUNT,WS)
                       for i in range(SAMPLE_COUNT+1)]
        # 与原线相同的 XY 走廊；均匀采样允许少量重建误差。
        error = max(max(min(xy_dist(p,q) for q in new_samples) for p in samples),
                    max(min(xy_dist(p,q) for q in samples) for p in new_samples))
        report.append("XY max deviation %.2f cm" % error)
        if error>XY_TOLERANCE:
            raise RuntimeError("重建 XY 偏离原线路 %.2f cm" % error)
        report.append("完成；请在编辑器确认车辆运行后保存关卡。")
    except Exception:
        write_spline(sp,old,end_tangents)
        for i,(t,kind) in enumerate(zip(old_tangents,original_types)):
            sp.set_spline_point_type(i,kind,False)
            sp.set_tangent_at_spline_point(i,t,WS,False)
        sp.update_spline()
        raise
    finally:
        with open(OUT,"w",encoding="utf-8") as fh:
            fh.write("\n".join(report))
    unreal.log("[Inter31Fit] 重铺完成；报告："+OUT)


try:
    run()
except Exception:
    unreal.log_error("[Inter31Fit] "+traceback.format_exc())
