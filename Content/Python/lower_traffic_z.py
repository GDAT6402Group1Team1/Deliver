# -*- coding: utf-8 -*-
"""交通线 + 车整体降 13cm（离地 15 → 2）。一个脚本做完，自带状态诊断和回读验证。

核心做法：**只挪 actor**
------------------------
样条点存的是**局部**坐标，所以把 actor 往下挪 13，样条、Box、车身网格全部
跟着一起降——一次操作三样东西同时到位。

之前绕过远路：第一版去改样条点的**世界**坐标，actor 和 Box 没动，于是三者
对不齐，又写了第二个脚本补救（先把点抬回去、再挪 actor）。两次改动叠加，
状态难追、也更容易被构造脚本或重载打回去。这一版回到最简单的形式。

两段独立处理，各有各的完成标签，互不阻塞：
  ① 车道/路口 actor（tag 前缀 ClaudeGenLane，含 Lane_ 和 Inter_）
  ② 车（类名前缀 BP_car_base）

自带三样东西
------------
· **状态诊断**：先报告"样条首点 Z − actor Z"的分布。原始摆法下这个差应当是 0
  （spawn_lane 在第一个控制点上生成 actor）。如果大量是 -13，说明上一轮
  改世界坐标点的效果还在、actor 没跟上——脚本会先把它归零再统一下降。
· **回读验证**：挪完立刻读回 actor / Box / 样条首点三者的世界 Z，逐个核对
  是不是都正好降了 13，对不上就点名。不靠"写入了 N 个"这种计数器。
· **点数护栏**：挪 actor 会触发构造脚本重跑，可能把样条冲回蓝图默认的 2 个点。
  挪之前记点数、挪之后回读比对，变了就报警并提示别保存。

用法：py lower_traffic_z.py      （DRY_RUN=True 只诊断不改）
**跑完立刻 Ctrl+S**，这些改动全在内存里。
"""

import traceback

import unreal

DRY_RUN = False
DELTA_Z = -13.0

LANE_TAG_PREFIX = "ClaudeGenLane"
LANE_DONE_TAG = "ClaudeLaneZDown13"
LOWER_CARS = True                  # 诊断提示「车已经降过」时改成 False
CAR_CLASS_PREFIX = "BP_car_base"
CAR_DONE_TAG = "ClaudeCarZDown13"

# 认为"actor 和样条已经对齐"的容差
ALIGN_TOL = 1.0

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "lower_traffic_z.txt"
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
    """先打「已被编辑」标记，构造脚本重跑时才会把实例的样条数据应用回去。"""
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


def first_point_z(actor):
    for sp in actor.get_components_by_class(unreal.SplineComponent):
        if sp.get_number_of_spline_points() > 0:
            return sp.get_location_at_spline_point(0, WS).z
    return None


def box_z(actor):
    boxes = actor.get_components_by_class(unreal.BoxComponent)
    return boxes[0].get_world_location().z if boxes else None


def diagnose(lane_actors):
    """报告 actor 和样条当前的相对关系，判断关卡处在哪个状态。"""
    offsets = []
    for a in lane_actors:
        fz = first_point_z(a)
        if fz is not None:
            offsets.append(fz - a.get_actor_location().z)
    if not offsets:
        w(u"  （没有可测的样条）")
        return
    aligned = sum(1 for d in offsets if abs(d) <= ALIGN_TOL)
    off13 = sum(1 for d in offsets if abs(d + 13.0) <= ALIGN_TOL)
    other = len(offsets) - aligned - off13
    w(u"  样条首点 Z − actor Z：对齐(≈0) %d 个；低 13 的 %d 个；其它 %d 个"
      % (aligned, off13, other))
    if off13:
        w(u"  → 有 %d 个处于「样条已降、actor 没跟上」的半成品状态，"
          u"脚本会先把样条抬回与 actor 对齐，再统一下降。" % off13)
    if other:
        sample = [d for d in offsets
                  if abs(d) > ALIGN_TOL and abs(d + 13.0) > ALIGN_TOL][:5]
        w(u"  → 有 %d 个偏移既不是 0 也不是 -13，样例：%s"
          % (other, u", ".join(u"%.1f" % d for d in sample)))


