# -*- coding: utf-8 -*-
"""按真实车道几何模拟 TraceForIntersection，统计它会不会认错路口段。

上一个审计脚本的扫描几何是我编的（从路口段正后方 2500cm 扫过去），
只能说明"同通道下可能返回别的路的盒子"，不能估真实发生率。
这里改成沿**真实车道样条**逐点出发、沿**车道切线**往前扫，
和车实际在做的事一致。

每个采样点扫一次，看第一个命中的 Inter_* 是不是"自己这条路、自己这个方向"：
    正确      —— 同路名、同偏移（就是自己这条车道的路口段）
    串路      —— 别的路名（横向车流，灯态相反，最危险）
    串车道    —— 同路名但不同偏移（隔壁平行车道）
    逆向      —— 朝向相反（dot < 0），对向车道
探测半径和长度游戏里是多少还不知道（要看蓝图），所以扫一组值，
看结论对参数有多敏感。

只读，什么都不改。
用法：py sim_trace.py
"""

import traceback

import unreal

TAG_PREFIX = "ClaudeGenLane"
STATION_STEP = 400.0          # 沿车道每隔多远采一个点
LOOKAHEADS = [1000.0, 2000.0, 3000.0]
RADII = [50.0, 100.0, 150.0]
DOT_SAME_DIR = 0.7            # 判"同向"的阈值
MAX_LANES = 0                 # 0 = 全部车道

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "sim_trace.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def parse(label):
    """Lane_形状13_-0675_S00 -> ('形状13', '-0675')；认不出返回 (label, '')。"""
    parts = str(label).split("_")
    if len(parts) >= 3:
        return parts[1], parts[2]
    return str(label), ""


def road_object_type():
    members = [n for n in dir(unreal.ObjectTypeQuery) if not n.startswith("_")]
    for want in ("ECC_TRAFFIC_ROAD", "TRAFFIC_ROAD"):
        if want in members:
            return getattr(unreal.ObjectTypeQuery, want)
    for n in members:
        if "ROAD" in n.upper():
            return getattr(unreal.ObjectTypeQuery, n)
    return None


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()

    lanes = []
    for a in eas.get_all_level_actors():
        if not any(str(t).startswith(TAG_PREFIX) for t in a.tags):
            continue
        if not a.get_actor_label().startswith("Lane_"):
            continue
        sp = a.get_component_by_class(unreal.SplineComponent)
        if sp is not None and sp.get_number_of_spline_points() >= 2:
            lanes.append((a, sp))
    if MAX_LANES:
        lanes = lanes[:MAX_LANES]
    w("车道段 %d 条" % len(lanes))

    ot = road_object_type()
    if ot is None:
        w("!! 找不到 Traffic_Road 的 ObjectTypeQuery")
        flush()
        return
    w("探测通道 %s" % ot)
    w("每隔 %.0fcm 采一个点，沿车道切线往前扫" % STATION_STEP)
    w("")
    flush()

    w("=" * 92)
    w("%-8s %-8s %8s %8s %8s %8s %8s %8s" % (
        "前瞻", "半径", "样本", "正确", "串路", "串车道", "逆向", "没扫到"))
    w("=" * 92)

    detail_done = False
    for look in LOOKAHEADS:
        for rad in RADII:
            n = ok = xroad = xlane = xdir = miss = 0
            examples = []
            for a, sp in lanes:
                road, off = parse(a.get_actor_label())
                total = sp.get_spline_length()
                d = 0.0
                while d <= total:
                    p = sp.get_location_at_distance_along_spline(d, WS)
                    t = sp.get_tangent_at_distance_along_spline(d, WS)
                    h = (t.x * t.x + t.y * t.y + t.z * t.z) ** 0.5
                    d += STATION_STEP
                    if h < 1e-3:
                        continue
                    tx, ty, tz = t.x / h, t.y / h, t.z / h
                    end = unreal.Vector(p.x + tx * look, p.y + ty * look,
                                        p.z + tz * look)
                    n += 1
                    try:
                        hits = unreal.SystemLibrary.sphere_trace_multi_for_objects(
                            world, p, end, rad, [ot], False, [a],
                            unreal.DrawDebugTrace.NONE, True)
                    except Exception:
                        continue
                    first = None
                    for hh in hits or []:
                        try:
                            ha = hh.to_dict().get("hit_actor")
                            hl = ha.get_actor_label() if ha is not None else ""
                        except Exception:
                            continue
                        # 只有 Inter_ 能通过 Cast 成 IntersectionChild；
                        # Lane_ 会 Cast 失败，等于这一帧白做，不算认错。
                        if hl.startswith("Inter_"):
                            first = (ha, hl)
                            break
                    if first is None:
                        miss += 1
                        continue
                    ha, hl = first
                    hroad, hoff = parse(hl)
                    f = ha.get_actor_forward_vector()
                    dot = f.x * tx + f.y * ty
                    if dot < DOT_SAME_DIR and hroad == road:
                        xdir += 1
                    elif hroad != road:
                        xroad += 1
                        if len(examples) < 6:
                            examples.append("%s  ->  %s  (dot %.2f)"
                                            % (a.get_actor_label(), hl, dot))
                    elif hoff != off:
                        xlane += 1
                        if len(examples) < 6:
                            examples.append("%s  ->  %s  (dot %.2f)"
                                            % (a.get_actor_label(), hl, dot))
                    else:
                        ok += 1
            w("%-8.0f %-8.0f %8d %8d %8d %8d %8d %8d"
              % (look, rad, n, ok, xroad, xlane, xdir, miss))
            if examples and not detail_done:
                for e in examples:
                    w("         例: %s" % e)
                detail_done = True
            flush()

    w("")
    w("列的含义：")
    w("  正确   第一个命中的 Inter_ 就是自己这条车道的")
    w("  串路   命中别的路名的 Inter_ —— 横向车流，灯态相反，最危险")
    w("  串车道 同路但不同偏移的 Inter_ —— 隔壁平行车道")
    w("  逆向   同路但朝向相反（dot < %.1f）—— 对向车道" % DOT_SAME_DIR)
    w("  没扫到 这一段前方没有任何 Inter_（正常，路中间本来就没有路口）")
    w("")
    w("注意：真实的前瞻长度是随车速变的，半径也还没从蓝图里读出来，")
    w("      所以看的是趋势——串路/串车道的比例对参数有多敏感。")
    w("只读脚本，没有改动任何东西。")
    flush()
    unreal.log("[simtrace] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[simtrace] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
