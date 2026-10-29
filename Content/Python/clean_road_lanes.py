# -*- coding: utf-8 -*-
"""删掉指定道路的全部交通线（`Lane_*` + `Inter_*`），按 tag 精确清理。

为什么需要它
------------
`gen_traffic_lanes.py` 只清掉**本次名单里那几条路**的 tag
（`ClaudeGenLane:<路名>`）。所以一条路从名单里被移除（进了 EXCLUDE_ROADS）之后，
它已经生成的 actor 会**永远留在关卡里**，没有任何脚本会去动它们。

实测残留（2026-10-29）：`BranchRoad_R020` 的车道被手动删过，但
`Inter_BranchRoad_R020_+0180_I00` 还在，而且被 `Intersection_15` 的盒子罩着，
于是那个路口仍然有两条路的段、看起来像个正常路口。

按 **tag** 删而不是按标签名：标签能被人改，tag 是生成时打的、可靠。
（反过来 `gen_intersections.py` 的路口盒不用在这里管——它每次都先按
`ClaudeGenIntersection` 清掉全部再重建。）

DRY_RUN=True 只列不删（默认）。
用法：py clean_road_lanes.py
"""

import traceback

import unreal

DRY_RUN = False
TAG_PREFIX = "ClaudeGenLane"
ROADS = ["BranchRoad_R020",      # 和 MainRoad_R003 的交叉点（原 Intersection_15）
         "MainRoad_R004",        # 和 MainRoad_R010 的交叉点（原 Intersection_39）
         "MainRoad_R005"]

OUT = unreal.Paths.project_saved_dir() + "clean_road_lanes.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[cleanlane] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    w(u"=== 清理指定道路的交通线 ===")
    w(u"关卡：%s     模式：%s"
      % (world.get_name() if world else u"?", u"只列不删" if DRY_RUN else u"**删除**"))
    w(u"目标路：%s" % u"、".join(ROADS))

    wanted = set(u"%s:%s" % (TAG_PREFIX, r) for r in ROADS)
    hits = []
    for a in eas.get_all_level_actors():
        tags = [str(t) for t in a.tags]
        if any(t in wanted for t in tags):
            hits.append(a)

    w(u"")
    w(u"命中 %d 个 actor：" % len(hits))
    lane_n = inter_n = other_n = 0
    for a in sorted(hits, key=lambda x: x.get_actor_label()):
        lbl = a.get_actor_label()
        if lbl.startswith("Lane_"):
            lane_n += 1
        elif lbl.startswith("Inter_"):
            inter_n += 1
        else:
            other_n += 1
        w(u"    %-46s tags=%s" % (lbl[:46], u",".join(str(t) for t in a.tags)))
    w(u"")
    w(u"其中 Lane_ %d 个、Inter_ %d 个、其他 %d 个" % (lane_n, inter_n, other_n))

    if not hits:
        w(u"")
        w(u"没有残留。要么已经清干净了，要么这条路的 actor 没打 tag")
        w(u"（手摆的交通线没有 tag，本脚本认不出来，只能在 Outliner 里按名字找）。")
        return

    if DRY_RUN:
        w(u"")
        w(u"只列不删。确认上面这些都该删之后，把 DRY_RUN 改 False 再跑。")
        return

    killed = 0
    for a in hits:
        nm = a.get_actor_label()
        try:
            eas.destroy_actor(a)
            killed += 1
        except Exception as exc:
            w(u"！删 %s 失败：%s" % (nm, str(exc)[:60]))
    w(u"")
    w(u"已删 %d 个。**记得 Ctrl+S 保存关卡。**" % killed)
    w(u"接着跑 gen_intersections.py 重建路口盒——这些段一删，")
    w(u"原来罩住它们的盒子就该跟着重算（没有段的地方不再有盒子）。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[cleanlane] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[cleanlane] 写入 %s" % OUT)
