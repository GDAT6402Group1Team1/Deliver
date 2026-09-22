# -*- coding: utf-8 -*-
"""量每条源道路样条两侧路面的**实际**半宽，看标称宽度和中心线还对不对。

diag_lane_fit.py 报出"左右路缘余量平均差 +403cm"——车道整体不在路中间了。
但那是从车道点出发量的，车道本身有 ±180/±540 的偏移，混在一起说不清
到底是"路加宽了"还是"路挪位了"。这里直接从**源道路样条**出发量：

    左半宽 / 右半宽 —— 从中心线沿法线往两侧走到路面消失
    两者相加  = 实际路宽，对比 MAIN_ROAD_WIDTH(1800) / SIDE_ROAD_WIDTH(1080)
    两者相减  = 中心线偏离路面中心多少（正 = 路面偏右，样条偏左）

这两个数直接决定要不要改 gen_traffic_lanes.py 顶上的
MAIN_ROAD_OFFSETS / SIDE_ROAD_OFFSETS——重跑生成器只会重测高度，
横向偏移是写死的常数，路加宽了它自己不会跟。

用中位数而不是平均：路口处法线会扫到交叉路的路面上，量出来是整条路宽，
平均会被这些点整体抬高，中位数不受影响。

只读，什么都不改。
用法：py diag_road_width.py
"""

import traceback

import unreal

ROAD_CLASS = "BP_PS2DEMSplineActor_v3_C"
MAIN_ROAD_FOLDER = "MainRoad"
MAIN_NOMINAL = 1800.0
SIDE_NOMINAL = 1080.0

STEP = 1000.0            # 沿路每隔多远量一个断面
MAX_STATIONS = 40        # 单条路最多量几个断面
SIDE_STEP = 50.0         # 横向步长，决定半宽的分辨率
SIDE_MAX = 2000.0        # 最远探到哪（要够宽才看得出加宽了多少）
SIDE_WINDOW = 300.0      # 只认落在中心线高度 ±这个范围内的路面

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "road_width.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def median(vals):
    if not vals:
        return None
    s = sorted(vals)
    n = len(s)
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2.0


def road_top(world, x, y, z_hint):
    """(x,y) 处路面（SplineMesh）的顶面高度，找不到返回 None。"""
    top = z_hint + SIDE_WINDOW
    bottom = z_hint - SIDE_WINDOW
    for _ in range(6):
        try:
            hit = unreal.SystemLibrary.line_trace_single(
                world, unreal.Vector(x, y, top), unreal.Vector(x, y, bottom),
                unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
                False, [], unreal.DrawDebugTrace.NONE, True)
        except Exception:
            return None
        if hit is None:
            return None
        d = hit.to_dict()
        if not d.get("blocking_hit"):
            return None
        comp = d.get("hit_component")
        cn = comp.get_class().get_name() if comp is not None else "?"
        z = d["impact_point"].z
        if "SplineMesh" in cn:
            return z
        top = z - 1.0            # 裙边/地形挡在前面，继续往下找
        if top <= bottom:
            return None
    return None


