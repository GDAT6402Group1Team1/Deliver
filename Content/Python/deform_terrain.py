# -*- coding: utf-8 -*-
"""用 EditorApplySpline 把道路下方的地形压平到路面高度。

关键接口（Landscape actor 上，BlueprintCallable，Python 可调）：
    EditorApplySpline(SplineComponent, StartWidth, EndWidth,
                      StartSideFalloff, EndSideFalloff, StartRoll, EndRoll,
                      NumSubdivisions, bRaiseHeights, bLowerHeights,
                      PaintLayer, EditLayerName)
影响范围 = Width(半宽) + SideFalloff，之外的地形完全不动。

为什么不能直接喂 形状13：
    实测它与真实路面高度差 -546 ~ +411 cm，按它压地形会压到错误高度。
    所以先沿路量出真实路面（Landscape 上的 SplineMeshComponent）的 Z，
    用这些点建一条临时样条，拿临时样条去压。

DRY_RUN=True 时只测量和报告，不改任何东西。
用法：py deform_terrain.py
"""

import traceback

import unreal

ROAD_LABELS = []               # 空 = 全部道路（调参时可填 ["形状 13"] 只跑一条）
SKIP_KEYWORDS = ["River"]      # 河流样条不是路，跳过
DRY_RUN = False                # True = 只测量报告，不动地形

SAMPLE_STEP = 200.0            # 沿路采样间距。加密可降低样条插值误差，
                               # 误差小了 TARGET_Z_OFFSET 才能收窄、路面不悬空
MEDIAN_WINDOW = 5              # 实测路面高度的中值滤波窗口（1 = 关闭）
TARGET_Z_OFFSET = -40.0        # 关键：地形目标高度 = 实测路面顶面 + 这个偏移
                               # 路面只有 6cm 厚，容错窗口极窄：
                               #   偏移太小 -> 地形冒出来，边缘被埋（齐平时实测 35%）
                               #   偏移太大 -> 路面悬空，侧面看是块飘着的板（-40 时空 34cm）
                               # 定案 -40：这一档实测不埋（中心 1.4% / 路缘 2.8%）。
                               # 悬空问题不靠缩小偏移解决——那会立刻变回埋住；
                               # 改为把路面加厚到 > 40cm，让地形嵌在板子里，两头兼顾。
HALF_WIDTH = None              # None = 自动横向探测实测半宽（推荐，路宽不一时必需）
                               # 形状13 是主路，实测 1800 宽 = 半宽 900；
                               # 之前手填 540 只压了中间 1080，540~900 那圈没碰到，边缘就埋着。
WIDTH_MARGIN = 150.0           # 半宽再外扩一点，保证路缘外侧也被压到。
                               # 从 60 提到 150：压宽一点只是多出一条平肩（侧面还有
                               # 绿色裙边盖着，几乎看不出来），压窄了却会把路缘埋掉，
                               # 两种误差的代价差很远，应该往宽了偏。

# 宽度以**大纲文件夹**的标称值为准，实测只用来兜底取大值。
# 理由和车道生成器里一样：实测会被路口、裙边、已压平的地形干扰。
# 之前只信实测 + WIDTH_PCTL=0.30（第 30 百分位），等于全路 70% 的断面都比
# 压平带宽——中间压到了、两边没压到，于是"中心不埋、路缘埋 37%"。
USE_NOMINAL_WIDTH = True
MAIN_ROAD_FOLDER = "MainRoad"
MAIN_HALF, SIDE_HALF = 900.0, 540.0   # 1800 / 1080 的一半
WIDTH_PROBE_MAX = 1400.0       # 横向探测最远距离
WIDTH_PROBE_STEP = 50.0
WIDTH_STATIONS = 13            # 断面多取几个，分位数更稳
WIDTH_PCTL = 0.30              # 取第 30 百分位而不是中位数。
                               # 路口处横向探测会打到交叉路的路面，只会把宽度测大不会测小，
                               # 所以低分位比中位稳健（形状30 只有 374m 却跨多个路口，
                               # 中位数被抬到 1375 = 路宽 2750，裙边整体探出路面）
