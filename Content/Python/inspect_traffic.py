# -*- coding: utf-8 -*-
"""第三轮：确定 Box 是否跟随样条 + 量出路面实际宽度。

决定性实验：默认 BoxComponent 的 extent 是 (32,32,32)。
如果生成的实例 Box 还是 32，说明构造脚本没碰它 -> Box 是固定的，
那么长样条方案不成立，必须沿路铺一串短段。
再把样条拉长重跑构造脚本，看 Box 跟不跟着变。
"""

import traceback

import unreal

ROAD_LABEL = "形状 13"
LANE_BP = "/Game/PS2DEM/BP_TrafficLine1"
OUT = unreal.Paths.project_saved_dir() + "traffic_inspect.txt"
WS = unreal.SplineCoordinateSpace.WORLD

lines = []


def w(s=""):
    lines.append(str(s))


def box_info(actor, tag):
    for c in actor.get_components_by_class(unreal.BoxComponent):
        vals = {}
        for key in ("box_extent", "relative_location", "relative_scale3d"):
            try:
                vals[key] = c.get_editor_property(key)
            except Exception as exc:
                vals[key] = "?(%s)" % exc
        w("  [%s] %s" % (tag, c.get_name()))
        for k, v in vals.items():
            w("        %-18s = %s" % (k, v))
        return vals.get("box_extent")
    w("  [%s] 没有 BoxComponent" % tag)
    return None


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    # ---- 决定性实验：Box 跟不跟样条 ----
    w("=" * 72)
    w("1. Box 是否跟随样条（决定长样条 vs 短段链）")
    w("=" * 72)
    try:
        gen = unreal.EditorAssetLibrary.load_asset(LANE_BP).generated_class()
        tmp = eas.spawn_actor_from_class(gen, unreal.Vector(0, 0, -200000))
        before = box_info(tmp, "默认 2 点 / 长 100")

        sp = tmp.get_component_by_class(unreal.SplineComponent)
        if sp:
            sp.clear_spline_points(False)
            for pt in ((0, 0, 0), (1500, 0, 0), (3000, 600, 0)):
                sp.add_spline_point(unreal.Vector(*pt), unreal.SplineCoordinateSpace.LOCAL, False)
            sp.update_spline()
            w("  已把样条改成 3 点 / 长约 %.0f" % sp.get_spline_length())
        try:
            tmp.rerun_construction_scripts()
            w("  已重跑构造脚本")
        except Exception as exc:
            w("  (rerun_construction_scripts 不可用: %s)" % exc)
        after = box_info(tmp, "改成 3 点 / 长 ~3300")

        w("")
        if before is not None and after is not None:
            same = (abs(before.x - after.x) < 0.01 and abs(before.y - after.y) < 0.01)
            w("  >>> 结论: Box %s" % ("**没有**跟随样条（固定尺寸）→ 必须铺短段链"
                                     if same else "**跟随**样条 → 可以用一条长样条"))
        eas.destroy_actor(tmp)
    except Exception:
        w(traceback.format_exc())

    # ---- 量路宽：找 形状13 附近的静态网格体 ----
    w("")
    w("=" * 72)
    w("2. 路面宽度：形状 13 附近的 StaticMeshActor")
    w("=" * 72)
    road = None
    for a in actors:
        if a.get_actor_label().strip() == ROAD_LABEL:
            road = a
            break
    if road is None:
        w("没找到 形状 13")
    else:
        sp = road.get_component_by_class(unreal.SplineComponent)
        probe = sp.get_location_at_distance_along_spline(sp.get_spline_length() * 0.5, WS)
        w("样条中点 (%.0f, %.0f, %.0f) 附近 3000cm 内的 actor:" % (probe.x, probe.y, probe.z))
        near = []
        for a in actors:
            d = (a.get_actor_location() - probe).length()
            if d < 3000:
                near.append((d, a))
        near.sort(key=lambda t: t[0])
        for d, a in near[:12]:
            cls = a.get_class().get_name()
            w("  %7.0fcm  %-26s %s" % (d, a.get_actor_label()[:26], cls))
            for smc in a.get_components_by_class(unreal.StaticMeshComponent):
                try:
                    m = smc.static_mesh
                    if m:
                        b = m.get_bounds().box_extent
                        s = smc.get_editor_property("relative_scale3d")
                        w("        mesh %-24s 半尺寸(%.0f,%.0f,%.0f) scale(%.2f,%.2f,%.2f) → 实宽 %.0f"
                          % (m.get_name()[:24], b.x, b.y, b.z, s.x, s.y, s.z, b.y * 2 * s.y))
                except Exception:
                    pass
        w("")
        w("样条点 Scale 全是 (1, 10, 1) —— 若底模宽 100cm 则路宽 1000cm")

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    unreal.log("[inspect_traffic] 写入 %s" % OUT)


_state = {"ticks": 0, "handle": None}


def _wait(_dt):
    _state["ticks"] += 1
    try:
        n = len(unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors())
    except Exception:
        n = 0
    if n > 50 or _state["ticks"] > 10800:
        unreal.unregister_slate_post_tick_callback(_state["handle"])
        unreal.log("[inspect_traffic] 世界就绪(%d actor)" % n)
        try:
            run()
        except Exception:
            unreal.log_error("[inspect_traffic] 失败:" + chr(10) + traceback.format_exc())


_state["handle"] = unreal.register_slate_post_tick_callback(_wait)
unreal.log("[inspect_traffic] 已挂载")
