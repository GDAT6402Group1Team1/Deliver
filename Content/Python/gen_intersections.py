# -*- coding: utf-8 -*-
"""在每个路口放一个 BP_Intersection，**必须**罩住路口段的 Box，尽量避开路段的 Box。

两条约束的优先级是有高下的：
    硬约束：罩住本路口全部 Inter_* 的盒体。罩不住这个盒子就没意义。
    软约束：不碰 Lane_* 的盒体。做得到就做，做不到就报出来。
早先版本反了——先收缩到不碰任何 Lane_ 为止，收过头才判无解，
于是"罩不住"成了常态（上一轮 10 个路口里有报 !! 没罩住 的）。

四条边独立伸缩，不是对称缩放。对称盒子为了躲开左边一个 Lane_ Box，
右边得跟着一起缩，这是罩不住的主要来源。独立伸缩只动挨着的那条边，
再把中心挪到新的中点、extent 取半跨度，对称盒子照样表达得出来。

判定用真实盒体，不是 actor 原点。Box 放大到 2 倍 (128x128x600) 之后
点判定的误差不能忽略了：原点在盒内但盒体探出去、原点在盒外但盒体已经
压上车道 Box，两种错都会出现。车道 Box 随行驶方向转过、不是轴对齐，
用 |前|*hx + |右|*hy + |上|*hz 逐轴外接成 AABB（保守偏大）。

不能用"到中心的球面距离"当判据：四车道有 +-675 的横向偏移，
径向会把沿路方向的间距压缩掉，两类 Box 就分不开了
（实测 10 个路口里 7 个因此判成无解）。

依赖：先跑 gen_traffic_lanes.py。
RESTORE=True 只删除，不新建。
用法：py gen_intersections.py
"""

import traceback

import unreal

RESTORE = False
LANE_TAG_PREFIX = "ClaudeGenLane"
INTER_BP = "/Game/PS2DEM/BP_Intersection"
TAG = "ClaudeGenIntersection"

CLUSTER_DIST = 2500.0    # 距离小于此值的路口段归为同一个物理路口
COVER_MARGIN = 40.0      # 罩住路口段盒体之后再往外放这么多
MIN_CLEAR = 40.0         # 盒面与路段 Box 盒面之间希望留的距离（软约束）
Z_HALF = 400.0           # 盒子垂直半高

OUT = unreal.Paths.project_saved_dir() + "gen_intersections.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[gen_inter] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def box_aabb(a):
    """这个 actor 上 Box 的世界 AABB: (中心, 逐轴半长)。没有 Box 返回 None。"""
    for c in a.get_components_by_class(unreal.BoxComponent):
        e = c.get_unscaled_box_extent()
        sc = a.get_actor_scale3d()
        hx, hy, hz = e.x * sc.x, e.y * sc.y, e.z * sc.z
        try:
            center = c.get_world_location()
            rot = c.get_world_rotation()
        except Exception:
            center = a.get_actor_location()
            rot = a.get_actor_rotation()
        f = unreal.MathLibrary.get_forward_vector(rot)
        r = unreal.MathLibrary.get_right_vector(rot)
        u = unreal.MathLibrary.get_up_vector(rot)
        half = unreal.Vector(
            abs(f.x) * hx + abs(r.x) * hy + abs(u.x) * hz,
            abs(f.y) * hx + abs(r.y) * hy + abs(u.y) * hz,
            abs(f.z) * hx + abs(r.z) * hy + abs(u.z) * hz)
        return center, half
    return None


