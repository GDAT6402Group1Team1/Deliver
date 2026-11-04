# -*- coding: utf-8 -*-
u"""把 testfortraffic 的车流减到几辆并放慢，方便做别的测试。

用法：编辑器控制台 `py thin_out_traffic.py`。改顶部三个数就能调。
恢复满车流：`py spawn_test_cars.py`（它会清掉所有车重新摆 30 辆）。

**改的是关卡里已有的车，不重新摆。** 已有的车都已经接上车道了
（能接上路的最远出生距离只有 233 cm，重摆就要重新过那道筛子），
留着它们比重摆稳。

三件事，缺一件都达不到"只有两三辆慢车"：

  1. **删到只剩 KEEP 辆**，保留离锚点最近的——留在视野里才看得见，
     撒在全图等于"车都不见了"。
  2. **把 MaxSpeed 写低**。`MaxSpeed` 在整张蓝图里没有 Set 节点，是纯配置值。
     低速不会让车接不上路：扫描长度 = UpdateFrameRate(60) × 5 × 车速，
     300 的车速也有 90000 cm，而实测所需最低车速只有个位数
     （见 Saved/diag_handoff_reach.txt）。
  3. **关掉分波克隆**（`bSpawnExtraWaves`）。不关的话每辆车会在自己的出生点
     再克隆两波（15 秒、再 30 秒），**留 3 辆 45 秒后会变成 9 辆**，
     表现是"明明删得只剩几辆，过一会儿又多起来了"。
  4. **关掉跟车速度协调**（`bPushSlowLeader`）。这一条最阴：两辆车一旦近距离跟车，
     组件会**把前车的 MaxSpeed 直接写成自己的**，并把自己压到
     `max(自己 - PushSlowLeaderSelfPenalty, MinPushedMaxSpeed)`，而那个下限默认是 **600**。
     也就是说只要有一次跟车，车速就被顶回 600 并且**留在那儿**（写的是配置值本身），
     表现是"车速改了、跑一会儿又快起来了"。保险起见同时把
     `MinPushedMaxSpeed` 压到 MAX_SPEED，万一哪天这个开关被打开也不会顶上去。

注意 `SetCurrentSpeed` 之类的运行时量不用管：车在 PIE 里从 0 按
`AcelerationPerSecond` 加速到 `MaxSpeed`，改 MaxSpeed 就够了。
"""

import math

import unreal

# ---------------------------------------------------------------- 配置

EXPECTED_LEVEL = "testfortraffic"
NAME_PREFIX = "BP_car_base"      # 车型蓝图和已放置实例的类名都按这个前缀认

KEEP = 3                         # 留几辆
MAX_SPEED = 300.0                # 留下来的车速上限（原来是 500~1300 随机）
DISABLE_EXTRA_WAVES = True       # 关掉分波克隆，不关的话 3 辆会变 9 辆
DISABLE_PUSH_SLOW_LEADER = True  # 关掉跟车速度协调，不关的话车速会被顶回 600

# 留哪几辆：离这条路最近的。和 spawn_test_cars.py 用同一个锚点。
LANE_TAG_PREFIX = "ClaudeGenLane"
ANCHOR_ROAD = "MainRoad_R003"

OUT = unreal.Paths.project_saved_dir() + "thin_out_traffic.txt"
lines = []


def w(s=u""):
    lines.append(str(s))
    unreal.log(u"[traffic] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write(u"\n".join(lines))
        unreal.log(u"[traffic] 报告写到 %s" % OUT)
    except Exception as exc:
        unreal.log_error(u"[traffic] 写报告失败：%s" % exc)


def _anchor_center(actors):
    u"""锚点路上所有车道段起点的平均位置，找不到就 None。"""
    pts = []
    for a in actors:
        try:
            if not unreal.SystemLibrary.is_valid(a):
                continue
        except Exception:
            pass
        if not any(str(t).startswith(LANE_TAG_PREFIX) for t in a.tags):
            continue
        if ANCHOR_ROAD not in a.get_actor_label():
            continue
        loc = a.get_actor_location()
        pts.append((loc.x, loc.y))
    if not pts:
        return None
    return (sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts))


