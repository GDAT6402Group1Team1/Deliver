# -*- coding: utf-8 -*-
u"""以斑马线 actor 为准，统一 Inter_* 段上的 Crosswalk 停车 tag。

为什么需要这个脚本
------------------
"这个路口有斑马线"和"车要在这儿提前停"是**同一个事实**，只存一处——存在
`Inter_*` 的 `Crosswalk` tag 上，`UDeliveryTrafficCarComponent::StopLineRequiredTag`
读它。但摆斑马线和打 tag 曾经分别由两个脚本负责：
`add_crosswalks_30_45.py` 两件都做，而新版 `setup_crosswalks.py` 只重做外观、
保留旧 tag。于是旧 tag 可以在标线没生成出来的地方留成孤儿——
实测 `Intersection_18` 就有 3 条这样的段，车在那儿白停 400 cm。

这个脚本把 tag 重新对齐到标线上：有标线的加 tag、没标线的摘 tag。

判据是几何关系，不是单纯距离
----------------------------
斑马线中心按设计在**段入口的正后方** `BOX_HALF(128) + SETBACK/2(200) = 328` cm 处
（主路实测 374~387，因为路更宽、量到的边界更远）。所以判据是：
  * 沿行驶方向的投影为**负**（在后方），绝对值落在 [BEHIND_MIN, BEHIND_MAX]
  * 横向偏移不超过 LATERAL_MAX
只比直线距离的话，对向车道和横向那条路的段也会落进 1200 cm 内被误判——
而误判的后果是车在**已经过了斑马线之后**才提前停，停在路口里。

**只改 tag。** 不动位置、不动样条、不删不加任何 actor。

用法
----
    py sync_crosswalk_tags.py        # DRY_RUN=True 时只报告不写
无界面：
    UnrealEditor-Cmd <工程>.uproject -run=pythonscript \
        -script=<本文件绝对路径> -unattended -nopause -nosplash -NullRHI
输出在 ~/Library/Logs/Unreal Engine/DeliveryEditor/Delivery.log
"""

import math
import re
import traceback

import unreal

DRY_RUN = False

LEVEL = u"/Game/Level/TestForCharacter"
STOP_TAG = u"Crosswalk"
SURFACE_TAG = u"DeliveryCrosswalkSurfaceV2"
INTER = re.compile(r'^Inter_(.+)_([+-]\d+)_I\d+$')

# 斑马线中心相对段入口、沿行驶方向的**后方**距离范围。
# 设计值 328（支路）/ 374~387（主路，路更宽），两头各留余量
BEHIND_MIN = 150.0
BEHIND_MAX = 800.0
# 横向容差：主路全宽 1800、半宽 900，加余量。给太大会把横向那条路的段吃进来
LATERAL_MAX = 1100.0

WS = unreal.SplineCoordinateSpace.WORLD

_lines = []


def w(s=u""):
    _lines.append(s)
    unreal.log(u"[synctag] %s" % s)


def entry_and_dir(actor):
    u"""段入口点和行驶方向。

    方向用**两个采样点作差**：`get_direction_at_distance_along_spline` 在本工程的
    样条端点上返回过零向量（CLAUDE.md「端点切线」那条）。
    """
    main = None
    splines = actor.get_components_by_class(unreal.SplineComponent)
    for sp in splines:
        if sp.get_name() == u"Spline":
            main = sp
            break
    if main is None and splines:
        main = splines[0]
    if main is None or main.get_number_of_spline_points() < 2:
        return None
    length = main.get_spline_length()
    if length <= 1.0:
        return None
    p0 = main.get_location_at_distance_along_spline(0.0, WS)
    p1 = main.get_location_at_distance_along_spline(min(200.0, length), WS)
    dx, dy = p1.x - p0.x, p1.y - p0.y
    n = math.hypot(dx, dy)
    if n < 0.01:
        return None
    return p0, (dx / n, dy / n)