def half_width(world, p, rx, ry, z_ref):
    """沿 (rx,ry) 走到路面消失为止，返回半宽 cm。"""
    d = SIDE_STEP
    last = 0.0
    while d <= SIDE_MAX:
        if road_top(world, p.x + rx * d, p.y + ry * d, z_ref) is None:
            return last
        last = d
        d += SIDE_STEP
    return SIDE_MAX


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(
        unreal.UnrealEditorSubsystem).get_editor_world()

    roads = []
    for a in eas.get_all_level_actors():
        if a.get_class().get_name() != ROAD_CLASS:
            continue
        sp = a.get_component_by_class(unreal.SplineComponent)
        if sp is None or sp.get_number_of_spline_points() < 2:
            continue
        try:
            folder = str(a.get_folder_path())
        except Exception:
            folder = ""
        is_main = MAIN_ROAD_FOLDER.lower() in folder.lower()
        roads.append((a.get_actor_label(), sp, is_main))
    roads.sort(key=lambda t: t[0])

    w("源道路样条 %d 条（主路 %d）"
      % (len(roads), sum(1 for r in roads if r[2])))
    w("横向分辨率 %.0fcm，最远 %.0fcm" % (SIDE_STEP, SIDE_MAX))
    w("")
    w("%-16s %-4s %5s %9s %9s %9s %9s %9s" % (
        "路", "类型", "断面", "左半宽", "右半宽", "实际路宽", "标称", "中心偏移"))
    w("-" * 92)
    flush()

    summary = []
    for name, sp, is_main in roads:
        total = sp.get_spline_length()
        step = max(STEP, total / MAX_STATIONS) if total > 0 else STEP
        ls, rs = [], []
        d = 0.0
        while d <= total + 1e-3:
            p = sp.get_location_at_distance_along_spline(d, WS)
            r = sp.get_right_vector_at_distance_along_spline(d, WS)
            d += step
            h = (r.x * r.x + r.y * r.y) ** 0.5
            if h < 1e-4:
                continue
            rx, ry = r.x / h, r.y / h
            # 高度基准取中心线正下方的路面，而不是样条自己的 Z——
            # 源样条的 Z 本来就和真实路面差着几米（已知问题）。
            z_ref = road_top(world, p.x, p.y, p.z)
            if z_ref is None:
                continue
            ls.append(half_width(world, p, -rx, -ry, z_ref))
            rs.append(half_width(world, p, rx, ry, z_ref))

        ml, mr = median(ls), median(rs)
        nominal = MAIN_NOMINAL if is_main else SIDE_NOMINAL
        if ml is None:
            w("%-16s %-4s %5d %9s" % (name[:16], "主" if is_main else "次",
                                      len(ls), "量不到路面"))
            flush()
            continue
        width = ml + mr
        off = (mr - ml) / 2.0
        w("%-16s %-4s %5d %9.0f %9.0f %9.0f %9.0f %+9.0f"
          % (name[:16], "主" if is_main else "次", len(ls),
             ml, mr, width, nominal, off))
        summary.append((name, is_main, ml, mr, width, nominal, off))
        flush()

    w("")
    w("=" * 92)
    w("结论")
    w("=" * 92)
    for tag, want in (("主", True), ("次", False)):
        grp = [s for s in summary if s[1] == want]
        if not grp:
            continue
        mw = median([s[4] for s in grp])
        mo = median([s[6] for s in grp])
        nominal = MAIN_NOMINAL if want else SIDE_NOMINAL
        w("%s路 %d 条：实际路宽中位数 %.0f，标称 %.0f（差 %+.0f）"
          % (tag, len(grp), mw, nominal, mw - nominal))
        w("        中心偏移中位数 %+.0f（正 = 路面偏样条右侧）" % mo)
        # 车道落在等分点上：n 条车道把路宽 n+1 等分
        lanes = 4 if want else 2
        unit = mw / (lanes + 1)
        vals = [(-(lanes - 1) / 2.0 + i) * unit + mo for i in range(lanes)]
        w("        按现在的实际路宽重算等分点偏移："
          + ", ".join("%+.0f" % v for v in vals))
    w("")
    w("怎么用：")
    w("  实际路宽 ≈ 标称、中心偏移 ≈ 0  -> 横向不用动，只是高度过期，")
    w("      直接跑 rebuild_traffic.py 就够了。")
    w("  实际路宽明显变大/变小          -> 把上面那行重算出来的偏移填进")
    w("      gen_traffic_lanes.py 的 MAIN_ROAD_OFFSETS / SIDE_ROAD_OFFSETS，")
    w("      再跑 rebuild_traffic.py。偏移是写死的常数，重跑本身不会跟着变。")
    w("  中心偏移明显非 0                -> 路面相对源样条挪了位。重算出来的")
    w("      偏移里已经含了这个补偿量（每条路可能不同，必要时分路处理）。")
    w("只读脚本，没有改动任何东西。")
    flush()
    unreal.log("[width] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[width] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
