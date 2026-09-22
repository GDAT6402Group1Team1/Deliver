# -*- coding: utf-8 -*-
"""清掉关卡里所有 BP_car_base，然后在车道段起点重新零散摆一批做测试。

做法：
  1. 按**类**（BP_car_base_C）删，不按名字——关卡里的车实例名字五花八门
     （carbase4 / BP_car_base2 / ...），按标签匹配漏得到处都是。
  2. 在挑中的 Lane_ 段起点**往后** BEHIND_BOX 处各放一辆，朝向取样条方向。
     必须放在起点之后（也就是盒子之前）：Box 挂在第一个控制点上，
     车出生时 CurrentFollowSpline 是 None，靠 TraceForNewPath 往前扫，
     放在起点之后等于一出生就开过了自己的盒子，永远接不上路。
     放在**车道段**而不是路口段：路段起点附近最干净，没有横向车流的盒子干扰。

挑哪些车道：以 ANCHOR_ROAD 为中心，按距离排序，每隔 STRIDE 条取一条，
最多 MAX_CARS 辆。"每隔几条取一条"是为了零散——挨着放的话几辆车
一出生就互相判定为前车、集体停住，测不出东西。

只影响 BP_car_base 实例，不碰交通线。
用法：py spawn_test_cars.py
"""

import math
import random
import traceback

import unreal

CAR_BP = "/Game/Blueprint/BP_car_base"
CAR_CLASS_NAME = "BP_car_base_C"
LANE_TAG_PREFIX = "ClaudeGenLane"

ANCHOR_ROAD = "形状13"     # 以这条路为中心往外挑；"" = 全图均匀挑
MAX_CARS = 20
STRIDE = 3                 # 每隔几条车道放一辆
BEHIND_BOX = 300.0         # 放在车道段起点**往后**这么多的地方。
                           # Box 就挂在第一个控制点上（"起点探测把手"），
                           # 放在起点之后等于车一出生就开过了自己的盒子，
                           # 前向探测扫不到，TraceForNewPath 永远接不上路。
                           # 往后退一点，盒子才在车前方。
CAR_Z = 0.0                # 车高度微调。车道点本身已经在路面上方 15cm
MIN_LANE_LEN = 1500.0      # 太短的段不放（车还没加速就到头了）
TAG = "ClaudeTestCar"

# 车速随机：600~1500 的 100 倍数。MaxSpeed 在整张蓝图里没有 Set 节点、
# 是纯配置变量，所以实例上改了就一直有效。
SPEED_MIN, SPEED_MAX, SPEED_STEP = 600, 1500, 100
# 颜色怎么给还不确定（蓝图变量？材质参数？），按下面的顺序试，
# 报告里会写明实际走通的是哪条。互相区分得开就行，不追求好看。
PALETTE = [
    (0.90, 0.10, 0.10), (0.10, 0.45, 0.95), (0.95, 0.75, 0.10),
    (0.15, 0.70, 0.25), (0.85, 0.35, 0.85), (0.20, 0.85, 0.85),
    (0.95, 0.50, 0.10), (0.55, 0.25, 0.85), (0.95, 0.95, 0.95),
    (0.15, 0.15, 0.18),
]
COLOR_PARAM_HINTS = ["color", "colour", "tint", "basecolor", "base_color"]


def describe_car(car):
    """第一辆车上到底有什么可以用来上色的东西，原样记下来。"""
    out = []
    try:
        names = [str(n) for n in
                 unreal.BlueprintEditorLibrary.list_member_variable_names(
                     unreal.EditorAssetLibrary.load_asset(CAR_BP))]
        out.append("蓝图成员变量 %d 个：%s" % (len(names), ", ".join(names[:30])))
        hits = [n for n in names
                if any(h in n.lower() for h in COLOR_PARAM_HINTS)]
        out.append("名字像颜色的：%s" % (", ".join(hits) or "（没有）"))
    except Exception as exc:
        out.append("列不出蓝图成员变量：%s" % str(exc)[:60])
    for c in car.get_components_by_class(unreal.PrimitiveComponent):
        try:
            mats = c.get_materials()
        except Exception:
            mats = []
        out.append("  组件 %-26s %s  材质 %d 个：%s"
                   % (c.get_name()[:26], c.get_class().get_name(), len(mats),
                      ", ".join((m.get_name() if m else "None")
                                for m in mats[:3])))
    return out


