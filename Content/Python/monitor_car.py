# -*- coding: utf-8 -*-
"""Simulate 期间监控一辆车的运行状态，顺便实时检测"路口认错"。

用每帧回调采样，不是 sleep 轮询——Python 跑在游戏线程上，
轮询循环会把模拟本身卡死。

先跑这个脚本，再按 Simulate；脚本会等游戏世界出现再开始采。
跑满 DURATION 秒自动停并落盘，中途想停就跑 stop_monitor.py。

采两类东西：
  * 车自己的状态：位置、车速、有没有挂在样条上、被哪个路口拦住、避让系数
  * 我自己往前打的一次探测：前方第一个 Inter_ 是哪一个、离多远、朝向差多少
    （这是**我的**探测，不是蓝图那次。蓝图的半径和前瞻长度还没读出来，
      所以这里只能证明"前方有哪些候选、第一个是谁"，不能等同于蓝图的结果。）

用法：py monitor_car.py
"""

import time
import traceback

import unreal

TARGET = "car_base4"      # 按 actor 名字/标签模糊匹配；比对时忽略下划线和大小写，
                          # 所以 "carbase4" / "car_base4" / "BP_car_base4" 都能匹配上
DURATION = 120.0          # 采多久（秒）后自动停
SAMPLE_INTERVAL = 0.1     # 采样间隔（秒）
WRITE_EVERY = 2.0         # 多久落一次盘
PROBE_AHEAD = 3000.0      # 我自己那次探测往前打多远
PROBE_RADIUS = 100.0

OUT = unreal.Paths.project_saved_dir() + "car_monitor.txt"
KEY = "_delivery_car_monitor"

BP_VARS = ["CurrentSpeed", "MaxSpeed", "CurrentFollowSpline",
           "StoppedAtIntersection", "StopatInter"]
COMP_VARS = ["DebugSpeedMultiplier", "DebugDistanceAhead", "DebugBlockedElapsed"]


def road_object_type():
    members = [n for n in dir(unreal.ObjectTypeQuery) if not n.startswith("_")]
    for want in ("ECC_TRAFFIC_ROAD", "TRAFFIC_ROAD"):
        if want in members:
            return getattr(unreal.ObjectTypeQuery, want)
    for n in members:
        if "ROAD" in n.upper():
            return getattr(unreal.ObjectTypeQuery, n)
    return None


def prop(obj, name):
    try:
        return obj.get_editor_property(name)
    except Exception:
        return None


def find_car(world):
    try:
        allc = unreal.GameplayStatics.get_all_actors_of_class(world, unreal.Actor)
    except Exception:
        return None, []
    cars, names = [], []
    # 归一化：去掉下划线再比。actor 标签是 BP_car_base4，
    # 写成 carbase4 也该认出来，不然差一个下划线就白跑一轮 Simulate。
    norm = lambda s: s.lower().replace("_", "")
    want = norm(TARGET)
    for a in allc:
        try:
            nm = a.get_name()
            lbl = a.get_actor_label()
        except Exception:
            continue
        if "car" in norm(nm) or "car" in norm(lbl):
            names.append("%s  /  %s" % (nm, lbl))
            if want in norm(nm) or want in norm(lbl):
                cars.append(a)
    return (cars[0] if cars else None), names


def make_state():
    return {
        "rows": [],
        "t0": time.time(),
        "last_sample": 0.0,
        "last_write": 0.0,
        "car": None,
        "comp": None,
        "handle": None,
        "ot": road_object_type(),
        "resolved": False,
        "notes": [],
        "vars_ok": None,
    }


