# -*- coding: utf-8 -*-
"""回读车道样条和车的当前高度，验证 lower_lane_splines.py 的改动有没有留住。

为什么要单独回读：脚本报的"写入了 N 个点"只能证明函数被调用过，证明不了
结果留住了——这是 CLAUDE.md 里记过的教训（普通函数调用只改内存、不把对象
标记为已修改，存盘时整个对象不被序列化；现象是写完立刻读回是好的、
存盘重载就变回默认值）。

对照的是 lower_lane_splines.py 报告里记下的精确前后值：
    Inter_形状12_+0180_I00 / Spline       首点 Z  3534.2 → 3521.2
所以回读到 3521 = 留住了，3534 = 没留住。

顺便报告完成标签在不在：标签没了而高度也没降，说明关卡被重载过（没保存）。

只读。用法：py check_lane_z.py
"""

import traceback

import unreal

LANE_TAG_PREFIX = "ClaudeGenLane"
LANE_DONE_TAG = "ClaudeLaneZ2cm"
CAR_DONE_TAG = "ClaudeCarZ2cm"
CAR_CLASS_PREFIX = "BP_car_base"

# (actor 标签, 组件名, 降之前, 降之后)
EXPECT = [
    (u"Inter_形状12_+0180_I00", u"Spline", 3534.2, 3521.2),
    (u"Inter_形状12_+0180_I00", u"SplineLeft", 3534.2, 3521.2),
    (u"Inter_形状12_+0180_I01", u"Spline", 3170.9, 3157.9),
    (u"TestCar_00", None, 2570.1, 2557.1),
    (u"TestCar_02", None, 2418.4, 2405.4),
]

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "check_lane_z.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[checkz] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    actors = eas.get_all_level_actors()
    by_label = {}
    for a in actors:
        by_label.setdefault(a.get_actor_label(), a)

    w(u"=== 回读验证：降 13cm 留住了吗 ===")
    w(u"关卡：%s" % (world.get_name() if world else u"?"))
    w(u"")
    w(u"%-26s %-12s %9s %9s %9s  %s"
      % (u"对象", u"组件", u"降前", u"应为", u"实读", u"结论"))

    held = missed = unknown = 0
    for label, comp_name, before, after in EXPECT:
        a = by_label.get(label)
        if a is None:
            w(u"%-26s %-12s %9.1f %9.1f %9s  找不到这个 actor"
              % (label[:26], comp_name or u"-", before, after, u"-"))
            unknown += 1
            continue
        if comp_name is None:
            cur = a.get_actor_location().z
        else:
            cur = None
            for sp in a.get_components_by_class(unreal.SplineComponent):
                if sp.get_name() == comp_name:
                    cur = sp.get_location_at_spline_point(0, WS).z
                    break
            if cur is None:
                w(u"%-26s %-12s %9.1f %9.1f %9s  找不到这个组件"
                  % (label[:26], comp_name, before, after, u"-"))
                unknown += 1
                continue
        if abs(cur - after) < 1.0:
            verdict, held = u"✔ 留住了", held + 1
        elif abs(cur - before) < 1.0:
            verdict, missed = u"✘ 变回降前的值", missed + 1
        else:
            verdict, unknown = u"? 两个都不是", unknown + 1
        w(u"%-26s %-12s %9.1f %9.1f %9.1f  %s"
          % (label[:26], comp_name or u"-", before, after, cur, verdict))

    # 标签还在不在：标签和高度一起没了 = 关卡被重载过（没保存）
    lane_tagged = sum(1 for a in actors
                      if LANE_DONE_TAG in [str(t) for t in a.tags])
    lane_total = sum(1 for a in actors
                     if any(str(t).startswith(LANE_TAG_PREFIX) for t in a.tags))
    car_tagged = sum(1 for a in actors
                     if CAR_DONE_TAG in [str(t) for t in a.tags])
    car_total = sum(1 for a in actors
                    if a.get_class().get_name().startswith(CAR_CLASS_PREFIX))
    w(u"")
    w(u"完成标签：样条 %d/%d 带 %s；车 %d/%d 带 %s"
      % (lane_tagged, lane_total, LANE_DONE_TAG, car_tagged, car_total, CAR_DONE_TAG))

    w(u"")
    w(u"—— 怎么读 ——")
    if missed and lane_tagged == 0:
        w(u"高度变回降前的值、标签也一个都没了 → **关卡被重载过，改动没保存**。")
        w(u"  重跑 lower_lane_splines.py，然后**立刻 Ctrl+S**。")
    elif missed and lane_tagged > 0:
        w(u"标签还在、但高度变回去了 → 改动被**构造脚本冲掉**了。")
        w(u"  这说明 spline_has_been_edited 没起作用，得换写法（先改属性触发重建、")
        w(u"  等重建完再写点），不是重跑一次能解决的。")
    elif held and not missed:
        w(u"高度对得上 → **改动在内存里是好的**。")
        if lane_tagged == 0:
            w(u"  但标签为 0，有点反常，值得再看一眼。")
        w(u"  注意这只证明「现在是对的」；存盘重载后再跑一次本脚本才算证明存住了。")
    else:
        w(u"结果不一致，逐行看上面的表。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[checkz] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[checkz] 写入 %s" % OUT)