def paint(car, rgb):
    """给车上色。返回走通的是哪条路。"""
    col = unreal.LinearColor(rgb[0], rgb[1], rgb[2], 1.0)
    # 1) 蓝图上有颜色变量就直接写，这种最干净、存得住
    try:
        names = [str(n) for n in
                 unreal.BlueprintEditorLibrary.list_member_variable_names(
                     unreal.EditorAssetLibrary.load_asset(CAR_BP))]
    except Exception:
        names = []
    for n in names:
        if any(h in n.lower() for h in COLOR_PARAM_HINTS):
            try:
                car.set_editor_property(n, col)
                return "蓝图变量 %s" % n
            except Exception:
                continue
    # 2) 退而求其次：往组件的材质上写向量参数。这会在内部建 MID，
    #    编辑器里未必存得住（重载可能掉回原色），但 Simulate 期间够用。
    done = []
    for c in car.get_components_by_class(unreal.PrimitiveComponent):
        for pname in ("BaseColor", "Color", "Tint", "BodyColor"):
            try:
                c.set_vector_parameter_value_on_materials(pname, col)
                done.append("%s.%s" % (c.get_name()[:14], pname))
                break
            except Exception:
                continue
    if done:
        return "材质参数 " + ", ".join(done[:2])
    return "没上成"

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "test_cars.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    # --- 1. 删掉所有车 ---
    cars = [a for a in actors if a.get_class().get_name() == CAR_CLASS_NAME]
    w("找到 %s 实例 %d 个" % (CAR_CLASS_NAME, len(cars)))
    for a in cars[:12]:
        loc = a.get_actor_location()
        w("   %-28s (%.0f, %.0f, %.0f)"
          % (a.get_actor_label()[:28], loc.x, loc.y, loc.z))
    if len(cars) > 12:
        w("   ... 还有 %d 个" % (len(cars) - 12))

    killed = 0
    for a in cars:
        try:
            eas.destroy_actor(a)
            killed += 1
        except Exception as exc:
            w("!! 删不掉 %s: %s" % (a.get_actor_label(), str(exc)[:50]))
    w("已删除 %d 个" % killed)
    w("")
    flush()

    # --- 2. 收集候选车道段 ---
    lanes = []
    for a in actors:
        try:
            if not unreal.SystemLibrary.is_valid(a) or a.is_actor_being_destroyed():
                continue
        except Exception:
            pass
        if not any(str(t).startswith(LANE_TAG_PREFIX) for t in a.tags):
            continue
        lbl = a.get_actor_label()
        if not lbl.startswith("Lane_"):
            continue
        sp = a.get_component_by_class(unreal.SplineComponent)
        if sp is None or sp.get_number_of_spline_points() < 2:
            continue
        if sp.get_spline_length() < MIN_LANE_LEN:
            continue
        lanes.append((lbl, sp))

    w("可用车道段 %d 条（长度 >= %.0f）" % (len(lanes), MIN_LANE_LEN))
    if not lanes:
        w("!! 一条都没有，先跑 gen_traffic_lanes.py")
        flush()
        return

    # 按离锚定道路的距离排序：近的先来，所以 MAX_CARS 截断之后
    # 剩下的都聚在 ANCHOR_ROAD 附近
    if ANCHOR_ROAD:
        anchor_pts = [sp.get_location_at_distance_along_spline(
                          sp.get_spline_length() * 0.5, WS)
                      for lbl, sp in lanes if ANCHOR_ROAD in lbl]
        if anchor_pts:
            cx = sum(p.x for p in anchor_pts) / len(anchor_pts)
            cy = sum(p.y for p in anchor_pts) / len(anchor_pts)
            w("锚点 %s 的中心 (%.0f, %.0f)，按离它的距离排序"
              % (ANCHOR_ROAD, cx, cy))

            def key(t):
                p = t[1].get_location_at_distance_along_spline(0.0, WS)
                return (p.x - cx) ** 2 + (p.y - cy) ** 2

            lanes.sort(key=key)
        else:
            w("!! 没有名字含 %s 的车道段，改为全图顺序" % ANCHOR_ROAD)
            lanes.sort(key=lambda t: t[0])
    else:
        lanes.sort(key=lambda t: t[0])

    picked = lanes[::STRIDE][:MAX_CARS]
    w("每隔 %d 条取一条，共挑中 %d 条" % (STRIDE, len(picked)))
    w("")
    flush()

    # --- 3. 放车 ---
    bp = unreal.EditorAssetLibrary.load_asset(CAR_BP)
    if bp is None:
        w("!! 载入不了 %s" % CAR_BP)
        flush()
        return

    speeds = list(range(SPEED_MIN, SPEED_MAX + 1, SPEED_STEP))
    w("车速从 %s 里随机取" % speeds)
    w("")
    w("%-4s %-30s %-26s %7s %6s %-22s %s"
      % ("#", "车道段", "位置", "朝向", "车速", "颜色", "上色方式"))
    w("-" * 130)
    made = 0
    described = False
    paint_ways = {}
    for i, (lbl, sp) in enumerate(picked):
        # 起点处的行进方向：用起点和往前 100cm 两个点作差。
        # 不用 get_direction_at_distance_along_spline——它在端点返回零向量
        # （端点切线被 CURVE_CLAMPED 归零，见 CLAUDE.md）。
        p = sp.get_location_at_distance_along_spline(0.0, WS)
        q = sp.get_location_at_distance_along_spline(
            min(100.0, sp.get_spline_length()), WS)
        dx, dy = q.x - p.x, q.y - p.y
        h = (dx * dx + dy * dy) ** 0.5
        if h < 1e-3:
            w("   %-34s 取不到朝向，跳过" % lbl[:34])
            continue
        dx, dy = dx / h, dy / h
        yaw = math.degrees(math.atan2(dy, dx))
        # 沿行进方向**倒退** BEHIND_BOX，把盒子留在车前方
        loc = unreal.Vector(p.x - dx * BEHIND_BOX,
                            p.y - dy * BEHIND_BOX,
                            p.z + CAR_Z)
        rot = unreal.Rotator(0.0, 0.0, yaw)
        try:
            car = eas.spawn_actor_from_object(bp, loc, rot)
        except Exception as exc:
            w("   %-34s !! spawn 失败：%s" % (lbl[:34], str(exc)[:50]))
            continue
        if car is None:
            w("   %-34s !! spawn 返回 None" % lbl[:34])
            continue
        try:
            car.set_actor_label("TestCar_%02d" % i)
            tags = list(car.tags)
            tags.append(TAG)
            car.set_editor_property("tags", tags)
        except Exception:
            pass

        # 第一辆车先把"有什么可以上色的"原样记下来，
        # 免得上色失败时只知道失败、不知道该往哪改。
        if not described:
            described = True
            for ln in describe_car(car):
                w("   %s" % ln)
            w("")

        spd = random.choice(speeds)
        try:
            car.modify(True)
            car.set_editor_property("MaxSpeed", float(spd))
        except Exception as exc:
            w("   !! MaxSpeed 写不进去：%s" % str(exc)[:50])
            spd = -1

        rgb = PALETTE[i % len(PALETTE)]
        way = paint(car, rgb)
        paint_ways[way] = paint_ways.get(way, 0) + 1

        made += 1
        w("%-4d %-30s (%7.0f,%8.0f,%7.0f) %7.1f %6s %-22s %s"
          % (i, lbl[:30], loc.x, loc.y, loc.z, yaw,
             ("%d" % spd) if spd > 0 else "失败",
             "%.2f/%.2f/%.2f" % rgb, way))

    w("")
    w("=" * 78)
    w("删掉 %d 辆旧车，放置 %d 辆新车（标签 TestCar_xx，tag %s）"
      % (killed, made, TAG))
    w("上色方式统计：%s"
      % ("；".join("%s × %d" % kv for kv in paint_ways.items()) or "（没上成）"))
    if "没上成" in paint_ways:
        w("!! 有 %d 辆没上成色。上面第一辆车的组件/材质清单里能看出"
          % paint_ways["没上成"])
        w("   材质的颜色参数实际叫什么名字，把它加进 paint() 的候选名单即可。")
    w("")
    w("颜色若是走\"材质参数\"那条路，它内部建的是 MID，编辑器里未必存得住——")
    w("Simulate 期间肯定有效，关卡重载后可能掉回原色，重跑本脚本即可。")
    w("")
    w("车出生时 CurrentFollowSpline 是 None，靠 TraceForNewPath 就近找路，")
    w("所以放在车道段中间比放在路口段稳。跑 Simulate 看它们能不能接上。")
    w("接不上的话先看车是不是陷进路面了——CAR_Z 当前是 %.0f，" % CAR_Z)
    w("车道点本身在路面上方 15cm，车的原点如果在车底就够，在车中心就要抬。")
    w("关卡尚未保存。")
    flush()
    unreal.log("[cars] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[cars] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
