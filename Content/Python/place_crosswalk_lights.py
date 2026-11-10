# -*- coding: utf-8 -*-
u"""在每条斑马线两端各摆一盏人行红绿灯，相位和车灯相反。

规则（用户定的）
----------------
* **只摆有斑马线的地方**，灯位以**斑马线自己的横向范围**为基准、两端各往外延伸
  `EXTEND`。不再去量路缘——斑马线本来就是按双向全宽生成的，
  路宽这个事实只存一处。实测跨度 1080~1834 cm 逐条不同，用它自己的范围天然对。
* **取网格的实际范围，不是"锚点 ± 半跨度"**：锚点落在 ±180 的车道上、
  实测偏心 +173~+372 cm，按锚点对称摆会把一侧摆进路面里。
* 两盏**面对面**，颜色**和这条斑马线横穿的那条路的车灯相反**——
  车绿则人红。相位由"用哪个类"决定（起始色存在 CDO 上，实例写不进去），
  所以这里只是把 `gen_traffic_lights.py` 的 `use_green` 判据**取反**：
  压着组 0（车先绿）的用父类（先红），压着组 1（车先红）的用绿起始子类。
* 同一条斑马线那两盏**必须同色**：同一个类、同一帧生成、同一条 Delay 链，
  所以天然同步。和车灯之间仍会慢慢漂（灯和路口是两个独立定时器，
  CLAUDE.md「已知会漂」那条），这是选"纯时间"时接受的代价。

归属判据抄 `sync_crosswalk_tags.py`（段入口后方 150~800、横向 ≤1100、同向），
**组不唯一或匹配不到段的斑马线一律跳过并点名**——"和谁相反"定不下来时
摆上去只会是错的，而那种错在视口里看不出来。

幂等：按自己的 tag 先清后摆，重跑不会越摆越多。
**注意 `remove_center_traffic_lights.py` 也会删掉这些**（它按类名前缀
`trafficlight` 清），那是预期的清理路径。

无界面：
    UnrealEditor-Cmd <工程>.uproject -run=pythonscript \
        -script=<本文件绝对路径> -unattended -nopause -nosplash -NullRHI
"""
import json
import math
import re

import unreal

DRY_RUN = False
LEVEL = u"/Game/Level/TestForCharacter"
SURFACE_TAG = u"DeliveryCrosswalkSurfaceV2"
INTER = re.compile(r'^Inter_(.+)_([+-]\d+)_I\d+$')
BEHIND_MIN, BEHIND_MAX, LATERAL_MAX = 150.0, 800.0, 1100.0

LIGHT_BP = u"/Game/trafficlight/trafficlight"                 # 起始红
GREEN_START_BP = u"/Game/trafficlight/trafficlight_green_start"  # 起始绿
GREEN_FIRST_GROUP = 0   # 车灯那边：组 0 先绿。必须和 gen_traffic_lights.py 一致

EXTEND = 150.0          # 从斑马线横向末端再往外延伸多少（马路外）
MODEL_YAW_OFFSET = 90.0 # 灯头/SpotLight 面向局部 −Y，和 gen_traffic_lights.py 同一个数
GROUND_PROBE_UP = 400.0 # 探地射线从灯位上方多高开始
GROUND_PROBE_DOWN = 600.0
TAG = u"ClaudeGenCrosswalkLight"

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + u"place_crosswalk_lights.json"
_lines = []


def w(s=u""):
    _lines.append(s)
    unreal.log(u"[xlight] %s" % s)


def entry_and_dir(actor):
    u"""段入口点和行驶方向；两个采样点作差，端点切线在本工程会退化成零向量"""
    main = None
    splines = actor.get_components_by_class(unreal.SplineComponent)
    for sp in splines:
        if sp.get_name() == u"Spline":
            main = sp
            break
    if main is None and splines:
        main = splines[0]
    if main is None or main.get_number_of_spline_points() < 2:
        return None
    length = main.get_spline_length()
    if length <= 1.0:
        return None
    p0 = main.get_location_at_distance_along_spline(0.0, WS)
    p1 = main.get_location_at_distance_along_spline(min(200.0, length), WS)
    dx, dy = p1.x - p0.x, p1.y - p0.y
    n = math.hypot(dx, dy)
    if n < 0.01:
        return None
    return p0, (dx / n, dy / n)