MAX_HALF_WIDTH = 1200.0        # 硬上限兜底。从 1000 提到 1200：主路标称半宽 900
                               # 加 150 余量就是 1050，旧上限会把它削回去。             # 沿路取几个断面测宽度，取中位数（避开路口干扰）
SIDE_FALLOFF = 250.0           # 两侧过渡带
NUM_SUBDIV = 500               # 上一轮用 20 把路压得更糟：707m 只切 20 段 = 每段 35m，
                               # 变形成了粗折线，在路面上下反复穿插（浅而密）。
                               # 若这个参数是"每段"语义，500 会很慢，那就说明猜错了，改回小值。
RAISE = True
LOWER = True
EDIT_LAYER = None              # None = 自动找 LandscapeEditLayerSplines 那一层（推荐，可逆）
                               # 填字符串 = 强制指定；填 "" = 用引擎默认

UP = 60000.0
DOWN = 60000.0
MAX_STEPS = 8
WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "deform_terrain.txt"

lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[deform] %s" % s)


def road_surface_z(world, x, y, z_hint):
    """迭代射线：跳过地形，找到路面网格(SplineMesh)的高度。

    和贴地那个函数正好相反——那个跳过路面找地形，这个跳过地形找路面。
    路面被埋的地方，地形在上面，必须穿过去才能量到路。
    """
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
        if "SplineMesh" in cn:
            return z, "road"
        top = z - 1.0
        if top <= z_hint - DOWN:
            return None, "below-range"
    return None, "too-many-layers"


def horiz_right(tsp, d):
    """样条在该处右向量的水平单位分量。"""
    r = tsp.get_right_vector_at_distance_along_spline(d, WS)
    h = (r.x * r.x + r.y * r.y) ** 0.5
    if h < 1e-4:
        return 1.0, 0.0
    return r.x / h, r.y / h


def measure_half_width(world, tsp, total):
    """横向逐步探测，实测路面半宽。

    路宽不一（实测 1080~1800），手填一个值必然有的路压不全、有的路压过头。
    从中心向两侧每 WIDTH_PROBE_STEP 打一条射线，打不到路面网格就是到边了。
    取多个断面的中位数，避开路口处横向路面造成的虚高。
    """
    halves = []
    for k in range(1, WIDTH_STATIONS + 1):
        d = total * k / (WIDTH_STATIONS + 1.0)
        loc = tsp.get_location_at_distance_along_spline(d, WS)
        hx, hy = horiz_right(tsp, d)
        for sign in (-1.0, 1.0):
            off = 0.0
            last_ok = 0.0
            while off <= WIDTH_PROBE_MAX:
                z, _why = road_surface_z(world,
                                         loc.x + hx * off * sign,
                                         loc.y + hy * off * sign, loc.z)
                if z is None:
                    break
                last_ok = off
                off += WIDTH_PROBE_STEP
            if last_ok > 0:
                halves.append(last_ok)
    if not halves:
        return None, 0
    halves.sort()
    v = halves[min(len(halves) - 1, int(len(halves) * WIDTH_PCTL))]
    return min(v, MAX_HALF_WIDTH), len(halves)


