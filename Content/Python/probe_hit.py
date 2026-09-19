# -*- coding: utf-8 -*-
"""最小探针：摸清 UE5.8 Python 里 line_trace 的返回结构，别再猜属性名。"""

import traceback

import unreal

OUT = unreal.Paths.project_saved_dir() + "probe_hit.txt"
lines = []


def w(s=""):
    lines.append(str(s))


try:
    ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    world = ues.get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    # 拿 形状13 中点当探测位置
    road = None
    for a in eas.get_all_level_actors():
        if a.get_actor_label().strip() == "形状 13":
            road = a
            break
    sp = road.get_component_by_class(unreal.SplineComponent)
    p = sp.get_location_at_distance_along_spline(sp.get_spline_length() * 0.5,
                                                 unreal.SplineCoordinateSpace.WORLD)
    start = unreal.Vector(p.x, p.y, p.z + 50000.0)
    end = unreal.Vector(p.x, p.y, p.z - 50000.0)
    w("探测点 (%.0f, %.0f, %.0f)" % (p.x, p.y, p.z))

    res = unreal.SystemLibrary.line_trace_single(
        world, start, end, unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
        False, [], unreal.DrawDebugTrace.NONE, True)

    w("")
    w("返回 type = %s" % type(res).__name__)
    w("返回 repr = %s" % repr(res)[:200])
    w("是 tuple 吗 = %s" % isinstance(res, tuple))

    hit = res[1] if isinstance(res, tuple) and len(res) > 1 else res
    w("")
    w("HitResult 的 dir()（过滤掉下划线开头）:")
    for name in sorted(n for n in dir(hit) if not n.startswith("_")):
        w("    %s" % name)

    w("")
    w("尝试 get_editor_property 常见字段:")
    for k in ("blocking_hit", "initial_overlap", "time", "distance", "location",
              "impact_point", "normal", "impact_normal", "hit_actor", "actor",
              "component", "bone_name", "item", "face_index", "trace_start", "trace_end"):
        try:
            w("    %-16s = %s" % (k, hit.get_editor_property(k)))
        except Exception as exc:
            w("    %-16s ! %s" % (k, str(exc)[:70]))

    w("")
    w("GameplayStatics 系列 helper:")
    for fn in ("break_hit_result",):
        w("    unreal.GameplayStatics.%s 存在 = %s"
          % (fn, hasattr(unreal.GameplayStatics, fn)))
    try:
        out = unreal.GameplayStatics.break_hit_result(hit)
        w("    break_hit_result 返回 %s 个值" % (len(out) if isinstance(out, tuple) else 1))
        w("    %s" % repr(out)[:400])
    except Exception as exc:
        w("    break_hit_result ! %s" % str(exc)[:120])

except Exception:
    w("失败:" + chr(10) + traceback.format_exc())

with open(OUT, "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
unreal.log("[probe_hit] 写入 %s" % OUT)
