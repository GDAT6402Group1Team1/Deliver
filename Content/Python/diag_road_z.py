# -*- coding: utf-8 -*-
"""沿 形状13 打射线，对比样条 Z 与真实路面 Z，并报出路面是什么东西。

UE5.8 的 HitResult 属性是 protected，只能用 to_dict() 取值 —— 前两版栽在这上面。
line_trace_single 直接返回 HitResult（不是 tuple）。
"""

import traceback

import unreal

ROAD_LABEL = "形状 13"
STEP = 1000.0
UP = 60000.0
DOWN = 60000.0
OUT = unreal.Paths.project_saved_dir() + "diag_road_z.txt"
WS = unreal.SplineCoordinateSpace.WORLD

lines = []
_dumped = [False]


def w(s=""):
    lines.append(str(s))


def pick(d, *names):
    for n in names:
        if n in d:
            return d[n]
    return None


def shoot(world, x, y, z):
    """返回 (命中Z, 命中对象名, 原始dict)。"""
    start = unreal.Vector(x, y, z + UP)
    end = unreal.Vector(x, y, z - DOWN)
    hit = unreal.SystemLibrary.line_trace_single(
        world, start, end, unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
        False, [], unreal.DrawDebugTrace.NONE, True)
    if hit is None:
        return None, "无返回", None
    try:
        d = hit.to_dict()
    except Exception as exc:
        return None, "to_dict失败:%s" % exc, None

    if not _dumped[0]:
        _dumped[0] = True
        w("--- HitResult.to_dict() 的键（只打印一次）---")
        for k in sorted(d.keys()):
            w("    %-22s = %s" % (k, str(d[k])[:90]))
        w("-" * 60)
        w("")

    blocking = pick(d, "blocking_hit", "b_blocking_hit", "BlockingHit")
    if blocking is False:
        return None, "未命中", d
    loc = pick(d, "impact_point", "ImpactPoint", "location", "Location")
    who = "?"
    for key in ("hit_object_handle", "actor", "Actor", "component", "Component"):
        if key in d and d[key] is not None:
            try:
                o = d[key]
                who = o.get_name() if hasattr(o, "get_name") else str(o)[:40]
                break
            except Exception:
                pass
    if loc is None:
        return None, "dict里没有落点(%s)" % who, d
    return loc.z, who, d


def run():
    ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    world = ues.get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    road = None
    for a in eas.get_all_level_actors():
        if a.get_actor_label().strip() == ROAD_LABEL:
            road = a
            break
    if road is None:
        w("找不到 %s" % ROAD_LABEL)
        return
    sp = road.get_component_by_class(unreal.SplineComponent)
    total = sp.get_spline_length()
    w("道路样条 %s  总长 %.0f cm  采样间距 %.0f" % (ROAD_LABEL, total, STEP))
    w("")

    rows = []
    d = 0.0
    while d <= total:
        p = sp.get_location_at_distance_along_spline(d, WS)
        gz, who, _ = shoot(world, p.x, p.y, p.z)
        rows.append((d, p.z, gz, who))
        d += STEP

    w("%8s %10s %10s %9s  %s" % ("沿线距离", "样条Z", "路面Z", "差值", "命中对象"))
    w("%8s %10s %10s %9s  %s" % ("-" * 8, "-" * 10, "-" * 10, "-" * 9, "-" * 26))
    hits = 0
    buried = []
    for dist, sz, gz, who in rows:
        if gz is None:
            w("%8.0f %10.1f %10s %9s  %s" % (dist, sz, "-", "-", who))
        else:
            hits += 1
            diff = sz - gz
            flag = "   <<< 样条在路面下" if diff < -50 else ("   << 高出路面" if diff > 300 else "")
            w("%8.0f %10.1f %10.1f %9.1f  %-26s%s" % (dist, sz, gz, diff, who[:26], flag))
            if diff < -50:
                buried.append((dist, diff, who))

    w("")
    w("=" * 64)
    w("采样 %d 点，命中 %d 点" % (len(rows), hits))
    if hits == 0:
        w("!! 依然全部未命中，射线这条路走不通")
    elif buried:
        worst = min(buried, key=lambda t: t[1])
        w("低于路面 50cm 以上的点: %d 个" % len(buried))
        w("最深: 沿线 %.0f cm 处低 %.0f cm（命中 %s）" % (worst[0], abs(worst[1]), worst[2]))
        w("埋没区间:")
        s = prev = buried[0][0]
        for dd, _, _ in buried[1:]:
            if dd - prev > STEP * 1.5:
                w("   %.0f ~ %.0f cm" % (s, prev))
                s = dd
            prev = dd
        w("   %.0f ~ %.0f cm" % (s, prev))
    else:
        w("没有点低于路面 —— 样条本身是贴合的")

    # 命中对象种类统计：判断路面是地形还是方块
    w("")
    kinds = {}
    for _, _, gz, who in rows:
        if gz is not None:
            kinds[who] = kinds.get(who, 0) + 1
    w("命中对象分布（判断路面是地形还是方块）:")
    for k in sorted(kinds, key=lambda x: -kinds[x]):
        w("   %-34s %d 次" % (k, kinds[k]))

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    unreal.log("[diag_road_z] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[diag_road_z] 失败:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("失败:\n" + err)
