# -*- coding: utf-8 -*-
"""补做：把车道/路口 actor 的**原点和 Box** 也降 13，和已经降过的样条对齐。

背景
----
lower_lane_splines.py 只把 SplineComponent 的点在**世界空间**挪了 -13，
actor 原点没动，于是挂在 actor 上的 Box（起点探测把手，车靠它接上下一段路）
还停在原高度，比样条高了 13。

为什么不能直接把 actor 往下挪 13
--------------------------------
样条点存的是**局部**坐标。actor 一往下挪，样条跟着再降 13，就变成 -26 了。
用世界坐标写清楚（A=actor 原点，P=样条点，B=box）：

                原始      上一轮之后     目标
    actor        A           A          A-13
    样条         P          P-13        P-13
    box          B           B          B-13

所以这里分两步：**先把样条点在世界空间抬回 +13**（等于恢复它原来的局部偏移），
**再把 actor 整体降 13**。净结果：样条停在现在的位置不动，actor 和 Box 降下来。
Box 不用单独处理——它挂在 actor 上，actor 一动它就跟着走。

护栏
----
挪动 actor 会触发构造脚本重跑，而构造脚本可能把实例的样条点冲回蓝图默认的
2 个点（靠 spline_has_been_edited 标记保护，上一轮报告显示全部设置成功）。
这里在挪之前记下每个组件的点数，挪完回读比对，**对不上就点名**——
CLAUDE.md 里记过这个失败模式，不能只靠"没报错"就认为没事。

只处理带 ClaudeLaneZ2cm 标签的 actor（= 上一轮确实降过样条的那些），
完成后换成 ACTOR_DONE_TAG，重复跑会跳过。

用法：py lower_lane_actors.py      （DRY_RUN=True 只看不改）
跑完 **Ctrl+S**。
"""

import traceback

import unreal

DRY_RUN = False
DELTA_Z = -13.0
SPLINE_DONE_TAG = "ClaudeLaneZ2cm"      # 上一轮给样条打的
ACTOR_DONE_TAG = "ClaudeLaneActorZ2cm"  # 这一轮的

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "lower_lane_actors.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[loweract] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()

    targets, done, no_spline_tag = [], 0, 0
    for a in eas.get_all_level_actors():
        tags = [str(t) for t in a.tags]
        if ACTOR_DONE_TAG in tags:
            done += 1
            continue
        if SPLINE_DONE_TAG in tags:
            targets.append(a)
        elif any(t.startswith("ClaudeGenLane") for t in tags):
            no_spline_tag += 1

    w(u"=== 补降 actor 原点 + Box（%+.0f）===" % DELTA_Z)
    w(u"关卡：%s" % (world.get_name() if world else u"?"))
    w(u"模式：%s" % (u"只看不改（DRY_RUN）" if DRY_RUN else u"**写入**"))
    w(u"待处理 %d 个；已补过 %d 个；带车道标签但样条没降过的 %d 个（跳过，"
      u"先跑 lower_lane_splines.py）" % (len(targets), done, no_spline_tag))
    if not targets:
        w(u"")
        w(u"没有要处理的。")
        return

    shown = 0
    collapsed = []
    box_moved = 0
    for actor in sorted(targets, key=lambda a: a.get_actor_label()):
        sps = actor.get_components_by_class(unreal.SplineComponent)
        before_counts = [(sp.get_name(), sp.get_number_of_spline_points()) for sp in sps]
        a_loc = actor.get_actor_location()
        boxes = actor.get_components_by_class(unreal.BoxComponent)

        if shown < 5:
            bz = boxes[0].get_world_location().z if boxes else None
            p0 = sps[0].get_location_at_spline_point(0, WS).z if sps else None
            w(u"  %-28s actor %.1f → %.1f   box %s   样条首点 %s（不动）"
              % (actor.get_actor_label()[:28], a_loc.z, a_loc.z + DELTA_Z,
                 u"%.1f → %.1f" % (bz, bz + DELTA_Z) if bz is not None else u"无",
                 u"%.1f" % p0 if p0 is not None else u"无"))
            shown += 1
        if boxes:
            box_moved += len(boxes)
        if DRY_RUN:
            continue

        # ① 样条点在世界空间抬回 +13（恢复原始局部偏移）
        for sp in sps:
            n = sp.get_number_of_spline_points()
            sp.modify(True)
            for i in range(n):
                p = sp.get_location_at_spline_point(i, WS)
                sp.set_location_at_spline_point(
                    i, unreal.Vector(p.x, p.y, p.z - DELTA_Z), WS, False)
            sp.update_spline()

        # ② actor 整体降 13，Box 跟着走
        actor.modify(True)
        actor.set_actor_location(
            unreal.Vector(a_loc.x, a_loc.y, a_loc.z + DELTA_Z), False, False)

        # ③ 回读点数：挪 actor 会触发构造脚本重跑，可能把样条冲回默认的 2 个点
        after = {sp.get_name(): sp.get_number_of_spline_points()
                 for sp in actor.get_components_by_class(unreal.SplineComponent)}
        for name, cnt in before_counts:
            if after.get(name, -1) != cnt:
                collapsed.append(u"%s/%s %d → %s"
                                 % (actor.get_actor_label(), name, cnt,
                                    after.get(name, u"缺失")))

        tags = [t for t in list(actor.tags) if str(t) != SPLINE_DONE_TAG]
        tags.append(ACTOR_DONE_TAG)
        actor.set_editor_property("tags", tags)

    w(u"")
    if DRY_RUN:
        w(u"只是预览：会动 %d 个 actor（含 %d 个 Box）。DRY_RUN 改 False 再跑。"
          % (len(targets), box_moved))
        return

    w(u"已处理 %d 个 actor，Box %d 个跟着一起降。样条的世界位置保持不变。"
      % (len(targets), box_moved))
    if collapsed:
        w(u"")
        w(u"！以下样条的点数在挪动后变了，**被构造脚本冲掉了**：")
        for x in collapsed[:10]:
            w(u"    %s" % x)
        w(u"  这批需要重新生成，别直接保存。")
    else:
        w(u"点数回读全部一致，没有被构造脚本冲掉。")
    w(u"")
    w(u"**记得 Ctrl+S。** 验证：保存 → 重启 → 跑 diag_lane_fit.py，")
    w(u"垂直那栏仍应接近 0（期望值已是 2cm）。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[loweract] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[loweract] 写入 %s" % OUT)
