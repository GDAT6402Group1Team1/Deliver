# -*- coding: utf-8 -*-
"""读出关卡里各类交通 actor 的 Box 实际尺寸（含 actor 缩放）。

用法：py check_boxes.py
"""

import traceback

import unreal

OUT = unreal.Paths.project_saved_dir() + "boxes.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def box_of(a):
    for c in a.get_components_by_class(unreal.BoxComponent):
        try:
            ext = c.get_unscaled_box_extent()
        except Exception as exc:
            return "读取失败: %s" % str(exc)[:50]
        sc = a.get_actor_scale3d()
        try:
            rl = c.get_editor_property("relative_location")
            rloc = "(%.0f,%.0f,%.0f)" % (rl.x, rl.y, rl.z)
        except Exception:
            rloc = "?"
        return ("半extent(%.0f,%.0f,%.0f) × actor缩放(%.2f,%.2f,%.2f)"
                "  ->  实际 %.0f × %.0f × %.0f cm   相对位置 %s"
                % (ext.x, ext.y, ext.z, sc.x, sc.y, sc.z,
                   ext.x * sc.x * 2, ext.y * sc.y * 2, ext.z * sc.z * 2, rloc))
    return "没有 BoxComponent"


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    # 蓝图默认值（临时生成实例读，读完即删）
    w("=== 蓝图默认 ===")
    for path in ("/Game/PS2DEM/BP_TrafficLine1",
                 "/Game/PS2DEM/BP_TrafficLine1_IntersectionChild",
                 "/Game/PS2DEM/BP_Intersection"):
        try:
            gen = unreal.EditorAssetLibrary.load_asset(path).generated_class()
            tmp = eas.spawn_actor_from_class(gen, unreal.Vector(0, 0, -300000))
            w("  %-46s %s" % (path.split("/")[-1], box_of(tmp)))
            eas.destroy_actor(tmp)
        except Exception as exc:
            w("  %-46s 失败 %s" % (path.split("/")[-1], str(exc)[:60]))

    # 关卡里实际生成的（每类抽一个样例）
    w("")
    w("=== 关卡里的实例（每类抽一个）===")
    seen = {}
    for a in actors:
        lbl = a.get_actor_label()
        for pre in ("Lane_", "Inter_", "Intersection_"):
            if lbl.startswith(pre) and pre not in seen:
                seen[pre] = a
    for pre in ("Lane_", "Inter_", "Intersection_"):
        a = seen.get(pre)
        if a is None:
            w("  %-14s （关卡里没有）" % pre)
        else:
            w("  %-14s %-26s %s" % (pre, a.get_actor_label()[:26], box_of(a)))

    # Intersection_ 的尺寸各不相同，全部列一遍
    w("")
    w("=== 全部 Intersection_（尺寸逐个不同）===")
    for a in sorted((x for x in actors if x.get_actor_label().startswith("Intersection_")),
                    key=lambda x: x.get_actor_label()):
        w("  %-20s %s" % (a.get_actor_label(), box_of(a)))

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    unreal.log("[boxes] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[boxes] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
