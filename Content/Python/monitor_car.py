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

import math
import time
import traceback

import unreal

CAR_CLASS_NAME = "BP_car_base_C"   # 名字对不上时按类兜底
FALLBACK_TO_ANY = True             # 找不到 TARGET 就监控同类的第一辆
TARGET = "car_base6"      # 按 actor 名字/标签模糊匹配；比对时忽略下划线和大小写，
                          # 所以 "carbase4" / "car_base4" / "BP_car_base4" 都能匹配上
DURATION = 120.0          # 采多久（秒）后自动停
SAMPLE_INTERVAL = 0.1     # 采样间隔（秒）
WRITE_EVERY = 2.0         # 多久落一次盘
PROBE_AHEAD = 3000.0      # 我自己那次探测往前打多远
PROBE_RADIUS = 100.0
FUTURE_GUESS = 1000.0     # 复现未来点时往前多少 cm。蓝图用的是 帧率*车速*Mult，
                          # 帧率那个节点是自定义函数、这里读不到，取个量级相当的常数。
                          # 方向（我们真正要看的东西）对这个值不敏感。
WS_M = unreal.SplineCoordinateSpace.WORLD

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
    cars, fallback, names = [], [], []
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
        is_car_class = a.get_class().get_name() == CAR_CLASS_NAME
        if is_car_class or "car" in norm(nm) or "car" in norm(lbl):
            names.append("%s  /  %s%s"
                         % (nm, lbl, "  [同类]" if is_car_class else ""))
            if want in norm(nm) or want in norm(lbl):
                cars.append(a)
            elif is_car_class:
                fallback.append(a)
    if cars:
        return cars[0], names
    # 名字对不上就退而监控同类的第一辆。整轮空手而归最浪费——
    # 一次 Simulate 的代价远大于监控错一辆车。
    if FALLBACK_TO_ANY and fallback:
        return fallback[0], names
    return None, names


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
    seen = sorted(st.get("seen") or [])
    lines.append("整轮采到过值的蓝图变量: %s" % (", ".join(seen) or "（一个都没有）"))
    never = [n for n in BP_VARS if n not in seen]
    if never:
        lines.append("!! 整轮从没采到值: %s（名字不对，或者这辆车确实没进过那个状态）"
                     % ", ".join(never))
    for n in st["notes"]:
        lines.append(n)
    lines.append("")
    lines.append("%-7s %-26s %8s %-17s %-7s %-7s %6s %7s   %-62s %s" % (
        "时间", "位置", "车速", "样条", "红灯", "停哪", "避让", "前车",
        "复现蓝图：沿线/总长 离样条 样条朝向 车头 差值 末端",
        "我的探测：前方第一个 Inter_"))
    lines.append("-" * 210)
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
    # 变量第一帧是 None 不等于读不到——车刚出生时 CurrentFollowSpline
    # 本来就是 None。累计"整轮下来从没拿到过值"的才算真读不到。
    if st.get("seen") is None:
        st["seen"] = set()
    for n in BP_VARS:
        if vals[n] is not None:
            st["seen"].add(n)

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
    # 组件名是关键：一个 Box 底下挂着 Spline(直行) / SplineLeft / SplineRight 三条，
    # 蓝图是随机挑一条。挑中 SplineRight 时未来点沿右转弧走，探测球自然伸向右前方——
    # 那是正常的右转，不是方向算错。只看 actor 名分不出这三种。
    try:
        if spline is None:
            spline_owner = "-"
        else:
            cn = spline.get_name()
            short = {"Spline": "直", "SplineLeft": "左", "SplineRight": "右"}.get(cn, cn[:3])
            spline_owner = "%s%s" % (
                spline.get_owner().get_actor_label()[:14], short)
    except Exception:
        spline_owner = "有" if spline is not None else "-"

    # 复现蓝图 GetFuturePostionandRotationAlongSpline 的那套计算。
    # 蓝图：Distance = GetDistanceAlongSplineAtLocation(车位置) + 帧率*车速*Mult
    #       Rotation = GetRotationAtDistanceAlongSpline(Distance)
    #       探测终点 = 未来点 + GetForwardVector(Rotation)*200
    # 车说探测球总伸向正右方，所以要比的就是这个 Rotation 的前向和车头朝向。
    rep = "-"
    if spline is not None:
        try:
            L = spline.get_spline_length()
            d0 = spline.get_distance_along_spline_at_location(loc, WS_M)
            # 车离样条有多远：偏太多说明吸附到了不该吸的那条
            near = spline.find_location_closest_to_world_location(loc, WS_M)
            off = ((near.x - loc.x) ** 2 + (near.y - loc.y) ** 2) ** 0.5
            r = spline.get_rotation_at_distance_along_spline(d0, WS_M)
            f = r.get_forward_vector()
            fh = (f.x * f.x + f.y * f.y) ** 0.5
            cyaw = math.degrees(math.atan2(fwd.y, fwd.x))
            if fh > 1e-4:
                syaw = math.degrees(math.atan2(f.y / fh, f.x / fh))
                diff = (syaw - cyaw + 180.0) % 360.0 - 180.0
            else:
                syaw, diff = 999.0, 999.0
            # 末端判定：蓝图拿未来点和"最后一个控制点"比，容差 100
            n = spline.get_number_of_spline_points()
            last = spline.get_location_at_spline_point(n - 1, WS_M)
            fut = spline.get_location_at_distance_along_spline(
                min(d0 + FUTURE_GUESS, L), WS_M)
            at_end = (((fut.x - last.x) ** 2 + (fut.y - last.y) ** 2
                       + (fut.z - last.z) ** 2) ** 0.5) < 100.0
            rep = ("d%.0f/%.0f 偏%.0f 样条%.1f° 车%.1f° 差%+.1f° %s"
                   % (d0, L, off, syaw, cyaw, diff,
                      "延伸" if at_end else "零长"))
        except Exception as exc:
            rep = "算不了:%s" % str(exc)[:28]

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

    st["rows"].append(
        "%7.2f (%6.0f,%6.0f,%6.0f) %8s %-17s %-7s %-7s %6s %7s   %-62s %s" % (
        t, loc.x, loc.y, loc.z,
        ("%.0f" % spd) if isinstance(spd, (int, float)) else "?",
        spline_owner[:17],
        str(stop_at), stopped_lbl,
        ("%.2f" % cvals["DebugSpeedMultiplier"])
        if isinstance(cvals.get("DebugSpeedMultiplier"), (int, float)) else "?",
        ("%.0f" % cvals["DebugDistanceAhead"])
        if isinstance(cvals.get("DebugDistanceAhead"), (int, float)) else "?",
        rep, probe))


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
        st["notes"].append("【状态】采样已开始（游戏世界已出现）。")
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
    # 这一笔是"已挂上、还没开始采"的快照。不标清楚的话，它和
    # "跑完了但一行都没采到"长得一模一样，会白白误判一轮。
    st["notes"].append("【状态】已挂上回调，正在等 Simulate——现在按 Simulate。")
    st["notes"].append("       这行还在 = 一帧都没采到（游戏世界还没出现）。")
    write_out(st)
    st["notes"].pop()
    st["notes"].pop()
    unreal.log("[carmon] 已挂上，现在按 Simulate。%.0f 秒后自动停，"
               "或跑 stop_monitor.py 提前停。" % DURATION)


try:
    start()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[carmon] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
