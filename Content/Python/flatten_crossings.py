# -*- coding: utf-8 -*-
"""路口重叠处只保留主路的路面，把交叉路盖上来的那几片藏掉。

问题：路面是 Landscape 上 1982 个 SplineMeshComponent。两条路相交处，
两边的路面片互相叠着而且高度不一样。lane 取高度的射线只认"最上面那层路面"，
于是路口内的点爬到交叉路的面上、路口外的点还在主路的面上，
相邻两段车道就断开了（实测 Inter_形状13_-0225_I03 与 Lane_形状13_-0225_S04）。
注意这和上一个已修的问题不是一回事：那个是射线**打空**回退到源样条 Z，
这个是射线**打中了**，但打中的是另一条路。

做法不是删，是隐藏 + 关碰撞：
  * 看不见   -> 视觉上路口只剩主路，平整
  * 无碰撞   -> surface_hit 打不到它，车道高度自然只跟主路
  * 可逆     -> RESTORE=True 全部恢复；删掉的可回不来
地形样条系统重新生成时，隐藏可能被重置（和加厚一样），这点没验证过。

归属判定靠**方向**不靠距离：重叠处两条路的片都离得很近，
但主路的片顺着主路走、交叉路的片顺着交叉路走，方向差得很开。

DRY_RUN=True（默认）只报告不动手。
用法：py flatten_crossings.py
"""

import traceback

import unreal

KEEP_ROAD = "形状 13"     # 保留这条路的路面
CROSS_ROADS = []          # 空 = 所有与 KEEP_ROAD 相交的路；也可以只填 ["形状 44"]
DRY_RUN = True            # True 只报告；确认无误后改 False 再跑
RESTORE = False           # True = 把之前藏起来的全部恢复，其它都不做

REGION_MARGIN = 150.0     # 重叠区半径 = 主路半宽 + 这个余量
MAIN_HALF_WIDTH = 900.0   # 主路半宽（1800/2）
DIR_MARGIN = 0.15         # 方向判定的最小区分度，太接近就判"说不清"，不动它

XY_CROSS = 200.0
Z_CROSS = 600.0
DETECT_STEP = 250.0
MERGE_DIST = 2500.0

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "flatten_crossings.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[flatten] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def piece_geom(c):
    """路面片的世界端点和方向。spline_params 里的端点是局部的，要过一遍变换。"""
    try:
        p = c.get_editor_property("spline_params")
        sp = p.get_editor_property("start_pos")
        ep = p.get_editor_property("end_pos")
    except Exception:
        return None
    try:
        t = c.get_world_transform()
        a = unreal.MathLibrary.transform_location(t, sp)
        b = unreal.MathLibrary.transform_location(t, ep)
    except Exception:
        return None
    d = b - a
    h = (d.x * d.x + d.y * d.y) ** 0.5
    if h < 1e-3:
        return None
    mid = unreal.Vector((a.x + b.x) / 2.0, (a.y + b.y) / 2.0, (a.z + b.z) / 2.0)
    return mid, (d.x / h, d.y / h)


def tangent_at(sp, p):
    """样条上离 p 最近处的水平切线方向，以及那一点的距离。"""
    key = sp.find_input_key_closest_to_world_location(p)
    q = sp.get_location_at_spline_input_key(key, WS)
    t = sp.get_tangent_at_spline_input_key(key, WS)
    h = (t.x * t.x + t.y * t.y) ** 0.5
    if h < 1e-3:
        return None, None, None
    dxy = ((q.x - p.x) ** 2 + (q.y - p.y) ** 2) ** 0.5
    return (t.x / h, t.y / h), dxy, q.z


