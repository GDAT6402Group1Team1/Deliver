# -*- coding: utf-8 -*-
"""查清"查路口查到别的交通线"这件事：两类 Box 是不是共用一个碰撞通道。

背景：TraceForIntersection 查 ObjectTypeQuery7 = Traffic_Road。
早先实测 BP_TrafficLine1 的 Box 就是 ECC_TRAFFIC_ROAD——
如果路口段 Box 也是这个通道，探测就分不出"路口"和"普通车道"，
扫到哪个算哪个；单次扫描先撞上车道 Box 就直接结束，路口漏判。

项目里其实已经定义了 GameTraceChannel2 = Traffic_Intersection（ObjectTypeQuery8），
只是没拿来区分这两类。

这个脚本只读不改，出四份证据：
  1. 三类 actor（Lane_ / Inter_ / Intersection_）的 Box 通道和碰撞设置
  2. IntersectionChild 是不是 BP_TrafficLine1 的子类（决定 Cast 会不会误命中）
  3. 每个路口段 Box 周围有多少个同通道的车道 Box（量化误判概率）
  4. 真做一次球形扫描，按命中顺序列出来——看车道 Box 会不会排在路口 Box 前面

用法：py audit_traffic_channels.py
"""

import traceback

import unreal

LANE_BP = "/Game/PS2DEM/BP_TrafficLine1"
CHILD_BP = "/Game/PS2DEM/BP_TrafficLine1_IntersectionChild"
TAG_PREFIX = "ClaudeGenLane"

NEAR_RADIUS = 2000.0     # 统计路口段 Box 周围多远内的同通道 Box
SAMPLE_INTERS = 6        # 详细演示几个路口
TRACE_BACK = 2500.0      # 从路口段 Box 往后退多远起扫
TRACE_RADII = [50.0, 100.0]

