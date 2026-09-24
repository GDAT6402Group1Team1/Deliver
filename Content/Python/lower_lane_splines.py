# -*- coding: utf-8 -*-
"""把车道线 / 路口线 / 左右转曲线、以及关卡里的车，整体下降 13cm（离地 15 → 2）。

为什么是 13：车道点原本在路面上方 Z_OFFSET = 15cm，而那个 15 在
gen_traffic_lanes.py 里**一个字的注释都没有**（同文件里 MAIN_ROAD_OFFSETS
上面有整整五行推导，对比之下很显眼），从用法反推最可能只是为了让样条在
编辑器里不被路面遮住。车的运行时高度**直接取自样条**（BP_car_base 沿样条
SetActorLocation），所以这 15cm 会一比一变成车的浮空。降到 2cm：
既贴地，又给路面起伏和 z-fighting 留一点余量。

**纯平移**：每个点都是 p.z - 13，XY 一律不动。自动切线由邻点位置差算出，
等量下移后点差不变、切线完全一样；手工切线根本没被碰。所以线形、朝向、
弧长全部保持原样，只是整条线低了 13。

处理哪些
--------
① 带 ClaudeGenLane* 标签的 actor 上的**全部** SplineComponent。这条标签同时
   覆盖 Lane_（直行车道）和 Inter_（路口段），而左右转曲线是挂在路口段
   actor 自带的 SplineLeft / SplineRight 上的，所以一次全都降到。
② 关卡里所有 BP_car_base* 实例。按**类名前缀**认，和 spawn_test_cars.py 的
   NAME_PREFIX 同一套判据——车实例的名字五花八门（carbase4 / BP_car_base2 /
   TestCar_xx），按名字或标签匹配都会漏。

两者必须一起降：只降样条的话，静止的车看着浮 13，一开起来又会吸附到样条上
突然下沉 13。

**①和②各有各的完成标签，互不阻塞。** 第一版两者共用一个标签，而且"样条
全部已处理"时直接 return——于是先跑过一次只降样条的旧版之后，再跑加了车辆的
新版，车辆那段被整个跳过，报告还显示"没有要处理的"，看起来像成功了。
现在分成两段独立统计，哪段没做就补哪段。

用法：py lower_lane_splines.py        （DRY_RUN=True 只看不改）
跑完 **Ctrl+S 保存关卡**。两张图（TestForCharacter / testfortraffic）
各有各的车道和车，要各开一次各跑一次。
"""

import traceback

import unreal

DRY_RUN = False
DELTA_Z = -13.0                       # 15 → 2

LANE_TAG_PREFIX = "ClaudeGenLane"
LANE_DONE_TAG = "ClaudeLaneZ2cm"      # 样条的完成标签

LOWER_CARS = True
CAR_CLASS_PREFIX = "BP_car_base"
CAR_DONE_TAG = "ClaudeCarZ2cm"        # 车的完成标签，和样条分开

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "lower_lane_splines.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[lowerz] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def mark_edited(sp):
    """给样条打「已被编辑」标记，否则构造脚本一重跑就把点冲回蓝图默认值。

    必须在**写点之前**调：它是 set_editor_property，在组件上改属性会触发
    所在 actor 重跑构造脚本、组件整批重建；放在写点之后调的话，
    刚写进去的点正好落在被丢弃的那批组件上。
    """
    for name in ("spline_has_been_edited", "b_spline_has_been_edited"):
        try:
            sp.set_editor_property(name, True)
            return True
        except Exception:
            continue
    return False


def add_tag(actor, tag):
    tags = list(actor.tags)
    tags.append(tag)
    actor.set_editor_property("tags", tags)


def lower_splines(eas):
    """① 样条。返回 (处理的 actor 数, 组件数, 点数, 跳过数, 标记失败的组件)。"""
    targets, done = [], 0
    for a in eas.get_all_level_actors():
        tags = [str(t) for t in a.tags]
        if not any(t.startswith(LANE_TAG_PREFIX) for t in tags):
            continue
        if LANE_DONE_TAG in tags:
            done += 1
        else:
            targets.append(a)

    w(u"① 样条：待处理 %d 个 actor，已降过 %d 个" % (len(targets), done))
    comps = pts = 0
    failed_mark = []
    shown = 0
    for actor in sorted(targets, key=lambda a: a.get_actor_label()):
        sps = actor.get_components_by_class(unreal.SplineComponent)
        for sp in sps:
            n = sp.get_number_of_spline_points()
            if n <= 0:
                continue
            if shown < 5:
                p0 = sp.get_location_at_spline_point(0, WS)
                w(u"    %-28s %-13s %2d 点   首点 Z %.1f → %.1f"
                  % (actor.get_actor_label()[:28], sp.get_name()[:13],
                     n, p0.z, p0.z + DELTA_Z))
                shown += 1
            comps += 1
            pts += n
            if DRY_RUN:
                continue
            # 顺序固定：先标记、再 modify、最后写点。理由见 mark_edited()。
            if not mark_edited(sp):
                failed_mark.append(u"%s/%s" % (actor.get_actor_label(), sp.get_name()))
            sp.modify(True)
            actor.modify(True)
            for i in range(n):
                p = sp.get_location_at_spline_point(i, WS)
                sp.set_location_at_spline_point(
                    i, unreal.Vector(p.x, p.y, p.z + DELTA_Z), WS, False)
            sp.update_spline()
        if not DRY_RUN and sps:
            add_tag(actor, LANE_DONE_TAG)
    return len(targets), comps, pts, done, failed_mark