def lateral_span(actor, lat):
    u"""斑马线网格沿 lat 方向的真实范围（投影 8 个包围盒角点）

    用网格的局部包围盒而不是 `get_actor_bounds`：后者是世界轴对齐的，
    斜着的路会把范围放大一截。
    """
    comps = [c for c in actor.get_components_by_class(unreal.StaticMeshComponent)
             if c.static_mesh]
    lo, hi = None, None
    for c in comps:
        box = c.static_mesh.get_bounding_box()
        xf = c.get_world_transform()
        for sx in (box.min.x, box.max.x):
            for sy in (box.min.y, box.max.y):
                for sz in (box.min.z, box.max.z):
                    p = xf.transform_location(unreal.Vector(sx, sy, sz))
                    d = p.x * lat[0] + p.y * lat[1]
                    lo = d if lo is None else min(lo, d)
                    hi = d if hi is None else max(hi, d)
    if lo is None:
        return None
    return lo, hi


def ground_z(world, x, y, z_hint):
    u"""探地。commandlet 里常年不命中（没有完整物理场景），所以必须有兜底"""
    try:
        start = unreal.Vector(x, y, z_hint + GROUND_PROBE_UP)
        end = unreal.Vector(x, y, z_hint - GROUND_PROBE_DOWN)
        hit = unreal.SystemLibrary.line_trace_single(
            world, start, end, unreal.TraceTypeQuery.TRACE_TYPE_CAMERA,
            False, [], unreal.DrawDebugTrace.NONE, True,
            unreal.LinearColor.RED, unreal.LinearColor.GREEN, 0.0)
        if hit:
            return hit.to_tuple()[4].z, True
    except Exception:
        pass
    return z_hint, False


