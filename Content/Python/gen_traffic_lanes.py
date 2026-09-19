# -*- coding: utf-8 -*-
"""沿道路样条生成交通车道，路口处断开并换成 IntersectionChild。

结构：
    路段(BP_TrafficLine1) --空隙-- 路口(IntersectionChild) --空隙-- 路段 ...

实测依据：
  * 路宽 1000cm（Cube 底模 100 × 样条点 Y 缩放 10，与路面块 scale 10.01 互证）
  * 双向四车道 -> 单车道 250cm -> 偏移 +-125 / +-375；靠左行驶
  * BP_TrafficLine1 与 IntersectionChild 结构完全相同（Spline + 固定 Box），
    Box 只是"起点探测把手"，所以一段路用一条样条即可
  * 不能信任源样条的 Z：形状13 与真实路面偏差 -546 ~ +411 cm，
    真正的路面是 Landscape 上的地形样条网格，必须逐点打射线重测
  * UE5.8 的 HitResult 属性是 protected，取值走 to_dict()

可重复运行：tag 按路区分（"ClaudeGenLane:<路名>"），每次只清掉 ROAD_LABELS
里那几条路的旧 actor，不会误删其它路已经生成好的车道。
"""

import traceback

import unreal

ROAD_LABELS = ["形状 13", "形状 39"]   # 要处理的道路，可以只留一条
LANE_BP = "/Game/PS2DEM/BP_TrafficLine1"
CHILD_BP = "/Game/PS2DEM/BP_TrafficLine1_IntersectionChild"
TAG_PREFIX = "ClaudeGenLane"   # 实际 tag = "ClaudeGenLane:<路名>"，按路独立，互不误删

ROAD_WIDTH = 1000.0
LANE_COUNT = 4
SAMPLE_STEP = 500.0
Z_OFFSET = 15.0
LEFT_HAND_TRAFFIC = True

# --- 路口切断参数 ---
BREAK_HALF = 900.0       # 路段在路口中心前后这么远处截断
INTER_HALF = 600.0       # IntersectionChild 覆盖中心 +-这么远（差值 300 就是空隙）
DETECT_STEP = 250.0
XY_CROSS = 200.0         # 最近 XY 小于此值才算真的相交（实测真路口都 <120）
Z_CROSS = 600.0          # Z 差大于此值是立交，不断开
MERGE_DIST = 2500.0      # 中心相距小于此值的路口合并成一个

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "gen_lanes_report.txt"

lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[gen_lanes] %s" % s)


def surface_hit(world, x, y, z_hint):
    """往下打射线取真实路面高度，返回 (路面Z, 命中对象名)。"""
    start = unreal.Vector(x, y, z_hint + 60000.0)
    end = unreal.Vector(x, y, z_hint - 60000.0)
    try:
        hit = unreal.SystemLibrary.line_trace_single(
            world, start, end, unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
            False, [], unreal.DrawDebugTrace.NONE, True)
        if hit is None:
            return None, "no-return"
        d = hit.to_dict()
        if not d.get("blocking_hit"):
            return None, "no-hit"
        who = "?"
        a = d.get("hit_actor")
        if a is not None:
            try:
                who = a.get_actor_label()
            except Exception:
                who = a.get_name()
        return d["impact_point"].z, who
    except Exception as exc:
        return None, "err:%s" % str(exc)[:40]


def find_intersections(tsp, roads, total):
    """返回 [(路口中心沿线距离, 对向道路名)]，已排除立交、已合并相近的。"""
    raw = []
    d = 0.0
    while d <= total:
        p = tsp.get_location_at_distance_along_spline(d, WS)
        for name, osp in roads:
            q = osp.find_location_closest_to_world_location(p, WS)
            dxy = ((q.x - p.x) ** 2 + (q.y - p.y) ** 2) ** 0.5
            if dxy < XY_CROSS and abs(q.z - p.z) < Z_CROSS:
                raw.append((d, name, dxy))
        d += DETECT_STEP

    groups = []
    for dist, name, dxy in raw:
        hit = None
        for g in groups:
            if g["road"] == name and dist - g["last"] <= DETECT_STEP * 3:
                hit = g
                break
        if hit is None:
            groups.append({"road": name, "last": dist, "best": (dist, dxy)})
        else:
            hit["last"] = dist
            if dxy < hit["best"][1]:
                hit["best"] = (dist, dxy)

    centers = sorted((g["best"][0], g["road"]) for g in groups)

    merged = []
    for c, name in centers:
        if merged and c - merged[-1][0] < MERGE_DIST:
            pc, pn = merged[-1]
            merged[-1] = ((pc + c) / 2.0, pn + "+" + name)
        else:
            merged.append((c, name))
    return merged


