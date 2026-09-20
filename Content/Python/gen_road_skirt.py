# -*- coding: utf-8 -*-
"""在路面下方生成与地面同色的"裙边"，挡住压地形后露出的空隙。

为什么用这个方案：
    路面只有 6cm 厚，地形压到顶面下 40cm 才不会埋住它，于是底下空 34cm，
    侧面看是块飘着的板。缩小偏移会立刻变回埋住——两头没法兼顾。
    在路面下补一圈和地面同色的 mesh 把空隙挡住，纯加法：
    地形和路面都不用动，不满意删掉 actor 即可，零残留。

几何：
    路面顶   road_top
    裙边顶   road_top - 5        略微重叠，避免接缝漏光
    地形     road_top - 40       被裙边挡住
    裙边底   road_top - 205      埋在地下

依赖：先跑 make_skirt_material.py 生成 M_RoadSkirt。
用法：py gen_road_skirt.py
"""

import traceback

import unreal

ROAD_LABELS = []               # 空 = 全部道路（调参时可填 ["形状 13"] 只跑一条）
SKIP_KEYWORDS = ["River"]
TAG = "ClaudeGenSkirt"

MESH_PATH = "/PCG/SampleContent/MeshSockets/Meshes/1M_CubeWithSocket"
                               # 和路面用的是同一个底模（在 PCG 插件的示例内容里）。
                               # 找不到会自动退回 /Engine/BasicShapes/Cube，同样是 100^3。
MAT_PATH = "/Game/materials/M_RoadSkirt"
MESH_SIZE = 100.0              # 底模边长

SEG_LEN = 500.0                # 每段裙边的长度（每段一个 StaticMeshActor）
SUB_SAMPLES = 6                # 每段内部再采样几次，用来保证弦不高过真实路面
MAX_GRADE = 1.0                # 纵坡上限（|dz|/水平长度）。真实道路不会超过 45 度，
                               # 超了就是数据异常，跳过该段免得生成歪掉的盒子
TOP_OVERLAP = 11.0             # 裙边顶低于路面顶多少。
                               # 路面现在 14cm 厚，所以 11 仍嵌在板子里（比底面高 3cm，不露缝），
                               # 但比顶面低 11cm —— 容错从 3cm 提到 11cm。
                               # 之前 3cm 太紧：裙边是横向水平的方块，遇到路面横向倾斜
                               # （弯道超高、横跨斜坡）时，低侧边缘就会冒出来盖住路。
DEPTH = 200.0                  # 裙边总高度，要大于 |deform 的 Z 偏移|(40) 很多
WIDTH_MARGIN = -25.0           # 裙边比路面宽出多少。取负 = 略窄于路面，
                               # 免得横向倾斜时裙边从路边探出来

WIDTH_PROBE_MAX = 1400.0
WIDTH_PROBE_STEP = 50.0
WIDTH_STATIONS = 13            # 断面多取几个，分位数更稳
WIDTH_PCTL = 0.30              # 取第 30 百分位而不是中位数。
                               # 路口处横向探测会打到交叉路的路面，只会把宽度测大不会测小，
                               # 所以低分位比中位稳健（形状30 只有 374m 却跨多个路口，
                               # 中位数被抬到 1375 = 路宽 2750，裙边整体探出路面）
MAX_HALF_WIDTH = 1000.0        # 硬上限兜底：实测最宽的路是 1800（半宽 900）
UP = 60000.0
DOWN = 60000.0
MAX_STEPS = 8

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "road_skirt.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[skirt] %s" % s)