def run():
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not les.load_level(LEVEL):
        w(u"！加载不了 %s" % LEVEL)
        return
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.EditorLevelLibrary.get_editor_world()
    actors = [a for a in eas.get_all_level_actors() if a]

    red_class = unreal.EditorAssetLibrary.load_blueprint_class(LIGHT_BP)
    green_class = unreal.EditorAssetLibrary.load_blueprint_class(GREEN_START_BP)
    if red_class is None or green_class is None:
        w(u"！灯的生成类加载不到（红 %s / 绿 %s）——先跑 make_light_variant.py"
          % (red_class is not None, green_class is not None))
        return

    marks = [a for a in actors
             if a.get_actor_label().startswith(u"Crosswalk_")
             or SURFACE_TAG in [str(t) for t in a.tags]]
    if not marks:
        w(u"！一条斑马线都没有，不摆任何灯（多半是加载错了关卡）")
        return

    segs = []
    for a in actors:
        if not INTER.match(a.get_actor_label()):
            continue
        ed = entry_and_dir(a)
        if ed is None:
            continue
        try:
            ln = a.get_editor_property(u"LightNumber")
        except Exception:
            ln = None
        segs.append((a.get_actor_label(), ed[0], ed[1], ln))

    # 先清掉自己摆过的
    old = [a for a in actors if TAG in [str(t) for t in a.tags]]
    w(u"=== 在斑马线两端摆人行灯 ===")
    w(u"DRY_RUN = %s，EXTEND = %.0f cm" % (DRY_RUN, EXTEND))
    w(u"斑马线 %d 条、Inter_ 段 %d 条、本脚本旧灯 %d 盏" % (len(marks), len(segs), len(old)))
    if old and not DRY_RUN:
        for a in old:
            eas.destroy_actor(a)
        w(u"已清掉旧的 %d 盏" % len(old))

    placed, report, skipped = 0, [], []
    for m in sorted(marks, key=lambda a: a.get_actor_label()):
        label = m.get_actor_label()
        loc = m.get_actor_location()
        fv = m.get_actor_forward_vector()
        lat = (-fv.y, fv.x)            # 横向轴 = 行驶方向左转 90°
        n = math.hypot(lat[0], lat[1])
        if n < 0.5:
            skipped.append((label, u"行驶方向不是水平的"))
            continue
        lat = (lat[0] / n, lat[1] / n)

        # 这条斑马线压着哪些段 → 灯组必须唯一
        groups = set()
        matched = []
        for slabel, entry, (fx, fy), ln in segs:
            dx, dy = loc.x - entry.x, loc.y - entry.y
            along = dx * fx + dy * fy
            if along >= 0.0:
                continue
            behind = -along
            lateral = abs(-dx * fy + dy * fx)
            if BEHIND_MIN <= behind <= BEHIND_MAX and lateral <= LATERAL_MAX:
                matched.append(slabel)
                groups.add(ln)
        if not matched:
            skipped.append((label, u"后方没有任何路口段，定不出该和谁相反"))
            continue
        if len(groups) != 1 or None in groups:
            skipped.append((label, u"灯组不唯一或读不到 LightNumber：%s" % sorted(
                [str(g) for g in groups])))
            continue
        group = groups.pop()

        span = lateral_span(m, lat)
        if span is None:
            skipped.append((label, u"网格取不到横向范围"))
            continue
        lo, hi = span

        # 车灯：组 0 先绿。人行灯要相反
        vehicle_green = (group % 2) == (GREEN_FIRST_GROUP % 2)
        cls = red_class if vehicle_green else green_class
        ped = u"红" if vehicle_green else u"绿"

        base = loc.x * lat[0] + loc.y * lat[1]
        pair = []
        for sign, edge in ((-1.0, lo - EXTEND), (1.0, hi + EXTEND)):
            px = loc.x + lat[0] * (edge - base)
            py = loc.y + lat[1] * (edge - base)
            pz, probed = ground_z(world, px, py, loc.z)
            # 面对面：朝向指向对面那一盏，也就是 −sign 那一侧
            face = (-sign * lat[0], -sign * lat[1])
            yaw = math.degrees(math.atan2(face[1], face[0])) + MODEL_YAW_OFFSET
            pair.append(dict(side=u"A" if sign < 0 else u"B",
                             loc=[px, py, pz], yaw=yaw, probed=probed))
            if DRY_RUN:
                continue
            # Rotator 是 (roll, pitch, yaw)，和 C++ 的 FRotator 顺序不同；
            # 填错位置会把 yaw 当成 pitch，灯直接躺下
            a = eas.spawn_actor_from_class(
                cls, unreal.Vector(px, py, pz), unreal.Rotator(0.0, 0.0, yaw))
            if a is None:
                skipped.append((label, u"生成失败"))
                continue
            a.set_actor_label(u"PedLight_%s_%s" % (label[10:], u"A" if sign < 0 else u"B"))
            a.tags = [unreal.Name(TAG), unreal.Name(u"%s:%s" % (TAG, group))]
            placed += 1

        report.append(dict(crosswalk=label, group=group, pedestrian_start=ped,
                           segments=matched, span=[lo, hi],
                           width=hi - lo, posts=pair))
        w(u"%-34s 组%s 车%s→人%s 跨度%.0f 灯距%.0f  yaw %.0f/%.0f"
          % (label, group, u"绿" if vehicle_green else u"红", ped,
             hi - lo, (hi - lo) + 2 * EXTEND, pair[0]['yaw'], pair[1]['yaw']))

    w(u"")
    w(u"跳过 %d 条：" % len(skipped))
    for label, why in skipped:
        w(u"  - %-34s %s" % (label, why))

    probed = sum(1 for r in report for p in r['posts'] if p['probed'])
    w(u"")
    w(u"摆了 %d 盏（%d 条斑马线 × 2），其中 %d 盏的高度是探地测的、"
      u"其余沿用斑马线自己的 Z" % (placed, len(report), probed))
    if probed == 0 and report:
        w(u"！一盏都没探到地面——commandlet 里没有完整物理场景，这是预期的；"
          u"灯脚高度等于路面高度，站在路肩上可能略低，要在编辑器里看一眼")

    with open(OUT, u"w") as f:
        json.dump(dict(level=LEVEL, dry_run=DRY_RUN, extend=EXTEND,
                       green_first_group=GREEN_FIRST_GROUP, placed=placed,
                       skipped=[dict(crosswalk=a, reason=b) for a, b in skipped],
                       detail=report), f, ensure_ascii=False, indent=1)
    w(u"明细写到 %s" % OUT)

    if DRY_RUN:
        w(u"DRY_RUN：什么都没摆、没保存。")
        return
    if placed:
        les.save_current_level()
        w(u"已保存关卡（只新增了灯 actor，没动交通线/斑马线/tag）。")


run()
