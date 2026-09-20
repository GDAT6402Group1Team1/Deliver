# -*- coding: utf-8 -*-
"""把车道样条线（Lane_* / Inter_*）的 Box 放大到蓝图默认值的 FACTOR 倍。

只动 BP_TrafficLine1 / BP_TrafficLine1_IntersectionChild 的 Box，
**不碰 BP_Intersection**（那个路口盒子是 gen_intersections.py 按路口逐个算的）。

用"蓝图默认 x FACTOR"这个绝对目标，不是"当前值 x FACTOR"——
否则重复跑一次就变 4 倍。所以这个脚本怎么跑都是幂等的。

改的是 box_extent 而不是 actor 缩放：缩放会把 Spline 和 Billboard 一起放大。

用法：py scale_lane_boxes.py
"""

import traceback

import unreal

FACTOR = 2.0
TAG_PREFIX = "ClaudeGenLane"

OUT = unreal.Paths.project_saved_dir() + "scale_lane_boxes.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[scalebox] %s" % s)


def default_extent(eas, cls):
    """生成一个临时实例读蓝图默认 Box 半尺寸，读完销毁。"""
    tmp = eas.spawn_actor_from_class(cls, unreal.Vector(0, 0, -300000))
    ext = None
    for c in tmp.get_components_by_class(unreal.BoxComponent):
        ext = c.get_unscaled_box_extent()
        break
    eas.destroy_actor(tmp)
    return ext


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = [a for a in eas.get_all_level_actors()
              if any(str(t).startswith(TAG_PREFIX) for t in a.tags)]
    w("带 %s 标签的 actor 共 %d 个" % (TAG_PREFIX, len(actors)))
    if not actors:
        w("!! 一个都没有，先跑 gen_traffic_lanes.py")
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
        return

    cache = {}
    done, skipped, failed = 0, 0, 0
    per_class = {}

    for a in actors:
        cls = a.get_class()
        key = cls.get_name()
        if key not in cache:
            try:
                cache[key] = default_extent(eas, cls)
            except Exception as exc:
                cache[key] = None
                w("!! 读 %s 默认 Box 失败: %s" % (key, str(exc)[:70]))
        base = cache[key]
        if base is None:
            failed += 1
            continue

        target = unreal.Vector(base.x * FACTOR, base.y * FACTOR, base.z * FACTOR)
        boxes = a.get_components_by_class(unreal.BoxComponent)
        if not boxes:
            skipped += 1
            continue
        for c in boxes:
            try:
                cur = c.get_unscaled_box_extent()
                if (abs(cur.x - target.x) < 0.01 and abs(cur.y - target.y) < 0.01
                        and abs(cur.z - target.z) < 0.01):
                    skipped += 1
                    continue
                c.modify(True)
                c.set_editor_property("box_extent", target)
                done += 1
                per_class[key] = per_class.get(key, 0) + 1
            except Exception as exc:
                failed += 1
                w("!! %s: %s" % (a.get_actor_label(), str(exc)[:70]))

    w("")
    for key, base in cache.items():
        if base is None:
            continue
        w("%-44s 默认半尺寸 (%.0f, %.0f, %.0f)  ->  (%.0f, %.0f, %.0f)"
          % (key, base.x, base.y, base.z,
             base.x * FACTOR, base.y * FACTOR, base.z * FACTOR))
        w("%-44s 即实际 %.0f x %.0f x %.0f cm  ->  %.0f x %.0f x %.0f cm"
          % ("", base.x * 2, base.y * 2, base.z * 2,
             base.x * 2 * FACTOR, base.y * 2 * FACTOR, base.z * 2 * FACTOR))
    w("")
    for key, n in sorted(per_class.items()):
        w("改了 %4d 个  %s" % (n, key))
    w("")
    w("改动 %d，已是目标值跳过 %d，失败 %d" % (done, skipped, failed))

    # 回读抽查：确认真的写进去了，而不是被构造脚本弹回来
    w("")
    w("回读抽查（每类前 3 个）：")
    seen = {}
    for a in actors:
        key = a.get_class().get_name()
        seen.setdefault(key, [])
        if len(seen[key]) >= 3:
            continue
        for c in a.get_components_by_class(unreal.BoxComponent):
            e = c.get_unscaled_box_extent()
            seen[key].append("   %-30s (%.0f, %.0f, %.0f)"
                             % (a.get_actor_label()[:30], e.x, e.y, e.z))
            break
    for key, rows in sorted(seen.items()):
        w("  %s" % key)
        for r in rows:
            w(r)

    w("")
    w("关卡尚未保存。")
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[scalebox] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