def road_top_z(world, x, y, z_hint, want_normal=False):
    """穿过地形找到路面网格顶面。want_normal=True 时返回 (z, 法线)。

    法线用来让裙边跟着路面倾斜——裙边若横向水平，遇到 1 度的横向倾斜
    在半宽 900 处就有 15.7cm 高差，足以冒出路面。
    """
    top = z_hint + UP
    for _ in range(MAX_STEPS):
        try:
            hit = unreal.SystemLibrary.line_trace_single(
                world, unreal.Vector(x, y, top), unreal.Vector(x, y, z_hint - DOWN),
                unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, False, [],
                unreal.DrawDebugTrace.NONE, True)
        except Exception:
            return (None, None) if want_normal else None
        if hit is None:
            return (None, None) if want_normal else None
        d = hit.to_dict()
        if not d.get("blocking_hit"):
            return (None, None) if want_normal else None
        comp = d.get("hit_component")
        z = d["impact_point"].z
        cn = comp.get_class().get_name() if comp is not None else "?"
        if "SplineMesh" in cn:
            if want_normal:
                n = d.get("impact_normal")
                return z, (n if n is not None else unreal.Vector(0, 0, 1))
            return z
        top = z - 1.0
        if top <= z_hint - DOWN:
            return (None, None) if want_normal else None
    return (None, None) if want_normal else None


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
    v = halves[min(len(halves) - 1, int(len(halves) * WIDTH_PCTL))]
    return min(v, MAX_HALF_WIDTH)


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    actors = eas.get_all_level_actors()

    removed = 0
    for a in actors:
        if TAG in [str(t) for t in a.tags]:
            eas.destroy_actor(a)
            removed += 1
    w("清除旧裙边 %d 个" % removed)

    mesh = unreal.EditorAssetLibrary.load_asset(MESH_PATH)
    if mesh is None:
        mesh = unreal.EditorAssetLibrary.load_asset("/Engine/BasicShapes/Cube")
        w("找不到 %s，退回引擎 Cube" % MESH_PATH)
    mat = unreal.EditorAssetLibrary.load_asset(MAT_PATH)
    if mat is None:
        w("!! 找不到材质 %s —— 请先跑 make_skirt_material.py" % MAT_PATH)
        return
    w("网格 %s   材质 %s" % (mesh.get_name(), mat.get_name()))
    w("")

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
    w("待处理道路 %d 条" % len(todo))

    total_segs = 0
    for idx, (label, road) in enumerate(todo):
        sp = road.get_component_by_class(unreal.SplineComponent)
        total = sp.get_spline_length()
        half = measure_half_width(world, sp, total)
        if half is None:
            w("[%d/%d] %s 探测不到路面，跳过" % (idx + 1, len(todo), label))
            continue
        half += WIDTH_MARGIN

        # 采样：位置 + 路面顶面高度；裙边中心在 top - TOP_OVERLAP - DEPTH/2
        # 记 (沿线距离, XY, 路面顶面Z)；顶面高度留着做弦修正
        pts = []
        d = 0.0
        while d <= total:
            loc = sp.get_location_at_distance_along_spline(d, WS)
            tz, nrm = road_top_z(world, loc.x, loc.y, loc.z, want_normal=True)
            if tz is not None:
                pts.append((d, loc.x, loc.y, tz, nrm))
            d += SEG_LEN
        if len(pts) < 2:
            w("[%d/%d] %s 有效采样点不足，跳过" % (idx + 1, len(todo), label))
            continue

        # AddComponentByClass 标了 ScriptNoExport，Python 加不了组件，
        # 所以每段生成一个 StaticMeshActor（拉伸的立方体），全是验证过的 API。
        made = 0
        steep = 0
        folder = "RoadSkirt"
        # 弦修正：段内密集采样，若直线弦高过真实路面就得下压。
        # 关键：drop 必须按“点”算而不是按“段”算——按段算的话，
        # 第 N 段的终点和第 N+1 段的起点会被压不同的量、落在不同高度，
        # 相邻段不再共用端点。坡度剧变处两端错位很大，算出的方向向量
        # 就会严重偏离路的走向，盒子整个转歪（Skirt_形状1_361 就是这么歪的）。
        seg_drop = []
        for i in range(len(pts) - 1):
            d0, x0, y0, t0, n0 = pts[i]
            d1, x1, y1, t1, n1 = pts[i + 1]
            drop = 0.0
            for k in range(1, SUB_SAMPLES):
                f = float(k) / SUB_SAMPLES
                dm = d0 + (d1 - d0) * f
                lm = sp.get_location_at_distance_along_spline(dm, WS)
                tm = road_top_z(world, lm.x, lm.y, lm.z)
                if tm is None:
                    continue
                chord = t0 + (t1 - t0) * f
                if chord - tm > drop:
                    drop = chord - tm
            seg_drop.append(drop)

        # 每个点取相邻两段所需 drop 的较大值 -> 所有段共用同一批点，折线连续
        pt_drop = []
        for i in range(len(pts)):
            cand = []
            if i > 0:
                cand.append(seg_drop[i - 1])
            if i < len(seg_drop):
                cand.append(seg_drop[i])
            pt_drop.append(max(cand) if cand else 0.0)
        max_drop = max(pt_drop) if pt_drop else 0.0

        zs = [pts[i][3] - pt_drop[i] - TOP_OVERLAP - DEPTH / 2.0
              for i in range(len(pts))]

        for i in range(len(pts) - 1):
            _d0, x0, y0, _t0, n0 = pts[i]
            _d1, x1, y1, _t1, n1 = pts[i + 1]
            p0 = unreal.Vector(x0, y0, zs[i])
            p1 = unreal.Vector(x1, y1, zs[i + 1])
            dv = unreal.Vector(p1.x - p0.x, p1.y - p0.y, p1.z - p0.z)
            seg = dv.length()
            if seg < 1.0:
                continue
            horiz = (dv.x * dv.x + dv.y * dv.y) ** 0.5
            if horiz < 1.0 or abs(dv.z) / horiz > MAX_GRADE:
                steep += 1
                continue
            mid = unreal.Vector((p0.x + p1.x) / 2.0, (p0.y + p1.y) / 2.0,
                                (p0.z + p1.z) / 2.0)
            # X 沿路，Z 取路面法线 -> 裙边顶面与路面平行，横向倾斜不再造成高差
            nz = unreal.Vector((n0.x + n1.x) * 0.5, (n0.y + n1.y) * 0.5,
                               (n0.z + n1.z) * 0.5)
            if nz.length() < 1e-3:
                nz = unreal.Vector(0, 0, 1)
            try:
                rot = unreal.MathLibrary.make_rot_from_xz(dv, nz)
            except Exception:
                rot = unreal.MathLibrary.make_rot_from_x(dv)
            a = eas.spawn_actor_from_class(unreal.StaticMeshActor, mid, rot)
            smc = None
            for getter in (lambda: a.static_mesh_component,
                           lambda: a.get_editor_property("static_mesh_component"),
                           lambda: a.get_component_by_class(unreal.StaticMeshComponent)):
                try:
                    smc = getter()
                    if smc is not None:
                        break
                except Exception:
                    continue
            if smc is None:
                if made == 0:
                    w("   !! 取不到 StaticMeshComponent，中止这条路")
                eas.destroy_actor(a)
                break
            smc.set_static_mesh(mesh)
            smc.set_material(0, mat)
            smc.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
            a.set_actor_scale3d(unreal.Vector(seg / MESH_SIZE,
                                              half * 2.0 / MESH_SIZE,
                                              DEPTH / MESH_SIZE))
            a.set_actor_label("Skirt_%s_%03d" % (label.replace(" ", ""), i))
            a.tags = [TAG]
            try:
                a.set_folder_path(folder)
            except Exception:
                pass
            made += 1

        total_segs += made
        w("[%d/%d] %-10s 半宽 %.0f  长 %.0fm  ->  %d 段  最大弦修正 %.1f cm%s"
          % (idx + 1, len(todo), label, half, total / 100.0, made, max_drop,
             ("  跳过陡坡段 %d" % steep) if steep else ""))

    w("")
    w("共生成裙边段 %d 个。关卡尚未保存。" % total_segs)
    w("颜色不对就去改 M_RoadSkirt 的 Color 参数，不用重跑本脚本。")
    w("要删除: clean_gen.py 里把 TAGS 改成 [\"%s\"]" % TAG)

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[skirt] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