def nominal_half(actor):
    """按大纲文件夹取标称半宽。认不出文件夹时当次路（偏保守）。"""
    try:
        folder = str(actor.get_folder_path())
    except Exception:
        folder = ""
    return MAIN_HALF if MAIN_ROAD_FOLDER.lower() in folder.lower() else SIDE_HALF


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    actors = eas.get_all_level_actors()

    land = None
    for a in actors:
        if a.get_class().get_name() == "Landscape":
            land = a
            break
    if land is None:
        w("!! 找不到 Landscape")
        return
    w("Landscape: %s" % land.get_actor_label())

    # ---- 编辑图层：决定可不可逆 ----
    w("")
    w("--- 编辑图层 ---")
    layers = None
    splines_layer_name = None
    try:
        layers = land.get_edit_layers_bp()
        w("get_edit_layers_bp() -> %d 个图层" % len(layers))
        for i, lay in enumerate(layers):
            # LayerName 那个 UPROPERTY 没有 EditAnywhere，只能走 UFUNCTION GetNameBP
            nm = None
            for meth in ("get_name_bp", "get_name"):
                if hasattr(lay, meth):
                    try:
                        nm = getattr(lay, meth)()
                        break
                    except Exception:
                        pass
            cls = type(lay).__name__
            w("   [%d] name=%r   类=%s" % (i, str(nm) if nm is not None else "?", cls))
            if "Splines" in cls and nm is not None:
                splines_layer_name = str(nm)
    except Exception as exc:
        w("get_edit_layers_bp() 失败: %s" % str(exc)[:90])

    if not layers:
        w(">>> 没有编辑图层 = 变形直接写基础高度图，**不可逆**，动手前务必先提交")
    else:
        w(">>> 有编辑图层 = 变形可以写进独立图层，事后能整层清除")
    if splines_layer_name:
        w(">>> 样条专用图层: %r" % splines_layer_name)
    else:
        w(">>> 没识别出样条专用图层，将使用引擎默认（可能落到普通图层上）")
    w("")

    # ---- 决定要处理哪些路 ----
    todo = []
    for a in actors:
        if a.get_class().get_name() != "BP_PS2DEMSplineActor_v3_C":
            continue
        lbl = a.get_actor_label().strip()
        if any(k.lower() in lbl.lower() for k in SKIP_KEYWORDS):
            continue
        if ROAD_LABELS and lbl not in ROAD_LABELS:
            continue
        todo.append((lbl, a))
    todo.sort()
    w("待处理道路 %d 条%s" % (len(todo), "（全部）" if not ROAD_LABELS else ""))
    w("")

    # ---- 逐条量真实路面高度并压地形 ----
    done = skipped = 0
    for idx, (label, road) in enumerate(todo):
        w("[%d/%d]" % (idx + 1, len(todo)))
        sp = road.get_component_by_class(unreal.SplineComponent)
        total = sp.get_spline_length()

        w("=" * 60)
        w("%s  总长 %.0f cm" % (label, total))
        w("=" * 60)

        if HALF_WIDTH is not None:
            half_w = HALF_WIDTH
            w("半宽: 手动指定 %.0f cm" % half_w)
        else:
            measured, n_probe = measure_half_width(world, sp, total)
            nom = nominal_half(a) if USE_NOMINAL_WIDTH else None
            if measured is None and nom is None:
                w("!! 横向探测不到路面，跳过这条路")
                skipped += 1
                continue
            base = max([v for v in (measured, nom) if v is not None])
            half_w = min(base + WIDTH_MARGIN, MAX_HALF_WIDTH)
            w("半宽: 标称 %s / 实测 %s（%d 次探测，第 %.0f 百分位）"
              % (("%.0f" % nom) if nom is not None else "-",
                 ("%.0f" % measured) if measured is not None else "-",
                 n_probe, WIDTH_PCTL * 100))
            w("      取大 %.0f + 余量 %.0f = %.0f  -> 压平带宽约 %.0f cm"
              % (base, WIDTH_MARGIN, half_w, half_w * 2))

        pts, why, deltas = [], {}, []
        d = 0.0
        while d <= total:
            p = sp.get_location_at_distance_along_spline(d, WS)
            rz, reason = road_surface_z(world, p.x, p.y, p.z)
            why[reason] = why.get(reason, 0) + 1
            if rz is not None:
                pts.append((d, p.x, p.y, rz))
                deltas.append(rz - p.z)
            d += SAMPLE_STEP

        w("采样 %d 点，量到路面 %d 点" % (int(total / SAMPLE_STEP) + 1, len(pts)))
        w("射线结果: %s" % ", ".join("%s×%d" % (k, v)
                                     for k, v in sorted(why.items(), key=lambda t: -t[1])))
        if deltas:
            deltas.sort()
            w("真实路面 Z 减去 形状 Z: 最小 %.0f  中位 %.0f  最大 %.0f cm"
              % (deltas[0], deltas[len(deltas) // 2], deltas[-1]))
            w("  -> 差值不为 0 就证明不能直接拿源样条去压地形")
        w("")
        w("将用于压地形的参数:")
        w("   半宽 %.0f + 侧向衰减 %.0f  => 单侧影响 %.0f cm，总带宽 %.0f cm"
          % (half_w, SIDE_FALLOFF, half_w + SIDE_FALLOFF,
             (half_w + SIDE_FALLOFF) * 2))
        use_layer = EDIT_LAYER if EDIT_LAYER is not None else (splines_layer_name or "")
        w("   RaiseHeights=%s  LowerHeights=%s  NumSubdivisions=%d" % (RAISE, LOWER, NUM_SUBDIV))
        w("   EditLayer=%r  %s" % (use_layer,
                                   "(自动选中样条专用层，可逆)" if use_layer and EDIT_LAYER is None
                                   else "(引擎默认)" if not use_layer else "(手动指定)"))

        if DRY_RUN:
            w("")
            w("DRY_RUN=True —— 没有改动任何地形。")
            w("确认参数合适后把 DRY_RUN 改成 False 再跑。")
            continue

        if len(pts) < 3:
            w("!! 量到的路面点太少，跳过")
            skipped += 1
            continue

        # 中值滤波去掉离群点：路口处射线常量到交叉那条路的路面，
        # 出现 -411 / +546 这种孤立尖刺（中位数只有 -1，说明整体是准的）。
        # 中值能杀尖刺又保留真实坡度（这条路全长有 43m 高差）。
        if MEDIAN_WINDOW > 1 and len(pts) >= MEDIAN_WINDOW:
            zs = [p[3] for p in pts]
            half = MEDIAN_WINDOW // 2
            fixed = []
            for i in range(len(zs)):
                seg = sorted(zs[max(0, i - half):min(len(zs), i + half + 1)])
                fixed.append(seg[len(seg) // 2])
            moved = [abs(fixed[i] - zs[i]) for i in range(len(zs))]
            big = len([m for m in moved if m > 50])
            w("中值滤波(窗口 %d): 修正了 %d 个点超过 50cm，最大修正 %.0f cm"
              % (MEDIAN_WINDOW, big, max(moved) if moved else 0.0))
            pts = [(pts[i][0], pts[i][1], pts[i][2], fixed[i]) for i in range(len(pts))]

        # 目标高度下移：地形要落在路面底面之下，不能和顶面齐平
        if TARGET_Z_OFFSET:
            pts = [(d0, x, y, z + TARGET_Z_OFFSET) for (d0, x, y, z) in pts]
            w("目标高度整体下移 %.0f cm（地形压到路面底面之下，而不是与顶面齐平）"
              % abs(TARGET_Z_OFFSET))

        # 用真实路面高度建一条临时样条，拿它去压地形
        tmp = eas.spawn_actor_from_class(
            unreal.EditorAssetLibrary.load_asset(
                "/Game/PS2DEM/BP_TrafficLine1").generated_class(),
            unreal.Vector(pts[0][1], pts[0][2], pts[0][3]))
        tmp.set_actor_label("__TempDeformSpline")
        tsp = tmp.get_component_by_class(unreal.SplineComponent)
        tsp.clear_spline_points(False)
        for _d, x, y, z in pts:
            tsp.add_spline_point(unreal.Vector(x, y, z), WS, False)
        # 必须设 CURVE_CLAMPED：默认自动切线在不规则高程数据上会过冲，
        # 样条自己就在实测路面高度上下摆动，地形再忠实跟随这条摆动的曲线，
        # 结果就是路面被反复穿插。上一轮漏了这步。
        for i in range(tsp.get_number_of_spline_points()):
            tsp.set_spline_point_type(i, unreal.SplinePointType.CURVE_CLAMPED, False)
        tsp.update_spline()

        try:
            land.editor_apply_spline(tsp, half_w, half_w,
                                     SIDE_FALLOFF, SIDE_FALLOFF,
                                     0.0, 0.0, NUM_SUBDIV, RAISE, LOWER,
                                     None, use_layer)
            w("EditorApplySpline 已执行，写入图层 %r" % use_layer)
            done += 1
        except Exception:
            w("EditorApplySpline 失败:" + chr(10) + traceback.format_exc())
        finally:
            eas.destroy_actor(tmp)
            w("临时样条已删除")

    w("")
    w("=" * 60)
    w("全部完成: 成功压平 %d 条，跳过 %d 条，共 %d 条" % (done, skipped, len(todo)))
    w("变形都写在图层 %r，不满意可整层清除。" % (splines_layer_name or "(默认)"))
    w("关卡尚未保存。")
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[deform] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