def lower_cars(eas):
    """② 车。和样条完全独立，样条做没做都不影响这里。"""
    if not LOWER_CARS:
        w(u"② 车辆：LOWER_CARS 关着，跳过")
        return 0, 0
    targets, done = [], 0
    for a in eas.get_all_level_actors():
        if not a.get_class().get_name().startswith(CAR_CLASS_PREFIX):
            continue
        if CAR_DONE_TAG in [str(t) for t in a.tags]:
            done += 1
        else:
            targets.append(a)

    w(u"② 车辆：待处理 %d 辆（类名前缀 %s），已降过 %d 辆"
      % (len(targets), CAR_CLASS_PREFIX, done))
    for i, car in enumerate(sorted(targets, key=lambda a: a.get_actor_label())):
        loc = car.get_actor_location()
        if i < 5:
            w(u"    %-24s [%s]  Z %.1f → %.1f"
              % (car.get_actor_label()[:24], car.get_class().get_name()[:16],
                 loc.z, loc.z + DELTA_Z))
        if DRY_RUN:
            continue
        car.modify(True)
        car.set_actor_location(
            unreal.Vector(loc.x, loc.y, loc.z + DELTA_Z), False, False)
        add_tag(car, CAR_DONE_TAG)
    return len(targets), done


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()

    w(u"=== 车道 / 路口 / 转弯样条 + 车辆，整体降 %.0fcm ===" % -DELTA_Z)
    w(u"关卡：%s" % (world.get_name() if world else u"?"))
    w(u"模式：%s" % (u"只看不改（DRY_RUN）" if DRY_RUN else u"**写入**"))
    w(u"")

    n_actors, n_comps, n_pts, n_skipped, failed_mark = lower_splines(eas)
    w(u"")
    n_cars, n_cars_done = lower_cars(eas)

    w(u"")
    if DRY_RUN:
        w(u"只是预览：会动 %d 个 actor 的 %d 个样条组件（%d 个点）+ %d 辆车。"
          % (n_actors, n_comps, n_pts, n_cars))
        w(u"DRY_RUN 改成 False 再跑一次才会写入。")
        return

    w(u"已写入：样条 %d 个 actor / %d 个组件 / %d 个点；车 %d 辆。一律 Z %+.0f。"
      % (n_actors, n_comps, n_pts, n_cars, DELTA_Z))
    if n_actors == 0 and n_skipped:
        w(u"（样条这次没动：%d 个都带着 %s 标签，说明之前已经降过一轮。）"
          % (n_skipped, LANE_DONE_TAG))
    if n_cars == 0 and n_cars_done:
        w(u"（车这次没动：%d 辆都带着 %s 标签。）" % (n_cars_done, CAR_DONE_TAG))
    if n_actors == 0 and n_cars == 0:
        w(u"两边都没动——如果这不是你预期的，去掉对应标签再跑；"
          u"但先确认真的需要，重复跑会叠加成 26cm。")

    if failed_mark:
        w(u"")
        w(u"！这 %d 个组件的「已编辑」标记没设上，**它们的改动可能存不住**："
          % len(failed_mark))
        for x in failed_mark[:8]:
            w(u"    %s" % x)
        w(u"  现象是：现在读回来是对的，存盘重载就变回原值。")

    w(u"")
    w(u"**记得 Ctrl+S 保存关卡。** 脚本不替你存。")
    w(u"验证：保存 → 重启编辑器 → 跑 diag_lane_fit.py，垂直那栏应当仍接近 0")
    w(u"（它的 Z_OFFSET 已同步改成 2）。只看「写入了多少个点」证明不了结果留住了。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[lowerz] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[lowerz] 写入 %s" % OUT)