def diagnose_cars(cars, lane_actors):
    """车原本摆在车道起点的高度上（spawn_test_cars.py: p.z + CAR_Z，CAR_Z=0），
    所以「车 Z − 最近车道起点 Z」就是判断车降没降的不变量：

        ≈ +13  车还没降，车道已经降了  → 这次该降
        ≈   0  两边都降了（或都没降）   → 再降就是多降
        ≈ -13  车已经比车道低 13       → 已经多降过一次

    这一段是被「车有一起降低吗」这个问题逼出来的：车这边没有 actor/样条那种
    内部不变量可查，不比对车道的话，双降了也看不出来。
    """
    starts = []
    for a in lane_actors:
        fz = first_point_z(a)
        if fz is not None:
            loc = a.get_actor_location()
            starts.append((loc.x, loc.y, fz))
    if not starts or not cars:
        return
    devs = []
    for c in cars:
        loc = c.get_actor_location()
        best = None
        for sx, sy, sz in starts:
            d2 = (sx - loc.x) ** 2 + (sy - loc.y) ** 2
            if best is None or d2 < best[0]:
                best = (d2, sz)
        devs.append(loc.z - best[1])
    devs.sort()
    mid = devs[len(devs) // 2]
    near0 = sum(1 for d in devs if abs(d) <= 3.0)
    near13 = sum(1 for d in devs if abs(d - 13.0) <= 3.0)
    neg13 = sum(1 for d in devs if abs(d + 13.0) <= 3.0)
    w(u"  车 Z − 最近车道起点 Z：中位数 %+.1f（≈+13 的 %d 辆，≈0 的 %d 辆，"
      u"≈-13 的 %d 辆，共 %d 辆）" % (mid, near13, near0, neg13, len(devs)))
    if near0 > len(devs) * 0.5:
        # ≈0 是**有歧义**的：两边都降了、和两边都没降，读数一模一样。
        # 第一版只写了前一种解释，结果在"两边都没降"的情况下误报了一次。
        # 要分辨只能看车道的绝对高度对不对得上原始值，所以这里只陈述事实、
        # 由上面那行"样条首点 − actor"和车道的实际 Z 一起判断。
        w(u"  · 车和车道同高。**这一档有歧义**：可能是两边都降过了，")
        w(u"    也可能是两边都还没降（比如上一轮改动没留住、关卡退回原始状态）。")
        w(u"    分辨办法：看下面 ① 段打印的车道 actor 起步高度——")
        w(u"    和原始值一样就是都没降，这次该降；已经低 13 就是都降过了，")
        w(u"    再跑会变成 -26，把 LOWER_CARS 改成 False。")
    elif near13 > len(devs) * 0.5:
        w(u"  → 多数车比车道高 13，正是「车道降了、车没降」，这次该降。")
    elif neg13 > len(devs) * 0.5:
        w(u"  ⚠ 多数车已经比车道低 13 —— 之前多降过一次，**这次别再降**。")


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    actors = eas.get_all_level_actors()

    lane_all = [a for a in actors
                if any(str(t).startswith(LANE_TAG_PREFIX) for t in a.tags)]
    lane_todo = [a for a in lane_all
                 if LANE_DONE_TAG not in [str(t) for t in a.tags]]
    car_all = [a for a in actors
               if a.get_class().get_name().startswith(CAR_CLASS_PREFIX)]
    car_todo = [a for a in car_all
                if CAR_DONE_TAG not in [str(t) for t in a.tags]]

    w(u"=== 交通线 + 车整体降 %.0fcm ===" % -DELTA_Z)
    w(u"关卡：%s" % (world.get_name() if world else u"?"))
    w(u"模式：%s" % (u"只诊断不改（DRY_RUN）" if DRY_RUN else u"**写入**"))
    w(u"")
    w(u"—— 当前状态 ——")
    w(u"车道/路口 actor 共 %d 个，待处理 %d 个（其余已带 %s 标签）"
      % (len(lane_all), len(lane_todo), LANE_DONE_TAG))
    diagnose(lane_all)
    w(u"车 共 %d 辆，待处理 %d 辆" % (len(car_all), len(car_todo)))
    diagnose_cars(car_all, lane_all)

    if DRY_RUN:
        w(u"")
        w(u"DRY_RUN：什么都没改。把顶部 DRY_RUN 改成 False 再跑。")
        return

    # ---------- ① 车道 / 路口 ----------
    w(u"")
    w(u"—— ① 车道 / 路口 ——")
    realigned = 0
    bad_move = []
    collapsed = []
    shown = 0
    for a in sorted(lane_todo, key=lambda x: x.get_actor_label()):
        sps = a.get_components_by_class(unreal.SplineComponent)
        before_counts = [(sp.get_name(), sp.get_number_of_spline_points()) for sp in sps]
        a_z0 = a.get_actor_location().z
        f_z0 = first_point_z(a)
        b_z0 = box_z(a)

        # 归零：样条相对 actor 的偏移应当是 0（spawn_lane 在首点上生成 actor）。
        # 上一轮改世界坐标留下的 -13 在这里补回去，之后再统一下降。
        if f_z0 is not None and abs(f_z0 - a_z0) > ALIGN_TOL:
            fix = a_z0 - f_z0
            for sp in sps:
                mark_edited(sp)
                sp.modify(True)
                n = sp.get_number_of_spline_points()
                for i in range(n):
                    p = sp.get_location_at_spline_point(i, WS)
                    sp.set_location_at_spline_point(
                        i, unreal.Vector(p.x, p.y, p.z + fix), WS, False)
                sp.update_spline()
            realigned += 1

        loc = a.get_actor_location()
        a.modify(True)
        a.set_actor_location(
            unreal.Vector(loc.x, loc.y, loc.z + DELTA_Z), False, False)

        # 回读：三者是否都正好降了 DELTA_Z
        a_z1 = a.get_actor_location().z
        f_z1 = first_point_z(a)
        b_z1 = box_z(a)
        if abs((a_z1 - a_z0) - DELTA_Z) > ALIGN_TOL:
            bad_move.append(u"%s actor %.1f → %.1f" % (a.get_actor_label(), a_z0, a_z1))
        if f_z0 is not None and f_z1 is not None and abs(f_z1 - a_z1) > ALIGN_TOL:
            bad_move.append(u"%s 样条没跟上 actor（差 %.1f）"
                            % (a.get_actor_label(), f_z1 - a_z1))
        if b_z0 is not None and b_z1 is not None and abs((b_z1 - b_z0) - DELTA_Z) > ALIGN_TOL:
            bad_move.append(u"%s box %.1f → %.1f" % (a.get_actor_label(), b_z0, b_z1))

        after = {sp.get_name(): sp.get_number_of_spline_points()
                 for sp in a.get_components_by_class(unreal.SplineComponent)}
        for name, cnt in before_counts:
            if after.get(name, -1) != cnt:
                collapsed.append(u"%s/%s %d → %s"
                                 % (a.get_actor_label(), name, cnt, after.get(name, u"缺失")))

        if shown < 5:
            w(u"  %-28s actor %.1f→%.1f  box %s  样条首点 %s"
              % (a.get_actor_label()[:28], a_z0, a_z1,
                 u"%.1f→%.1f" % (b_z0, b_z1) if b_z0 is not None else u"无",
                 u"%.1f→%.1f" % (f_z0, f_z1) if f_z0 is not None else u"无"))
            shown += 1
        add_tag(a, LANE_DONE_TAG)

    w(u"  处理 %d 个；其中 %d 个先做了对齐归零。" % (len(lane_todo), realigned))

    # ---------- ② 车 ----------
    w(u"")
    w(u"—— ② 车 ——")
    if not LOWER_CARS:
        w(u"  LOWER_CARS 关着，跳过。")
        car_todo = []
    for i, car in enumerate(sorted(car_todo, key=lambda x: x.get_actor_label())):
        loc = car.get_actor_location()
        car.modify(True)
        car.set_actor_location(
            unreal.Vector(loc.x, loc.y, loc.z + DELTA_Z), False, False)
        z1 = car.get_actor_location().z
        if abs((z1 - loc.z) - DELTA_Z) > ALIGN_TOL:
            bad_move.append(u"%s 车没动（%.1f → %.1f）"
                            % (car.get_actor_label(), loc.z, z1))
        if i < 5:
            w(u"  %-24s [%s]  Z %.1f → %.1f"
              % (car.get_actor_label()[:24], car.get_class().get_name()[:16], loc.z, z1))
        add_tag(car, CAR_DONE_TAG)
    w(u"  处理 %d 辆。" % len(car_todo))

    # ---------- 结论 ----------
    w(u"")
    w(u"—— 回读验证 ——")
    if bad_move:
        w(u"！%d 处没有按预期移动：" % len(bad_move))
        for x in bad_move[:12]:
            w(u"    %s" % x)
        w(u"  **先别保存**，把这份报告发出来。")
    else:
        w(u"actor / Box / 样条首点 三者全部正好降了 %.0f，没有例外。" % -DELTA_Z)
    if collapsed:
        w(u"！%d 个样条的点数在挪动后变了（被构造脚本冲掉）：" % len(collapsed))
        for x in collapsed[:10]:
            w(u"    %s" % x)
        w(u"  **先别保存**。")
    elif lane_todo:
        w(u"样条点数回读全部一致，没有被构造脚本冲掉。")

    w(u"")
    w(u"**立刻 Ctrl+S 保存关卡**，改动只在内存里。")
    w(u"保存后重启编辑器、再跑一次本脚本（DRY_RUN=True）看状态诊断：")
    w(u"「对齐(≈0)」应当是全部，标签数也应当还在——那才算证明存住了。")


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
