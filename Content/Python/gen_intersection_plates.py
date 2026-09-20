# -*- coding: utf-8 -*-
"""路口补一块整板，让路网看起来是一体的。

问题：两条路在交汇处各铺各的，两块 14cm 板子互相穿插。
上一版只把次路藏了，结果那里成了缺口——藏完没补东西。

正确做法（真实道路网格也是这么做的）：
    路口 = 一块完整的路面板
    两条路都在板子边缘收住，不再互相穿插

板子尺寸按"交汇矩形"算：沿主路方向取次路的宽度，横向取主路的宽度，
斜交时按夹角放大。材质用 ROAD_MAT 指定（从组件读会拿到网格自带的
WorldGridMaterial 棋盘格，不是地形样条实际用的那个）。
同时在板子下面补一块同样尺寸的绿色裙边。

已排除的方案：把路刷进地形材质。地形单元格 99.2cm，
刷出来的路边缘会以 1 米为单位锯齿化，比现在难看。

RESTORE=True 删除所有板子并恢复被隐藏的路面段。
用法：py gen_intersection_plates.py
"""

import math
import traceback

import unreal

RESTORE = True
SKIP_KEYWORDS = ["River"]
TAG = "ClaudeGenPlate"

XY_CROSS = 200.0
Z_CROSS = 600.0
DETECT_STEP = 250.0
MERGE_DIST = 2500.0            # 相距小于此值的交汇合并（三岔路会并成一个）

PLATE_MARGIN = 80.0            # 板子四周比交汇矩形多出多少
PLATE_THICK = 14.0             # 和路面同厚
OBLIQUE_CAP = 1.6              # 斜交时沿主路方向的放大上限。
                               # 2.2 时有几个路口被拉到 4120cm（41 米），像条大板不像路口，
                               # 那些是浅角度汇入（匝道那种），矩形板本来就不适合，先压住上限。
SKIRT_DEPTH = 200.0
SKIRT_TOP_DROP = 3.0

MESH_PATH = "/PCG/SampleContent/MeshSockets/Meshes/1M_CubeWithSocket"
SKIRT_MAT = "/Game/materials/M_RoadSkirt"
ROAD_MAT = "/Game/materials/road"   # 直接指定路面材质。
                                    # 从组件读会拿到 WorldGridMaterial（网格自带的默认棋盘格），
                                    # 不是地形样条实际用的那个。留空则退回从组件读。
MESH_SIZE = 100.0

WIDTH_PROBE_MAX = 1400.0
WIDTH_PROBE_STEP = 50.0
WIDTH_STATIONS = 7
UP = 60000.0
DOWN = 60000.0
MAX_STEPS = 8

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "intersection_plates.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[plate] %s" % s)


def road_top_z(world, x, y, z_hint):
    top = z_hint + UP
    for _ in range(MAX_STEPS):
        try:
            hit = unreal.SystemLibrary.line_trace_single(
                world, unreal.Vector(x, y, top), unreal.Vector(x, y, z_hint - DOWN),
                unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, False, [],
                unreal.DrawDebugTrace.NONE, True)
        except Exception:
            return None
        if hit is None:
            return None
        d = hit.to_dict()
        if not d.get("blocking_hit"):
            return None
        comp = d.get("hit_component")
        z = d["impact_point"].z
        cn = comp.get_class().get_name() if comp is not None else "?"
        if "SplineMesh" in cn:
            return z
        top = z - 1.0
        if top <= z_hint - DOWN:
            return None
    return None


def horiz_right(sp, d):
    r = sp.get_right_vector_at_distance_along_spline(d, WS)
    h = (r.x * r.x + r.y * r.y) ** 0.5
    if h < 1e-4:
        return 1.0, 0.0
    return r.x / h, r.y / h