OUT = unreal.Paths.project_saved_dir() + "traffic_channels.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    """边跑边写。否则后面一炸，前面几节的结果会被异常处理覆盖掉。"""
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def road_object_type():
    """取 Traffic_Road 对应的 ObjectTypeQuery 枚举值。

    通道在 ini 里起了名字之后，Python 枚举成员名就跟着变了
    （不再是 OBJECT_TYPE_QUERY7），所以按名字找而不是硬写。
    """
    members = [n for n in dir(unreal.ObjectTypeQuery) if not n.startswith("_")]
    w("  ObjectTypeQuery 的全部成员：%s" % ", ".join(sorted(members)))
    for want in ("TRAFFIC_ROAD", "OBJECT_TYPE_QUERY7"):
        if want in members:
            w("  用 unreal.ObjectTypeQuery.%s" % want)
            return getattr(unreal.ObjectTypeQuery, want)
    for n in members:
        if "ROAD" in n.upper():
            w("  用 unreal.ObjectTypeQuery.%s（名字里带 ROAD）" % n)
            return getattr(unreal.ObjectTypeQuery, n)
    w("  !! 找不到 Traffic_Road 对应的枚举，跳过实扫")
    return None


def obj_type_of(c):
    """PrimitiveComponent 的 ObjectType。get_editor_property 读不到，用函数。"""
    for how in ("get_collision_object_type",):
        fn = getattr(c, how, None)
        if fn is None:
            continue
        try:
            return str(fn())
        except Exception as exc:
            return "读取失败(%s)" % str(exc)[:40]
    try:
        return str(c.get_editor_property("collision_object_type"))
    except Exception as exc:
        return "读取失败(%s)" % str(exc)[:40]


def describe_box(a):
    for c in a.get_components_by_class(unreal.BoxComponent):
        e = c.get_unscaled_box_extent()
        try:
            ce = str(c.get_collision_enabled())
        except Exception:
            ce = "?"
        try:
            prof = str(c.get_collision_profile_name())
        except Exception:
            prof = "?"
        return ("半extent(%.0f,%.0f,%.0f)  通道=%s  碰撞=%s  预设=%s"
                % (e.x, e.y, e.z, obj_type_of(c), ce, prof))
    return "没有 BoxComponent"


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    actors = eas.get_all_level_actors()

    lanes, inters, bigs = [], [], []
    for a in actors:
        lbl = a.get_actor_label()
        if any(str(t).startswith(TAG_PREFIX) for t in a.tags):
            if lbl.startswith("Lane_"):
                lanes.append(a)
            elif lbl.startswith("Inter_"):
                inters.append(a)
        elif lbl.startswith("Intersection_"):
            bigs.append(a)

    w("=" * 78)
    w("1. 三类 Box 的通道和碰撞设置")
    w("=" * 78)
    for name, arr in (("Lane_ (BP_TrafficLine1)", lanes),
                      ("Inter_ (IntersectionChild)", inters),
                      ("Intersection_ (BP_Intersection)", bigs)):
        w("")
        w("  %s   共 %d 个" % (name, len(arr)))
        for a in arr[:3]:
            w("    %-26s %s" % (a.get_actor_label()[:26], describe_box(a)))
        if not arr:
            w("    （关卡里没有）")
    w("")
    w("  >>> 如果 Lane_ 和 Inter_ 的通道相同，探测就分不出这两类，")
    w("      这就是'查路口查到另一条交通线'的直接原因。")
    w("")
    flush()

    # --- 2. 类继承关系 ---
    w("=" * 78)
    w("2. IntersectionChild 是不是 BP_TrafficLine1 的子类")
    w("=" * 78)
    try:
        pc = unreal.EditorAssetLibrary.load_asset(LANE_BP).generated_class()
        cc = unreal.EditorAssetLibrary.load_asset(CHILD_BP).generated_class()
        w("  父候选 %s" % pc.get_name())
        w("  子候选 %s" % cc.get_name())
        for label, a, b in (("Child 是 Lane 的子类", cc, pc),
                            ("Lane 是 Child 的子类", pc, cc)):
            try:
                r = unreal.MathLibrary.class_is_child_of(a, b)
            except Exception as exc:
                r = "判定失败 %s" % str(exc)[:50]
            w("  %-24s -> %s" % (label, r))
        w("")
        w("  >>> 若 Child 是 Lane 的子类：Cast 到 Child 时，普通 Lane_ 会失败（好事，")
        w("      但这一帧的路口检查等于白做）。若两者无继承关系，Cast 同样失败。")
        w("      不管哪种，问题都在'扫描被车道 Box 抢先占掉'，不在 Cast。")
    except Exception as exc:
        w("  失败 %s" % str(exc)[:100])
    w("")
    flush()

    # --- 3. 路口段 Box 周围的同通道干扰 ---
    w("=" * 78)
    w("3. 每个路口段 Box 周围 %.0fcm 内有多少车道 Box" % NEAR_RADIUS)
    w("=" * 78)
    lane_pos = [(a.get_actor_location(), a.get_actor_label()) for a in lanes]
    worst = []
    for a in inters:
        p = a.get_actor_location()
        near = [(pl - p).length() for pl, _l in lane_pos if (pl - p).length() < NEAR_RADIUS]
        near.sort()
        worst.append((len(near), near[0] if near else 9e9, a.get_actor_label()))
    worst.sort(key=lambda t: -t[0])
    w("  %-30s %8s %12s" % ("路口段", "邻近车道Box", "最近距离"))
    w("  " + "-" * 56)
    for n, d, lbl in worst[:15]:
        w("  %-30s %8d %10.0f cm" % (lbl[:30], n, d if d < 9e9 else -1))
    if worst:
        tot = sum(n for n, _d, _l in worst)
        w("")
        w("  路口段共 %d 个，邻近车道 Box 合计 %d 个，平均每个路口段身边 %.1f 个"
          % (len(worst), tot, tot / max(1, len(worst))))
    w("")
    flush()

    # --- 4. 真扫一次 ---
    w("=" * 78)
    w("4. 实扫：从路口段后方 %.0fcm 扫向它，按命中顺序列出" % TRACE_BACK)
    w("=" * 78)
    rot_q = road_object_type()
    if rot_q is None:
        flush()
        return
    ot = [rot_q]
    for a in inters[:SAMPLE_INTERS]:
        p = a.get_actor_location()
        fwd = a.get_actor_forward_vector()
        start = unreal.Vector(p.x - fwd.x * TRACE_BACK,
                              p.y - fwd.y * TRACE_BACK,
                              p.z - fwd.z * TRACE_BACK)
        w("")
        w("  目标 %s  @ (%.0f, %.0f, %.0f)" % (a.get_actor_label(), p.x, p.y, p.z))
        for r in TRACE_RADII:
            try:
                hits = unreal.SystemLibrary.sphere_trace_multi_for_objects(
                    world, start, p, r, ot, False, [],
                    unreal.DrawDebugTrace.NONE, True)
            except Exception as exc:
                w("    半径 %-5.0f 扫描失败 %s" % (r, str(exc)[:60]))
                continue
            if not hits:
                w("    半径 %-5.0f 什么都没扫到" % r)
                continue
            w("    半径 %-5.0f 命中 %d 个：" % (r, len(hits)))
            for i, h in enumerate(hits):
                d = h.to_dict()
                try:
                    ha = d.get("hit_actor")
                    hl = ha.get_actor_label() if ha is not None else "?"
                except Exception:
                    hl = "?"
                dist = d.get("distance", -1)
                kind = ("路口段" if hl.startswith("Inter_")
                        else "车道" if hl.startswith("Lane_") else "其它")
                flag = ""
                if i == 0 and kind == "车道":
                    flag = "   <<< 单次扫描会停在这里，路口漏判"
                w("       %d. %-28s %-6s %8.0f cm%s"
                  % (i + 1, hl[:28], kind, dist, flag))

    w("")
    w("=" * 78)
    w("说明")
    w("=" * 78)
    w("  Traffic_Road        = GameTraceChannel1 = ObjectTypeQuery7（现在两类都用它）")
    w("  Traffic_Intersection= GameTraceChannel2 = ObjectTypeQuery8（已定义，闲置）")
    w("  只读脚本，什么都没改。")

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    unreal.log("[chan] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[chan] fail:" + chr(10) + err)
    # 追加而不是覆盖：前面几节已经跑出来的结果仍然有效，别一起丢掉
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
