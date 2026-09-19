# -*- coding: utf-8 -*-
"""查清灰色马路是怎么渲染出来的。

已排除：不是 StaticMeshActor（那 140 个 A_/B_/C_ 方块是建筑），
形状13 自己只有一个样条组件、不渲染任何东西。
剩下的可能：地形材质刷的 / ISM·HISM 实例 / 别的 actor 类型。
"""

import traceback

import unreal

OUT = unreal.Paths.project_saved_dir() + "find_road.txt"
WS = unreal.SplineCoordinateSpace.WORLD
lines = []


def w(s=""):
    lines.append(str(s))


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    # ---- 1. actor 类别统计 ----
    w("=" * 68)
    w("1. 关卡 actor 类别统计（共 %d 个）" % len(actors))
    w("=" * 68)
    counts = {}
    for a in actors:
        k = a.get_class().get_name()
        counts[k] = counts.get(k, 0) + 1
    for k in sorted(counts, key=lambda x: -counts[x]):
        w("  %-42s %d" % (k, counts[k]))

    # ---- 2. 实例化网格（ISM / HISM）----
    w("")
    w("=" * 68)
    w("2. 实例化静态网格 ISM / HISM")
    w("=" * 68)
    found = False
    for a in actors:
        for c in a.get_components_by_class(unreal.InstancedStaticMeshComponent):
            found = True
            try:
                m = c.static_mesh
                w("  %-30s %-34s 实例数 %d  mesh=%s"
                  % (a.get_actor_label()[:30], type(c).__name__,
                     c.get_instance_count(), m.get_name() if m else None))
            except Exception as exc:
                w("  %-30s (读取失败 %s)" % (a.get_actor_label()[:30], exc))
    if not found:
        w("  没有任何 ISM/HISM 组件")

    # ---- 3. 形状13 中点附近所有带渲染的组件 ----
    w("")
    w("=" * 68)
    w("3. 形状13 中点附近 4000cm 内、所有会渲染的组件")
    w("=" * 68)
    road = None
    for a in actors:
        if a.get_actor_label().strip() == "形状 13":
            road = a
            break
    if road is None:
        w("  找不到 形状 13")
    else:
        sp = road.get_component_by_class(unreal.SplineComponent)
        p = sp.get_location_at_distance_along_spline(sp.get_spline_length() * 0.5, WS)
        w("  参考点 (%.0f, %.0f, %.0f)" % (p.x, p.y, p.z))
        near = []
        for a in actors:
            d = (a.get_actor_location() - p).length()
            if d < 4000:
                near.append((d, a))
        near.sort(key=lambda t: t[0])
        for d, a in near[:15]:
            w("  %7.0fcm  %-24s %s" % (d, a.get_actor_label()[:24], a.get_class().get_name()))
            for c in a.get_components_by_class(unreal.PrimitiveComponent):
                try:
                    extra = ""
                    if isinstance(c, unreal.StaticMeshComponent) and c.static_mesh:
                        extra = " mesh=%s" % c.static_mesh.get_name()
                    w("        %-34s %s%s" % (type(c).__name__, c.get_name()[:22], extra))
                except Exception:
                    pass

    # ---- 4. 地形信息 ----
    w("")
    w("=" * 68)
    w("4. Landscape")
    w("=" * 68)
    for a in actors:
        cn = a.get_class().get_name()
        if "Landscape" in cn:
            w("  %-28s %s" % (a.get_actor_label()[:28], cn))
            try:
                o, e = a.get_actor_bounds(False)
                w("      中心 (%.0f, %.0f, %.0f)  半尺寸 (%.0f, %.0f, %.0f)"
                  % (o.x, o.y, o.z, e.x, e.y, e.z))
            except Exception:
                pass

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    unreal.log("[find_road] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[find_road] 失败:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("失败:\n" + err)