def run():
    w(u"=== 以斑马线为准统一 Crosswalk tag ===")
    w(u"DRY_RUN = %s" % DRY_RUN)

    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not les.load_level(LEVEL):
        w(u"！加载不了 %s" % LEVEL)
        return
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = [a for a in eas.get_all_level_actors() if a]

    marks = [a for a in actors
             if a.get_actor_label().startswith(u"Crosswalk_")
             or SURFACE_TAG in [str(t) for t in a.tags]]
    if not marks:
        w(u"！图里一个斑马线 actor 都没有。以它为准就等于要摘掉所有 tag，"
          u"这多半是加载错了关卡或者标线还没摆——不动任何东西，退出。")
        return

    segs = []
    for a in actors:
        if not INTER.match(a.get_actor_label()):
            continue
        ed = entry_and_dir(a)
        if ed is None:
            w(u"  ！%s 取不到方向，跳过（不改它的 tag）" % a.get_actor_label())
            continue
        segs.append((a, ed[0], ed[1]))

    w(u"斑马线 actor %d 个、路口段 %d 条" % (len(marks), len(segs)))

    to_add, to_remove, keep = [], [], 0
    for actor, entry, (fx, fy) in segs:
        best = None
        for m in marks:
            loc = m.get_actor_location()
            dx, dy = loc.x - entry.x, loc.y - entry.y
            along = dx * fx + dy * fy          # 负 = 在段入口后方
            lateral = abs(-dx * fy + dy * fx)
            if along >= 0.0:
                continue
            behind = -along
            if BEHIND_MIN <= behind <= BEHIND_MAX and lateral <= LATERAL_MAX:
                if best is None or behind < best[0]:
                    best = (behind, lateral, m.get_actor_label())

        tags = [str(t) for t in actor.tags]
        has = STOP_TAG in tags
        if best and not has:
            to_add.append((actor, tags, best))
        elif not best and has:
            to_remove.append((actor, tags))
        elif best and has:
            keep += 1

    w(u"")
    w(u"要加 tag（有斑马线、现在没 tag）：%d 条" % len(to_add))
    for actor, _tags, best in to_add:
        w(u"  + %-40s 后方 %.0f cm、横向 %.0f cm（%s）"
          % (actor.get_actor_label(), best[0], best[1], best[2]))

    w(u"")
    w(u"要摘 tag（没斑马线、却有 tag）：%d 条" % len(to_remove))
    for actor, _tags in to_remove:
        w(u"  - %s" % actor.get_actor_label())

    w(u"")
    w(u"本来就对的：%d 条" % keep)

    if DRY_RUN:
        w(u"DRY_RUN：什么都没写。")
        return

    changed = 0
    for actor, tags, _best in to_add:
        actor.tags = [unreal.Name(t) for t in tags] + [unreal.Name(STOP_TAG)]
        changed += 1
    for actor, tags in to_remove:
        actor.tags = [unreal.Name(t) for t in tags if t != STOP_TAG]
        changed += 1

    if changed:
        # 回读验证：写 tag 这件事在本工程栽过（"写完立刻读回是好的，存盘重载就变回去"）
        bad = 0
        for actor, _tags, _best in to_add:
            if STOP_TAG not in [str(t) for t in actor.tags]:
                w(u"  ！%s 加 tag 没生效" % actor.get_actor_label())
                bad += 1
        for actor, _tags in to_remove:
            if STOP_TAG in [str(t) for t in actor.tags]:
                w(u"  ！%s 摘 tag 没生效" % actor.get_actor_label())
                bad += 1
        if bad:
            w(u"！有 %d 条没写成功，不保存关卡。" % bad)
            return
        les.save_current_level()
        w(u"已改 %d 条并保存关卡（只动了 tag）。" % changed)
    else:
        w(u"tag 本来就和斑马线一致，没有要改的。")


try:
    run()
except Exception:
    unreal.log_error(traceback.format_exc())
