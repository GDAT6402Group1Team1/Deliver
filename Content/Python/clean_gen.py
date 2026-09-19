# -*- coding: utf-8 -*-
"""删除脚本生成的 actor。默认只删转弯道。

改 TAGS 可以删别的：
    ClaudeGenTurn            转弯道
    ClaudeGenIntersection    BP_Intersection 盒子
    ClaudeGenLane            路段 + 路口段（前缀匹配，含 "ClaudeGenLane:形状 13" 等）
"""

import traceback

import unreal

TAGS = ["ClaudeGenTurn"]

OUT = unreal.Paths.project_saved_dir() + "clean_gen.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[clean_gen] %s" % s)


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    doomed = []
    for a in actors:
        tags = [str(t) for t in a.tags]
        # 前缀匹配，这样 "ClaudeGenLane" 能命中 "ClaudeGenLane:形状 13"
        if any(t.startswith(p) for t in tags for p in TAGS):
            doomed.append(a)

    w("目标 tag: %s" % ", ".join(TAGS))
    w("命中 %d 个 actor" % len(doomed))
    by_kind = {}
    for a in doomed:
        k = a.get_actor_label().split("_")[0]
        by_kind[k] = by_kind.get(k, 0) + 1
    for k in sorted(by_kind, key=lambda x: -by_kind[x]):
        w("   %-16s %d" % (k, by_kind[k]))

    for a in doomed:
        eas.destroy_actor(a)
    w("已删除 %d 个。关卡尚未保存。" % len(doomed))

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[clean_gen] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
