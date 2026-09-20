# -*- coding: utf-8 -*-
"""查路面当前的状态，定位"边缘被埋的问题又出现了"是什么变了。

今天早些时候埋没问题是靠三件事一起压下去的：
    压平地形 (deform_terrain.py，目标 路面顶 -40)
    路面加厚 (thicken_road.py，6 -> 26cm)
    绿色裙边 (gen_road_skirt.py)
其中加厚和裙边都可能被别的操作冲掉——路面片是地形样条系统自动生成的，
样条一重新生成，改过的缩放就回默认值。CLAUDE.md 里这条一直标着"未验证"。

这个脚本回答：现在路面多厚？裙边还在吗？有没有被藏起来的路面片？
只读。
用法：py check_road_state.py
"""

import traceback

import unreal

MESH_HEIGHT = 100.0
EXPECT_THICKNESS = 26.0
SKIRT_TAG = "ClaudeGenSkirt"

OUT = unreal.Paths.project_saved_dir() + "road_state.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    land = None
    for a in actors:
        if a.get_class().get_name() == "Landscape":
            land = a
            break
    if land is None:
        w("!! 找不到 Landscape")
        flush()
        return

    sms = list(land.get_components_by_class(unreal.SplineMeshComponent))
    w("=" * 74)
    w("1. 路面厚度（期望 %.0fcm，默认值 6cm）" % EXPECT_THICKNESS)
    w("=" * 74)
    w("  路面片总数 %d" % len(sms))

    buckets, hidden, nocoll, unread = {}, 0, 0, 0
    for c in sms:
        try:
            p = c.get_editor_property("spline_params")
            ss = p.get_editor_property("start_scale")
            t = round(ss.y * MESH_HEIGHT, 1)
            buckets[t] = buckets.get(t, 0) + 1
        except Exception:
            unread += 1
        try:
            if not c.is_visible():
                hidden += 1
        except Exception:
            pass
        try:
            if str(c.get_collision_enabled()).endswith("NO_COLLISION"):
                nocoll += 1
        except Exception:
            pass

    for t in sorted(buckets, key=lambda x: -buckets[x]):
        mark = ""
        if abs(t - EXPECT_THICKNESS) < 0.5:
            mark = "  <- 期望值"
        elif t < 10:
            mark = "  <- 回到默认了，加厚被冲掉"
        w("    厚度 %6.1f cm   %d 片%s" % (t, buckets[t], mark))
    if unread:
        w("    读不到 %d 片" % unread)
    w("")
    w("  被隐藏的 %d 片，关了碰撞的 %d 片" % (hidden, nocoll))
    if hidden or nocoll:
        w("  >>> 有路面片被藏起来了。flatten_crossings.py 会做这件事，")
        w("      但它默认 DRY_RUN=True；如果这个数不是 0，说明它以 DRY_RUN=False 跑过。")
    w("")
    flush()

    w("=" * 74)
    w("2. 绿色裙边")
    w("=" * 74)
    skirts = [a for a in actors if SKIRT_TAG in [str(t) for t in a.tags]]
    w("  裙边 actor %d 个" % len(skirts))
    if not skirts:
        w("  >>> 裙边没了。gen_road_skirt.py 生成的 actor 全不在了，")
        w("      这本身就能让路缘看起来重新陷进地里。")
    w("")

    w("=" * 74)
    w("3. 结论指引")
    w("=" * 74)
    thin = sum(n for t, n in buckets.items() if t < 10)
    if thin > len(sms) * 0.5:
        w("  路面大面积回到默认厚度 -> 地形样条重新生成过，加厚被冲掉。")
        w("  重跑 thicken_road.py 即可；裙边若也没了，再跑 gen_road_skirt.py。")
    elif not skirts:
        w("  厚度还在但裙边没了 -> 先重跑 gen_road_skirt.py。")
    else:
        w("  厚度和裙边都还在 -> 埋没的原因不是这两个，")
        w("  接着看 diag_road_buried.py 的数字，和今天早些时候的比。")
    w("")
    w("只读脚本，没有改动任何东西。")
    flush()
    unreal.log("[roadstate] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[roadstate] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