def write_out(st, final=False):
    lines = []
    lines.append("监控目标 %s" % TARGET)
    lines.append("采样间隔 %.2fs  时长上限 %.0fs" % (SAMPLE_INTERVAL, DURATION))
    if st["vars_ok"] is not None:
        lines.append("读到的蓝图变量: %s" % (", ".join(st["vars_ok"]) or "（一个都没读到）"))
    for n in st["notes"]:
        lines.append(n)
    lines.append("")
    lines.append("%-7s %-26s %8s %6s %-7s %-7s %6s %7s   %s" % (
        "时间", "位置", "车速", "样条", "红灯", "停哪", "避让", "前车",
        "我的探测：前方第一个 Inter_"))
    lines.append("-" * 150)
    for r in st["rows"]:
        lines.append(r)
    if final:
        lines.append("")
        lines.append("采样结束，共 %d 行。" % len(st["rows"]))
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def sample(st, world):
    car = st["car"]
    loc = car.get_actor_location()
    fwd = car.get_actor_forward_vector()
    t = time.time() - st["t0"]

    vals = {}
    for n in BP_VARS:
        vals[n] = prop(car, n)
    if st["vars_ok"] is None:
        st["vars_ok"] = [n for n in BP_VARS if vals[n] is not None]
        missing = [n for n in BP_VARS if vals[n] is None]
        if missing:
            st["notes"].append("!! 读不到的蓝图变量: %s（名字可能不一样）"
                               % ", ".join(missing))

    comp = st["comp"]
    cvals = {n: (prop(comp, n) if comp is not None else None) for n in COMP_VARS}

    spd = vals.get("CurrentSpeed")
    spline = vals.get("CurrentFollowSpline")
    stop_at = vals.get("StopatInter")
    stopped = vals.get("StoppedAtIntersection")
    try:
        stopped_lbl = stopped.get_actor_label()[:16] if stopped is not None else "-"
    except Exception:
        stopped_lbl = "?"
    try:
        spline_owner = spline.get_owner().get_actor_label()[:18] if spline is not None else "-"
    except Exception:
        spline_owner = "有" if spline is not None else "-"

    # 我自己的前向探测
    probe = "-"
    if st["ot"] is not None:
        end = unreal.Vector(loc.x + fwd.x * PROBE_AHEAD,
                            loc.y + fwd.y * PROBE_AHEAD,
                            loc.z + fwd.z * PROBE_AHEAD)
        try:
            hits = unreal.SystemLibrary.sphere_trace_multi_for_objects(
                world, loc, end, PROBE_RADIUS, [st["ot"]], False, [car],
                unreal.DrawDebugTrace.NONE, True)
        except Exception:
            hits = []
        for hh in hits or []:
            try:
                ha = hh.to_dict().get("hit_actor")
                hl = ha.get_actor_label() if ha is not None else ""
                dist = hh.to_dict().get("distance", -1)
            except Exception:
                continue
            if hl.startswith("Inter_"):
                hf = ha.get_actor_forward_vector()
                dot = hf.x * fwd.x + hf.y * fwd.y
                probe = "%-30s %6.0fcm dot%+.2f" % (hl[:30], dist, dot)
                break

    st["rows"].append("%7.2f (%6.0f,%6.0f,%6.0f) %8s %6s %-7s %-7s %6s %7s   %s" % (
        t, loc.x, loc.y, loc.z,
        ("%.0f" % spd) if isinstance(spd, (int, float)) else "?",
        spline_owner[:6],
        str(stop_at), stopped_lbl,
        ("%.2f" % cvals["DebugSpeedMultiplier"])
        if isinstance(cvals.get("DebugSpeedMultiplier"), (int, float)) else "?",
        ("%.0f" % cvals["DebugDistanceAhead"])
        if isinstance(cvals.get("DebugDistanceAhead"), (int, float)) else "?",
        probe))


def tick(_delta):
    st = getattr(unreal, KEY, None)
    if st is None:
        return
    now = time.time() - st["t0"]
    if now > DURATION:
        stop()
        return
    if now - st["last_sample"] < SAMPLE_INTERVAL:
        return
    st["last_sample"] = now

    try:
        world = unreal.get_editor_subsystem(
            unreal.UnrealEditorSubsystem).get_game_world()
    except Exception:
        world = None
    if world is None:
        return          # 还没按 Simulate，继续等

    if not st["resolved"]:
        car, names = find_car(world)
        st["resolved"] = True
        if car is None:
            st["notes"].append("!! 没找到名字里带 '%s' 的 actor。" % TARGET)
            st["notes"].append("   世界里带 'car' 的 actor：")
            for n in names[:40]:
                st["notes"].append("     %s" % n)
            write_out(st, True)
            stop()
            return
        st["car"] = car
        st["notes"].append("锁定 %s  /  %s" % (car.get_name(), car.get_actor_label()))
        for c in car.get_components_by_class(unreal.ActorComponent):
            if "TrafficCar" in c.get_class().get_name():
                st["comp"] = c
                st["notes"].append("组件 %s" % c.get_class().get_name())
                break
        if st["comp"] is None:
            st["notes"].append("!! 这辆车上没有 DeliveryTrafficCarComponent")

    try:
        sample(st, world)
    except Exception:
        st["notes"].append("采样异常: %s" % traceback.format_exc()[-300:])

    if now - st["last_write"] > WRITE_EVERY:
        st["last_write"] = now
        write_out(st)


def stop():
    st = getattr(unreal, KEY, None)
    if st is None:
        return
    if st.get("handle") is not None:
        try:
            unreal.unregister_slate_post_tick_callback(st["handle"])
        except Exception:
            pass
    write_out(st, True)
    try:
        delattr(unreal, KEY)
    except Exception:
        pass
    unreal.log("[carmon] 停止，写入 %s" % OUT)


def start():
    old = getattr(unreal, KEY, None)
    if old is not None:
        stop()          # 先停掉上一次，免得挂两份回调
    st = make_state()
    setattr(unreal, KEY, st)
    st["handle"] = unreal.register_slate_post_tick_callback(tick)
    write_out(st)
    unreal.log("[carmon] 已挂上，现在按 Simulate。%.0f 秒后自动停，"
               "或跑 stop_monitor.py 提前停。" % DURATION)


try:
    start()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[carmon] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