def find_intersections(tsp, roads, total):
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


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    land = None
    for a in actors:
        if a.get_class().get_name() == "Landscape":
            land = a
            break
    if land is None:
        w("!! 找不到 Landscape")
        flush()
        return
    sms = list(land.get_components_by_class(unreal.SplineMeshComponent))
    w("Landscape 上路面片 %d 个" % len(sms))

    if RESTORE:
        n = 0
        for c in sms:
            try:
                if not c.is_visible():
                    c.set_visibility(True, False)
                    c.set_collision_enabled(
                        unreal.CollisionEnabled.QUERY_AND_PHYSICS)
                    n += 1
            except Exception:
                pass
        w("恢复了 %d 个隐藏的路面片。关卡尚未保存。" % n)
        flush()
        return

    splines = {}
    for a in actors:
        if a.get_class().get_name() == "BP_PS2DEMSplineActor_v3_C":
            sp = a.get_component_by_class(unreal.SplineComponent)
            if sp:
                splines[a.get_actor_label().strip()] = sp
    keep = splines.get(KEEP_ROAD.strip())
    if keep is None:
        w("!! 找不到 %s。关卡里的路：%s" % (KEEP_ROAD, ", ".join(sorted(splines))))
        flush()
        return

    others = [(n, s) for n, s in splines.items() if n != KEEP_ROAD.strip()]
    inters = find_intersections(keep, others, keep.get_spline_length())
    w("%s 上检测到路口 %d 处" % (KEEP_ROAD, len(inters)))

    jobs = []
    for idx, (dist, name) in enumerate(inters):
        names = [x.strip() for x in str(name).split("+")]
        if CROSS_ROADS:
            names = [x for x in names if x in [c.strip() for c in CROSS_ROADS]]
        if not names:
            continue
        center = keep.get_location_at_distance_along_spline(dist, WS)
        jobs.append((idx, center, names))
    w("本次处理 %d 处（CROSS_ROADS=%s）"
      % (len(jobs), CROSS_ROADS if CROSS_ROADS else "全部"))
    w("")
    flush()

    radius = MAIN_HALF_WIDTH + REGION_MARGIN
    total_hide = []
    for idx, center, names in jobs:
        w("=" * 84)
        w("路口 I%02d  @ (%.0f, %.0f, %.0f)   交叉路 %s   半径 %.0f"
          % (idx, center.x, center.y, center.z, "、".join(names), radius))
        w("=" * 84)

        cross_sps = [(n, splines[n]) for n in names if n in splines]
        if not cross_sps:
            w("  这些交叉路在关卡里找不到样条，跳过")
            continue

        mine = theirs = unclear = 0
        z_keep, z_cross = [], []
        for c in sms:
            g = piece_geom(c)
            if g is None:
                continue
            mid, (dx, dy) = g
            if ((mid.x - center.x) ** 2 + (mid.y - center.y) ** 2) ** 0.5 > radius:
                continue

            kt, _kd, _kz = tangent_at(keep, mid)
            if kt is None:
                continue
            score_keep = abs(kt[0] * dx + kt[1] * dy)

            best_cross, best_name = -1.0, None
            for n, s in cross_sps:
                ct, _cd, _cz = tangent_at(s, mid)
                if ct is None:
                    continue
                sc = abs(ct[0] * dx + ct[1] * dy)
                if sc > best_cross:
                    best_cross, best_name = sc, n

            if best_cross - score_keep > DIR_MARGIN:
                theirs += 1
                z_cross.append(mid.z)
                total_hide.append((c, idx, best_name, mid.z))
            elif score_keep - best_cross > DIR_MARGIN:
                mine += 1
                z_keep.append(mid.z)
            else:
                unclear += 1

        w("  区域内路面片：属于 %s %d 片，属于交叉路 %d 片，方向说不清 %d 片（不动）"
          % (KEEP_ROAD, mine, theirs, unclear))
        if z_keep and z_cross:
            ak = sum(z_keep) / len(z_keep)
            ac = sum(z_cross) / len(z_cross)
            w("  平均高度：%s %.0f    交叉路 %.0f    差 %+.0f cm"
              % (KEEP_ROAD, ak, ac, ac - ak))
            if ac > ak:
                w("  >>> 交叉路的面在上面，射线会先打到它 —— 这正是断开的原因")
            else:
                w("  >>> 交叉路的面在下面。那这个路口的断开**不是**这个原因造成的，")
                w("      藏掉它对追踪没帮助（对视觉平整仍有帮助）。")
        w("")
        flush()

    w("=" * 84)
    w("合计")
    w("=" * 84)
    w("  准备处理的交叉路路面片 %d 个" % len(total_hide))
    by_road = {}
    for _c, _i, nm, _z in total_hide:
        by_road[nm] = by_road.get(nm, 0) + 1
    for nm in sorted(by_road, key=lambda x: -by_road[x]):
        w("     %-14s %d 片" % (nm, by_road[nm]))

    if DRY_RUN:
        w("")
        w("DRY_RUN=True，什么都没改。")
        w("确认上面的分类和高度差合理之后，把 DRY_RUN 改成 False 再跑一次。")
        w("改完想反悔：把 RESTORE 改成 True 跑一次，全部恢复可见。")
        flush()
        return

    done = fail = 0
    for c, _i, _nm, _z in total_hide:
        try:
            c.set_visibility(False, False)
            c.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
            done += 1
        except Exception as exc:
            fail += 1
            if fail <= 3:
                w("  失败 %s: %s" % (c.get_name(), str(exc)[:70]))
    w("")
    w("已隐藏并关闭碰撞 %d 个，失败 %d 个。" % (done, fail))
    w("接下来重跑 gen_traffic_lanes.py，车道高度就只会跟 %s 的路面了。" % KEEP_ROAD)
    w("想反悔：RESTORE=True 跑一次。关卡尚未保存。")
    flush()


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[flatten] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