def measure_half_width(world, sp, total):
    halves = []
    for k in range(1, WIDTH_STATIONS + 1):
        d = total * k / (WIDTH_STATIONS + 1.0)
        loc = sp.get_location_at_distance_along_spline(d, WS)
        hx, hy = horiz_right(sp, d)
        for sign in (-1.0, 1.0):
            off, last_ok = 0.0, 0.0
            while off <= WIDTH_PROBE_MAX:
                if road_top_z(world, loc.x + hx * off * sign,
                              loc.y + hy * off * sign, loc.z) is None:
                    break
                last_ok = off
                off += WIDTH_PROBE_STEP
            if last_ok > 0:
                halves.append(last_ok)
    if not halves:
        return None
    halves.sort()
    return halves[len(halves) // 2]


def comp_pos(c):
    try:
        return c.k2_get_component_location()
    except Exception:
        return None


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
    sms = list(land.get_components_by_class(unreal.SplineMeshComponent))

    # ---- 撤销 ----
    n_del = 0
    for a in actors:
        if TAG in [str(t) for t in a.tags]:
            eas.destroy_actor(a)
            n_del += 1
    if RESTORE:
        n_show = 0
        for c in sms:
            try:
                if not c.is_visible():
                    c.set_visibility(True, False)
                    n_show += 1
            except Exception:
                pass
        w("已删除板子 %d 个，恢复路面段 %d 个。关卡尚未保存。" % (n_del, n_show))
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
        return
    # 先把所有隐藏的路面段恢复，再按新板子范围重新隐藏。
    # 否则调小板子重跑时，旧范围里多藏的段会留成空洞。
    n_show = 0
    for c in sms:
        try:
            if not c.is_visible():
                c.set_visibility(True, False)
                n_show += 1
        except Exception:
            pass
    w("清除旧板子 %d 个，恢复路面段 %d 个（重跑前先归位）" % (n_del, n_show))

    mesh = unreal.EditorAssetLibrary.load_asset(MESH_PATH) \
        or unreal.EditorAssetLibrary.load_asset("/Engine/BasicShapes/Cube")
    skirt_mat = unreal.EditorAssetLibrary.load_asset(SKIRT_MAT)
    # 路面材质直接从现有路面段读，保证和路完全一致
    road_mat = unreal.EditorAssetLibrary.load_asset(ROAD_MAT) if ROAD_MAT else None
    if road_mat is None:
        for c in sms:
            try:
                m = c.get_material(0)
                if m is not None and "WorldGrid" not in m.get_name():
                    road_mat = m
                    break
            except Exception:
                continue
    w("网格 %s   路面材质 %s   裙边材质 %s"
      % (mesh.get_name(), road_mat.get_name() if road_mat else "None",
         skirt_mat.get_name() if skirt_mat else "None"))
    if road_mat is None:
        w("!! 读不到路面材质，中止")
        return

    roads = []
    for a in actors:
        if a.get_class().get_name() != "BP_PS2DEMSplineActor_v3_C":
            continue
        lbl = a.get_actor_label().strip()
        if any(k.lower() in lbl.lower() for k in SKIP_KEYWORDS):
            continue
        sp = a.get_component_by_class(unreal.SplineComponent)
        if sp:
            roads.append([lbl, sp, 0.0])
    w("道路 %d 条，测量半宽…" % len(roads))
    for r in roads:
        r[2] = measure_half_width(world, r[1], r[1].get_spline_length()) or 500.0

    # ---- 找交汇并合并（三岔路会并成一个） ----
    raw = []
    for i in range(len(roads)):
        for j in range(i + 1, len(roads)):
            na, spa, wa = roads[i]
            nb, spb, wb = roads[j]
            d, tot, best = 0.0, spa.get_spline_length(), None
            while d <= tot:
                p = spa.get_location_at_distance_along_spline(d, WS)
                q = spb.find_location_closest_to_world_location(p, WS)
                dxy = ((q.x - p.x) ** 2 + (q.y - p.y) ** 2) ** 0.5
                if dxy < XY_CROSS and abs(q.z - p.z) < Z_CROSS:
                    if best is None or dxy < best[1]:
                        best = (p, dxy, d)
                d += DETECT_STEP
            if best is not None:
                raw.append({"pos": best[0], "da": best[2],
                            "a": (na, spa, wa), "b": (nb, spb, wb)})

    groups = []
    for r in raw:
        hit = None
        for g in groups:
            if (g["pos"] - r["pos"]).length() < MERGE_DIST:
                hit = g
                break
        if hit is None:
            groups.append({"pos": r["pos"], "items": [r]})
        else:
            hit["items"].append(r)
    w("交汇 %d 处（合并后）" % len(groups))
    w("")

    made = hidden = 0
    for gi, g in enumerate(groups):
        items = g["items"]
        # 主路 = 参与这个路口的最宽的那条
        parts = {}
        for r in items:
            for nm, sp, hw in (r["a"], r["b"]):
                parts[nm] = (sp, hw)
        major = max(parts.items(), key=lambda kv: kv[1][1])
        maj_name, (maj_sp, maj_hw) = major
        other_hw = max([hw for nm, (sp, hw) in parts.items() if nm != maj_name] or [maj_hw])

        ctr = g["pos"]
        tz = road_top_z(world, ctr.x, ctr.y, ctr.z)
        if tz is None:
            w("[%d] (%.0f, %.0f) 量不到路面，跳过" % (gi, ctr.x, ctr.y))
            continue

        # 主路在该处的方向；斜交时沿主路方向放大
        dmaj = maj_sp.get_distance_along_spline_at_location(ctr, WS) \
            if hasattr(maj_sp, "get_distance_along_spline_at_location") else 0.0
        fwd = maj_sp.get_direction_at_distance_along_spline(dmaj, WS)
        fwd = unreal.Vector(fwd.x, fwd.y, 0.0)
        oblique = 1.0
        for nm, (sp, hw) in parts.items():
            if nm == maj_name:
                continue
            try:
                dd = sp.get_distance_along_spline_at_location(ctr, WS)
                f2 = sp.get_direction_at_distance_along_spline(dd, WS)
                dot = abs(fwd.x * f2.x + fwd.y * f2.y) / max(
                    1e-4, (fwd.x ** 2 + fwd.y ** 2) ** 0.5 * (f2.x ** 2 + f2.y ** 2) ** 0.5)
                sin_a = max(0.3, math.sqrt(max(0.0, 1.0 - dot * dot)))
                oblique = max(oblique, min(OBLIQUE_CAP, 1.0 / sin_a))
            except Exception:
                pass

        len_along = other_hw * 2.0 * oblique + PLATE_MARGIN * 2.0
        wid_across = maj_hw * 2.0 + PLATE_MARGIN * 2.0
        rot = unreal.MathLibrary.make_rot_from_x(fwd)

        # 路面板
        plate = eas.spawn_actor_from_class(
            unreal.StaticMeshActor,
            unreal.Vector(ctr.x, ctr.y, tz - PLATE_THICK / 2.0), rot)
        smc = plate.static_mesh_component
        smc.set_static_mesh(mesh)
        smc.set_material(0, road_mat)
        smc.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
        plate.set_actor_scale3d(unreal.Vector(len_along / MESH_SIZE,
                                              wid_across / MESH_SIZE,
                                              PLATE_THICK / MESH_SIZE))
        plate.set_actor_label("Plate_%02d_%s" % (gi, maj_name.replace(" ", "")))
        plate.tags = [TAG]
        try:
            plate.set_folder_path("RoadPlates")
        except Exception:
            pass
        made += 1

        # 板子下的裙边
        if skirt_mat is not None:
            sk = eas.spawn_actor_from_class(
                unreal.StaticMeshActor,
                unreal.Vector(ctr.x, ctr.y,
                              tz - SKIRT_TOP_DROP - SKIRT_DEPTH / 2.0), rot)
            ssc = sk.static_mesh_component
            ssc.set_static_mesh(mesh)
            ssc.set_material(0, skirt_mat)
            ssc.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
            sk.set_actor_scale3d(unreal.Vector(len_along / MESH_SIZE,
                                               wid_across / MESH_SIZE,
                                               SKIRT_DEPTH / MESH_SIZE))
            sk.set_actor_label("PlateSkirt_%02d" % gi)
            sk.tags = [TAG]
            try:
                sk.set_folder_path("RoadPlates")
            except Exception:
                pass

        # 把板子覆盖范围内、所有路的路面段都藏掉（不只是次路）
        half_l, half_w2 = len_along / 2.0, wid_across / 2.0
        fl = (fwd.x ** 2 + fwd.y ** 2) ** 0.5 or 1.0
        fx, fy = fwd.x / fl, fwd.y / fl
        for c in sms:
            p = comp_pos(c)
            if p is None:
                continue
            dx, dy = p.x - ctr.x, p.y - ctr.y
            along = dx * fx + dy * fy
            across = -dx * fy + dy * fx
            if abs(along) <= half_l and abs(across) <= half_w2:
                try:
                    if c.is_visible():
                        c.set_visibility(False, False)
                        hidden += 1
                except Exception:
                    pass

        w("[%d] (%.0f, %.0f)  主路 %s  板 %.0f x %.0f  斜交系数 %.2f  路 %s"
          % (gi, ctr.x, ctr.y, maj_name, len_along, wid_across, oblique,
             "+".join(sorted(parts.keys()))))

    w("")
    w("共生成路口板 %d 块（各带一块裙边），隐藏路面段 %d 个" % (made, hidden))
    w("RESTORE=True 可删除全部板子并恢复路面段。关卡尚未保存。")

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[plate] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