def lane_point(world, tsp, offset, dist):
    """某个沿线距离处、该车道的点。横向用右向量水平分量，高度打射线重测。"""
    loc = tsp.get_location_at_distance_along_spline(dist, WS)
    right = tsp.get_right_vector_at_distance_along_spline(dist, WS)
    hx, hy = right.x, right.y
    h = (hx * hx + hy * hy) ** 0.5
    if h > 1e-4:
        hx, hy = hx / h, hy / h
    px, py = loc.x + hx * offset, loc.y + hy * offset
    pz, who = surface_hit(world, px, py, loc.z)
    if pz is None:
        return unreal.Vector(px, py, loc.z + right.z * offset), False, who
    return unreal.Vector(px, py, pz + Z_OFFSET), True, who


def build_points(world, tsp, offset, d_from, d_to):
    pts, missed, kinds = [], 0, {}
    d = d_from
    while d < d_to:
        p, ok, who = lane_point(world, tsp, offset, d)
        pts.append(p)
        if ok:
            kinds[who] = kinds.get(who, 0) + 1
        else:
            missed += 1
        d += SAMPLE_STEP
    p, ok, who = lane_point(world, tsp, offset, d_to)
    pts.append(p)
    if ok:
        kinds[who] = kinds.get(who, 0) + 1
    else:
        missed += 1
    return pts, missed, kinds


def mark_edited(sp):
    """给样条打"已被编辑"标记，否则构造脚本一重跑就把点冲回蓝图默认的 2 个点。

    蓝图定义的组件，实例上的改动默认不保留；UE 靠 bSplineHasBeenEdited
    （编辑器里显示为 "Override Construction Script"）决定要不要在
    构造脚本之后把实例的样条数据重新应用回去。手动拖点时 UE 自动打这个标记，
    从脚本写点必须自己设。
    """
    for name in ("spline_has_been_edited", "b_spline_has_been_edited"):
        try:
            sp.set_editor_property(name, True)
            return True
        except Exception:
            continue
    return False


def spawn_lane(eas, cls, pts, label, forward, tag):
    if len(pts) < 2:
        return None
    if not forward:
        pts = list(reversed(pts))
    a = eas.spawn_actor_from_class(cls, pts[0])
    a.set_actor_label(label)
    a.tags = [tag]
    sp = a.get_component_by_class(unreal.SplineComponent)
    sp.clear_spline_points(False)
    for p in pts:
        sp.add_spline_point(p, WS, False)
    for i in range(sp.get_number_of_spline_points()):
        sp.set_spline_point_type(i, unreal.SplinePointType.CURVE_CLAMPED, False)
    sp.update_spline()
    if not mark_edited(sp):
        w("!! %s 的 spline_has_been_edited 设置失败，双击后点会被构造脚本冲掉" % label)
    return a