def solve_box(inter_boxes, lane_boxes, cz):
    """四条边独立伸缩，罩住全部 inter_boxes，尽量避开 lane_boxes。

    返回 (bx0, bx1, by0, by1, 收缩次数, 仍然压着的车道Box列表)。
    硬下限 = 罩住所有 inter 盒体所需的范围，任何收缩都不许越过它。
    """
    fx0 = min(p.x - h.x for p, h, _l in inter_boxes)
    fx1 = max(p.x + h.x for p, h, _l in inter_boxes)
    fy0 = min(p.y - h.y for p, h, _l in inter_boxes)
    fy1 = max(p.y + h.y for p, h, _l in inter_boxes)

    bx0, bx1 = fx0 - COVER_MARGIN, fx1 + COVER_MARGIN
    by0, by1 = fy0 - COVER_MARGIN, fy1 + COVER_MARGIN

    shrunk = 0
    for _ in range(120):
        hit = [(p, h, l) for p, h, l in lane_boxes
               if p.x + h.x > bx0 and p.x - h.x < bx1
               and p.y + h.y > by0 and p.y - h.y < by1
               and abs(p.z - cz) < Z_HALF + h.z]
        if not hit:
            return bx0, bx1, by0, by1, shrunk, []

        # 对每个压着的车道 Box，看四条边里哪条能把它挤出去、且不越过硬下限。
        # 选"损失面积最小"的那一步走。
        best = None
        for p, h, l in hit:
            cands = [
                ("x1", bx0, p.x - h.x - MIN_CLEAR, by0, by1),   # 右边往左收
                ("x0", p.x + h.x + MIN_CLEAR, bx1, by0, by1),   # 左边往右收
                ("y1", bx0, bx1, by0, p.y - h.y - MIN_CLEAR),   # 上边往下收
                ("y0", bx0, bx1, p.y + h.y + MIN_CLEAR, by1),   # 下边往上收
            ]
            for tag, nx0, nx1, ny0, ny1 in cands:
                if nx0 > fx0 or nx1 < fx1 or ny0 > fy0 or ny1 < fy1:
                    continue            # 越过硬下限，会罩不住路口段
                if nx1 <= nx0 or ny1 <= ny0:
                    continue
                loss = ((bx1 - bx0) * (by1 - by0)) - ((nx1 - nx0) * (ny1 - ny0))
                if loss <= 0:
                    continue
                if best is None or loss < best[0]:
                    best = (loss, nx0, nx1, ny0, ny1)
        if best is None:
            # 没有任何一条边能在不破坏"罩住"的前提下挤开它们。
            # 硬约束优先：保住覆盖，把还压着的报出来。
            return bx0, bx1, by0, by1, shrunk, hit
        _loss, bx0, bx1, by0, by1 = best
        shrunk += 1

    return bx0, bx1, by0, by1, shrunk, []


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    removed = 0
    for a in actors:
        if TAG in [str(t) for t in a.tags]:
            eas.destroy_actor(a)
            removed += 1
    w("清除旧路口盒子 %d 个" % removed)
    if RESTORE:
        w("RESTORE=True，只删除不新建。关卡尚未保存。")
        flush()
        return

    inter_boxes, lane_boxes = [], []
    for a in actors:
        if not any(str(t).startswith(LANE_TAG_PREFIX) for t in a.tags):
            continue
        lbl = a.get_actor_label()
        bb = box_aabb(a)
        if bb is None:
            continue
        if lbl.startswith("Inter_"):
            inter_boxes.append((bb[0], bb[1], lbl))
        elif lbl.startswith("Lane_"):
            lane_boxes.append((bb[0], bb[1], lbl))
    w("路口段 Box %d 个，路段 Box %d 个" % (len(inter_boxes), len(lane_boxes)))
    if not inter_boxes:
        w("!! 没有 Inter_* actor，请先跑 gen_traffic_lanes.py")
        flush()
        return

    sh = inter_boxes[0][1]
    w("路口段盒体半尺寸样例 (%.0f, %.0f, %.0f)  <- 含旋转外接，比原始 extent 大是正常的"
      % (sh.x, sh.y, sh.z))

    clusters = []
    for pos, half, lbl in inter_boxes:
        got = None
        for c in clusters:
            if (c["center"] - pos).length() < CLUSTER_DIST:
                got = c
                break
        if got is None:
            clusters.append({"center": pos, "boxes": [(pos, half, lbl)]})
        else:
            got["boxes"].append((pos, half, lbl))
            n = len(got["boxes"])
            got["center"] = unreal.Vector(
                sum(p.x for p, _h, _l in got["boxes"]) / n,
                sum(p.y for p, _h, _l in got["boxes"]) / n,
                sum(p.z for p, _h, _l in got["boxes"]) / n)
    w("聚类出 %d 个物理路口" % len(clusters))
    w("")
    flush()

    cls = unreal.EditorAssetLibrary.load_asset(INTER_BP).generated_class()
    probe = eas.spawn_actor_from_class(cls, unreal.Vector(0, 0, -300000))
    base = None
    for c in probe.get_components_by_class(unreal.BoxComponent):
        base = c.get_unscaled_box_extent()
        break
    eas.destroy_actor(probe)
    if base is None:
        w("!! 读不到 BP_Intersection 的 Box")
        flush()
        return
    w("BP_Intersection 默认 Box 半尺寸 (%.1f, %.1f, %.1f)，靠 actor 缩放放大"
      % (base.x, base.y, base.z))
    w("")

    made = 0
    dirty = []
    for i, cl in enumerate(clusters):
        boxes = cl["boxes"]
        cz = sum(p.z for p, _h, _l in boxes) / len(boxes)
        bx0, bx1, by0, by1, shrunk, still = solve_box(boxes, lane_boxes, cz)

        cx, cy = (bx0 + bx1) / 2.0, (by0 + by1) / 2.0
        ex, ey = (bx1 - bx0) / 2.0, (by1 - by0) / 2.0

        # 硬约束自检：真的罩住了吗
        missed = [l for p, h, l in boxes
                  if abs(p.x - cx) + h.x > ex + 0.5
                  or abs(p.y - cy) + h.y > ey + 0.5]

        a = eas.spawn_actor_from_class(cls, unreal.Vector(cx, cy, cz))
        a.set_actor_label("Intersection_%02d" % i)
        a.tags = [TAG]
        a.set_actor_scale3d(unreal.Vector(ex / base.x, ey / base.y, Z_HALF / base.z))
        try:
            a.set_folder_path("RoadIntersections")
        except Exception:
            pass
        made += 1

        note = ""
        if shrunk:
            note += "  避让收缩 %d 次" % shrunk
        if still:
            note += "  !! 仍压着 %d 个车道Box" % len(still)
            dirty.append((i, [l for _p, _h, l in still]))
        if missed:
            note += "  !!! 没罩住 %d 个路口段（不该发生）" % len(missed)
        w("[%02d] (%.0f, %.0f)  半尺寸 %.0f x %.0f  路口段 %d 个%s"
          % (i, cx, cy, ex, ey, len(boxes), note))

    w("")
    w("共放置 %d 个 BP_Intersection" % made)
    if dirty:
        w("")
        w("以下路口为了罩住路口段，没能完全避开车道 Box：")
        for i, labels in dirty:
            w("  [%02d] %s" % (i, ", ".join(l[:30] for l in labels[:6])))
        w("  （硬约束是罩住路口段，这里是主动取舍。要改善就把间隙 TARGET_GAP 调大，")
        w("    让车道 Box 离路口远一点，再重跑 gen_traffic_lanes.py + 本脚本。）")
    else:
        w("全部路口都既罩住了路口段、又避开了所有车道 Box。")
    w("")
    w("关卡尚未保存。")
    flush()


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[gen_inter] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
