# -*- coding: utf-8 -*-
"""删掉指定道路的车道 + 路口段。

按 tag **精确**匹配（不是前缀）："ClaudeGenLane:形状 43"。
前缀匹配在这里不安全——关卡里同时有 形状 4 / 形状 42 / 形状 43 / 形状 44，
用 "ClaudeGenLane:形状 4" 做前缀会把后面三条一起带走。

只删属于这几条路的段。别的路在这个路口的路口段不动——
那些段带的是**别的路**的 tag，删了会在那条路的链条上开个洞。

生成器那边也要同步加 EXCLUDE_ROADS，否则下次重新生成又会长回来。
用法：py del_road_lanes.py
"""

import traceback

import unreal

ROADS = ["形状 43"]
TAG_PREFIX = "ClaudeGenLane"

OUT = unreal.Paths.project_saved_dir() + "del_road_lanes.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[del] %s" % s)


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    want = set("%s:%s" % (TAG_PREFIX, r.strip()) for r in ROADS)
    w("要删的 tag: %s" % ", ".join(sorted(want)))

    doomed = []
    for a in eas.get_all_level_actors():
        if want & set(str(t) for t in a.tags):
            doomed.append(a)

    if not doomed:
        w("没找到任何匹配的 actor。")
        w("关卡里现有的 ClaudeGenLane tag：")
        seen = set()
        for a in eas.get_all_level_actors():
            for t in a.tags:
                t = str(t)
                if t.startswith(TAG_PREFIX):
                    seen.add(t)
        for t in sorted(seen):
            w("   %s" % t)
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
        return

    by_kind = {}
    for a in doomed:
        k = a.get_actor_label().split("_")[0]
        by_kind[k] = by_kind.get(k, 0) + 1
    w("命中 %d 个 actor：" % len(doomed))
    for k in sorted(by_kind, key=lambda x: -by_kind[x]):
        w("   %-10s %d 个" % (k, by_kind[k]))
    w("")
    w("清单：")
    for a in sorted(doomed, key=lambda x: x.get_actor_label()):
        w("   %s" % a.get_actor_label())

    for a in doomed:
        eas.destroy_actor(a)
    w("")
    w("已删除 %d 个。" % len(doomed))
    w("别的路在这些路口的路口段没有动（它们带的是别的路的 tag）。")
    w("生成器里已加 EXCLUDE_ROADS，重新生成不会再长回来。")
    w("接下来建议重跑 gen_intersections.py —— 少了这些路口段，路口盒要重算。")
    w("关卡尚未保存。")

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[del] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