def _set_component_flag(car, prop_names, value):
    u"""在车的交通组件上写一个属性，返回 (成功, 说明)。"""
    comp = None
    comp_class = getattr(unreal, "DeliveryTrafficCarComponent", None)
    if comp_class is not None:
        comp = car.get_component_by_class(comp_class)
    if comp is None:
        return False, u"取不到 DeliveryTrafficCarComponent"

    for name in prop_names:
        try:
            comp.set_editor_property(name, value)
        except Exception:
            continue
        # 写入 API 可能静默失败，回读一次才算数
        try:
            if comp.get_editor_property(name) == value:
                comp.modify(True)
                return True, name
        except Exception:
            pass
    return False, u"属性名都写不进去（%s）" % u"/".join(prop_names)


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if world is None:
        w(u"！取不到编辑器世界。")
        flush()
        return

    level_name = unreal.SystemLibrary.get_object_name(world)
    if EXPECTED_LEVEL.lower() not in level_name.lower():
        w(u"！当前关卡是 %s，不是 %s，什么都不做。**删车是不可逆的**，"
          u"不在确认的关卡上动手。" % (level_name, EXPECTED_LEVEL))
        flush()
        return

    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    cars = []
    for a in actors:
        try:
            if not unreal.SystemLibrary.is_valid(a) or a.is_actor_being_destroyed():
                continue
        except Exception:
            pass
        # 按**类**认车，不按名字：关卡里的车实例名字五花八门
        # （carbase4 / BP_car_base2 / …），按标签或名字匹配漏得到处都是
        if str(a.get_class().get_name()).startswith(NAME_PREFIX):
            cars.append(a)

    w(u"关卡里现有车辆 %d 辆，目标留 %d 辆、车速上限 %.0f。"
      % (len(cars), KEEP, MAX_SPEED))
    if not cars:
        w(u"！一辆车都没有，先跑 spawn_test_cars.py。")
        flush()
        return

    center = _anchor_center(actors)
    if center is None:
        w(u"！找不到 %s 的车道段，按车辆原顺序留前 %d 辆。" % (ANCHOR_ROAD, KEEP))
        ordered = cars
    else:
        w(u"锚点中心 (%.0f, %.0f)，按离它的距离排序。" % center)
        def key(c):
            loc = c.get_actor_location()
            return math.sqrt((loc.x - center[0]) ** 2 + (loc.y - center[1]) ** 2)
        ordered = sorted(cars, key=key)

    keep = ordered[:max(0, KEEP)]
    drop = ordered[max(0, KEEP):]

    # 先改保留车、再删多余车。**顺序刻意**：万一属性写不进去，
    # 报告里会看到"一辆都没改成"，而车还全在，可以原样重试；
    # 先删的话失败时已经不可逆了
    w(u"")
    for car in keep:
        label = car.get_actor_label()
        loc = car.get_actor_location()

        speed_ok = False
        for name in ("MaxSpeed", "max_speed"):
            try:
                car.set_editor_property(name, float(MAX_SPEED))
                if abs(float(car.get_editor_property(name)) - MAX_SPEED) < 0.01:
                    speed_ok = True
                    break
            except Exception:
                continue

        wave_note = u"未改"
        if DISABLE_EXTRA_WAVES:
            ok, note = _set_component_flag(
                car, ("bSpawnExtraWaves", "spawn_extra_waves", "bSpawnSecondWave",
                      "spawn_second_wave"), False)
            wave_note = (u"已关（%s）" % note) if ok else (u"**没关掉**：%s" % note)

        push_note = u"未改"
        if DISABLE_PUSH_SLOW_LEADER:
            ok, note = _set_component_flag(
                car, ("bPushSlowLeader", "push_slow_leader"), False)
            push_note = u"已关" if ok else (u"**没关掉**：%s" % note)
            # 下限也压下去：开关万一被打开，至少不会把车速顶到 600
            _set_component_flag(car, ("MinPushedMaxSpeed", "min_pushed_max_speed"),
                                float(MAX_SPEED))

        car.modify(True)
        w(u"留 %-22s (%.0f, %.0f)  车速 %s  分波 %s  跟车协调 %s"
          % (label, loc.x, loc.y,
             u"%.0f" % MAX_SPEED if speed_ok else u"**写不进去**", wave_note, push_note))

    killed = 0
    for car in drop:
        try:
            if eas.destroy_actor(car):
                killed += 1
        except Exception as exc:
            w(u"！删 %s 失败：%s" % (car.get_actor_label(), exc))

    w(u"")
    w(u"删掉 %d 辆，剩 %d 辆。" % (killed, len(keep)))
    if DISABLE_EXTRA_WAVES:
        w(u"分波克隆已关：不关的话 45 秒后这 %d 辆会变成 %d 辆。" % (len(keep), len(keep) * 3))
    w(u"车速写的是 MaxSpeed（纯配置值，蓝图里没有 Set 节点）；"
      u"车在 PIE 里仍从 0 加速上去。")
    if DISABLE_PUSH_SLOW_LEADER:
        w(u"跟车速度协调已关：不关的话两车一跟近，MaxSpeed 会被顶到 "
          u"MinPushedMaxSpeed（默认 600）并留在那儿。")
    w(u"低速不影响接车道：扫描长度 = 60 × 5 × 车速，%.0f 的车速有 %.0f cm。"
      % (MAX_SPEED, 60.0 * 5.0 * MAX_SPEED))
    w(u"要恢复满车流：py spawn_test_cars.py（清掉所有车重新摆 30 辆）。")
    w(u"**记得存关卡**，否则这些改动在重载时丢。")
    flush()


if __name__ == "__main__":
    run()
else:
    run()
