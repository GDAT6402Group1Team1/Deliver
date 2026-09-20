# -*- coding: utf-8 -*-
"""给道路样条加密并贴合地形，然后（可选）重新生成地形样条。

思路（用户提的）：与其改地形高度图去迁就路，不如给路加点让它跟着地形走。
优点是完全不动 landscape，没有不可逆的大改动、没有几百 MB 的二进制 diff。

难点与解法：
  * 往下打射线会先撞到路面网格（盖在地形上面），拿不到地形高度。
    解法是**迭代射线**：命中 SplineMeshComponent 就从命中点略下方重新起一条，
    直到命中 LandscapeComponent。
  * 完全贴合每个起伏会让路很颠（车和布娃娃都受影响），所以对采到的高度
    做滑动平均平滑。SMOOTH_WINDOW 调大 = 更平顺但更不贴地。

生成地形样条要靠插件的 UPS2DEMLandscapeSplineLibrary：
    ConvertSelectedRoutesToLandscapeSplines(bShowConfirmation)
默认 DO_CONVERT=False，先只改样条点，你在视口里看过没问题再开。

用法：py densify_road.py
"""

import traceback

import unreal

ROAD_LABELS = ["形状 13"]     # 先只动一条验证；确认没问题再加
DENSIFY_STEP = 1500.0         # 新的点间距（原来平均 6400cm 一个点）
SMOOTH_WINDOW = 3             # 高度滑动平均窗口（1=不平滑，越大越平顺）
Z_LIFT = 0.0                  # 采到地形高度后整体抬高
DO_CONVERT = False            # True = 改完直接调插件重新生成地形样条
MAX_STEPS = 8                 # 迭代射线最多穿几层

UP = 60000.0
DOWN = 60000.0
WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "densify_road.txt"

lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[densify] %s" % s)


def terrain_z(world, x, y, z_hint):
    """迭代射线：跳过路面网格，一直打到地形本体。"""
    top = z_hint + UP
    for _ in range(MAX_STEPS):
        start = unreal.Vector(x, y, top)
        end = unreal.Vector(x, y, z_hint - DOWN)
        try:
            hit = unreal.SystemLibrary.line_trace_single(
                world, start, end, unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
                False, [], unreal.DrawDebugTrace.NONE, True)
        except Exception:
            return None, "trace-error"
        if hit is None:
            return None, "none"
        d = hit.to_dict()
        if not d.get("blocking_hit"):
            return None, "no-hit"
        comp = d.get("hit_component")
        z = d["impact_point"].z
        cn = comp.get_class().get_name() if comp is not None else "?"
        if "Landscape" in cn:
            return z, cn
        # 命中路面网格或别的东西，从它下方 1cm 重新起一条
        top = z - 1.0
        if top <= z_hint - DOWN:
            return None, "below-range"
    return None, "too-many-layers"


def smooth(vals, win):
    if win <= 1 or len(vals) < 3:
        return list(vals)
    out = []
    half = win // 2
    for i in range(len(vals)):
        a = max(0, i - half)
        b = min(len(vals), i + half + 1)
        seg = [v for v in vals[a:b] if v is not None]
        out.append(sum(seg) / len(seg) if seg else vals[i])
    return out


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()

    targets = []
    for a in eas.get_all_level_actors():
        if a.get_class().get_name() != "BP_PS2DEMSplineActor_v3_C":
            continue
        if a.get_actor_label().strip() in ROAD_LABELS:
            targets.append(a)
    if not targets:
        w("!! 找不到 %s" % ROAD_LABELS)
        return
    w("目标道路 %d 条，加密间距 %.0f cm，平滑窗口 %d"
      % (len(targets), DENSIFY_STEP, SMOOTH_WINDOW))
    w("")

    for actor in targets:
        label = actor.get_actor_label()
        sp = actor.get_component_by_class(unreal.SplineComponent)
        total = sp.get_spline_length()
        n_before = sp.get_number_of_spline_points()

        dists = []
        d = 0.0
        while d < total:
            dists.append(d)
            d += DENSIFY_STEP
        dists.append(total)

        xs, ys, zs_old, zs_new, why = [], [], [], [], {}
        for dd in dists:
            p = sp.get_location_at_distance_along_spline(dd, WS)
            xs.append(p.x)
            ys.append(p.y)
            zs_old.append(p.z)
            tz, reason = terrain_z(world, p.x, p.y, p.z)
            why[reason] = why.get(reason, 0) + 1
            zs_new.append(tz)

        got = len([z for z in zs_new if z is not None])
        w("%s  原 %d 点 / 长 %.0f cm  ->  新 %d 点，地形命中 %d"
          % (label, n_before, total, len(dists), got))
        w("   射线结果: %s" % ", ".join("%s×%d" % (k, v) for k, v in
                                        sorted(why.items(), key=lambda t: -t[1])))
        if got < len(dists) * 0.5:
            w("   !! 命中率过低，跳过这条路（不改动）")
            continue

        # 打空的点用相邻有效值兜底
        filled = list(zs_new)
        for i, v in enumerate(filled):
            if v is None:
                prev = next((filled[j] for j in range(i - 1, -1, -1) if filled[j] is not None), None)
                nxt = next((zs_new[j] for j in range(i + 1, len(zs_new)) if zs_new[j] is not None), None)
                filled[i] = prev if prev is not None else (nxt if nxt is not None else zs_old[i])

        final = [z + Z_LIFT for z in smooth(filled, SMOOTH_WINDOW)]

        deltas = [abs(final[i] - zs_old[i]) for i in range(len(final))]
        w("   高度调整: 平均 %.0f cm，最大 %.0f cm"
          % (sum(deltas) / len(deltas), max(deltas)))

        sp.clear_spline_points(False)
        for i in range(len(dists)):
            sp.add_spline_point(unreal.Vector(xs[i], ys[i], final[i]), WS, False)
        for i in range(sp.get_number_of_spline_points()):
            sp.set_spline_point_type(i, unreal.SplinePointType.CURVE_CLAMPED, False)
        sp.update_spline()
        for name in ("spline_has_been_edited", "b_spline_has_been_edited"):
            try:
                sp.set_editor_property(name, True)
                break
            except Exception:
                continue
        w("   已写入 %d 个点" % sp.get_number_of_spline_points())
        w("")

    if DO_CONVERT:
        w("选中目标道路并调用插件重新生成地形样条…")
        eas.set_selected_level_actors(targets)
        try:
            ok = unreal.PS2DEMLandscapeSplineLibrary.convert_selected_routes_to_landscape_splines(False)
            w("ConvertSelectedRoutesToLandscapeSplines -> %s" % ok)
            w(unreal.PS2DEMLandscapeSplineLibrary.get_generated_landscape_spline_summary())
        except Exception:
            w("插件调用失败:" + chr(10) + traceback.format_exc())
    else:
        w("DO_CONVERT=False —— 只改了样条点，没有重新生成地形样条。")
        w("先在视口里选中这条路看看新点位贴不贴地形，满意了把 DO_CONVERT 改成 True 再跑一次。")

    w("")
    w("注意: 源样条改了，之前生成的车道需要重跑 gen_traffic_lanes.py。关卡尚未保存。")

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[densify] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