def process_road(eas, world, all_splines, road_label, tag):
    """给一条路生成全部车道（路段 + 路口段）。返回统计字典。"""
    target = None
    others = []
    for name, sp in all_splines:
        if name.strip() == road_label:
            target = sp
        else:
            others.append((name, sp))
    if target is None:
        w("!! 找不到 %s，跳过" % road_label)
        return None

    tsp = target
    total = tsp.get_spline_length()
    w("")
    w("=" * 60)
    w("主路 %s   总长 %.0f cm   对照道路 %d 条" % (road_label, total, len(others)))
    w("=" * 60)
    roads = others

    inters = find_intersections(tsp, roads, total)
    w("检测到路口 %d 处（XY<%.0f 且 Z差<%.0f，相距<%.0f 的已合并）"
      % (len(inters), XY_CROSS, Z_CROSS, MERGE_DIST))
    for c, name in inters:
        w("   沿线 %8.0f   %s" % (c, name))

    segs = []
    cur = 0.0
    for c, _ in inters:
        a0, a1 = c - BREAK_HALF, c + BREAK_HALF
        if a0 - cur > SAMPLE_STEP * 2:
            segs.append((cur, a0))
        cur = max(cur, a1)
    if total - cur > SAMPLE_STEP * 2:
        segs.append((cur, total))
    w("切出路段 %d 段，路口段 %d 处" % (len(segs), len(inters)))
    w("")

    lane_w = ROAD_WIDTH / LANE_COUNT
    offsets = [(i - (LANE_COUNT - 1) / 2.0) * lane_w for i in range(LANE_COUNT)]
    lane_cls = unreal.EditorAssetLibrary.load_asset(LANE_BP).generated_class()
    child_cls = unreal.EditorAssetLibrary.load_asset(CHILD_BP).generated_class()
    w("通行 %s   单车道宽 %.0f   偏移 %s"
      % ("靠左" if LEFT_HAND_TRAFFIC else "靠右", lane_w,
         ["%.0f" % o for o in offsets]))
    w("")

    rid = road_label.replace(" ", "")
    total_missed = 0
    all_kinds = {}
    n_seg = n_int = 0

    for offset in offsets:
        forward = (offset < 0) if LEFT_HAND_TRAFFIC else (offset > 0)
        otag = "%+05d" % int(offset)

        for i, (d0, d1) in enumerate(segs):
            pts, miss, kinds = build_points(world, tsp, offset, d0, d1)
            total_missed += miss
            for k, v in kinds.items():
                all_kinds[k] = all_kinds.get(k, 0) + v
            if spawn_lane(eas, lane_cls, pts,
                          "Lane_%s_%s_S%02d" % (rid, otag, i), forward, tag):
                n_seg += 1

        for i, (c, _) in enumerate(inters):
            pts, miss, kinds = build_points(world, tsp, offset,
                                            c - INTER_HALF, c + INTER_HALF)
            total_missed += miss
            for k, v in kinds.items():
                all_kinds[k] = all_kinds.get(k, 0) + v
            if spawn_lane(eas, child_cls, pts,
                          "Inter_%s_%s_I%02d" % (rid, otag, i), forward, tag):
                n_int += 1

        w("  偏移 %+6.0f  %s  路段 %d + 路口 %d"
          % (offset, "顺行" if forward else "逆行", len(segs), len(inters)))

    w("小计: 路段 %d + 路口段 %d = %d 个 actor，射线打空 %d 点"
      % (n_seg, n_int, n_seg + n_int, total_missed))
    return {"road": road_label, "seg": n_seg, "int": n_int,
            "missed": total_missed, "kinds": all_kinds,
            "inters": len(inters), "segs": len(segs)}


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    actors = eas.get_all_level_actors()

    # 只清掉本次要处理的那几条路的旧 actor（外加早期版本的裸 tag），别误删别的路
    targets = set("%s:%s" % (TAG_PREFIX, r) for r in ROAD_LABELS)
    targets.add(TAG_PREFIX)
    removed = 0
    for a in actors:
        if targets & set(str(t) for t in a.tags):
            eas.destroy_actor(a)
            removed += 1
    w("清除旧 actor %d 个（仅限 %s）" % (removed, "、".join(ROAD_LABELS)))

    # 道路样条一次性收集好，各条路互为对照
    all_splines = []
    for a in actors:
        if a.get_class().get_name() == "BP_PS2DEMSplineActor_v3_C":
            sp = a.get_component_by_class(unreal.SplineComponent)
            if sp:
                all_splines.append((a.get_actor_label(), sp))
    w("关卡道路样条共 %d 条" % len(all_splines))

    stats = []
    for road_label in ROAD_LABELS:
        st = process_road(eas, world, all_splines, road_label,
                          "%s:%s" % (TAG_PREFIX, road_label))
        if st:
            stats.append(st)

    w("")
    w("=" * 60)
    w("总计")
    w("=" * 60)
    grand = {}
    tot_seg = tot_int = tot_miss = 0
    for st in stats:
        w("  %-10s 路口 %d 处，路段 %d 段 -> actor %d 个，打空 %d 点"
          % (st["road"], st["inters"], st["segs"], st["seg"] + st["int"], st["missed"]))
        tot_seg += st["seg"]
        tot_int += st["int"]
        tot_miss += st["missed"]
        for k, v in st["kinds"].items():
            grand[k] = grand.get(k, 0) + v
    w("  合计 路段 %d + 路口段 %d = %d 个 actor，打空 %d 点"
      % (tot_seg, tot_int, tot_seg + tot_int, tot_miss))
    w("  命中对象分布（出现建筑名 = 有点落在屋顶）:")
    for k in sorted(grand, key=lambda x: -grand[x]):
        w("     %-32s %d" % (k, grand[k]))
    w("")
    w("空隙 = BREAK_HALF(%.0f) - INTER_HALF(%.0f) = %.0f cm（路口两侧各留）"
      % (BREAK_HALF, INTER_HALF, BREAK_HALF - INTER_HALF))
    w("完成。关卡尚未保存。")

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[gen_lanes] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
