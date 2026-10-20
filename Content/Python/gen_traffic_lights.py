# -*- coding: utf-8 -*-
"""在路口摆红绿灯模型，替代现在那个 DrawDebugSphere 线框球。

做两件事
--------
① **摆灯**：在每个 BP_Intersection 的中心放 4 个 /Game/trafficlight/trafficlight，
   各自面向一个进入方向（面朝来车）。方向不是拍脑袋定的，是从该路口管辖的
   Inter_* 段的实际走向算出来的。
② **把路口的变量值读出来**（只读）：LightNumber / LightDuration / StopAtInter /
   ChildNodes 当前是什么。同步逻辑要靠这些，但它们的语义还没确认——
   与其猜，不如先把实际值打出来。

为什么是 4 个灯、只有 2 个状态
------------------------------
LightNumber 只有 0/1（gen_traffic_lanes.py 的 LIGHT_NUMBER_BY_AXIS：沿 Y 的
路口段=1、沿 X 的=0），所以一个路口只有两个相位：东西向 vs 南北向交替。
对向的两个方向永远同色。4 个灯模型，2 个布尔驱动。

朝向的坑
--------
灯模型的"正面"朝哪个轴**还没确认**。摩托车在这上面栽过一次（车头朝 +Y、
Pawn 朝 +X，差 90 度，表现是车横着走）。所以这里留了 MODEL_YAW_OFFSET，
报告会打印每个灯用的最终 yaw；第一次摆完先在视口里看一眼灯头冲哪，
不对就改这一个数再跑（脚本可重复运行，会先删掉上次摆的）。

只在 REQUIRE_LEVEL 指定的关卡里摆，跑错图直接退出。
用法：py gen_traffic_lights.py      （DRY_RUN=True 只看不摆）
跑完 **Ctrl+S**。
"""

import math
import traceback

import unreal

DRY_RUN = False
REQUIRE_LEVEL = "testfortraffic"        # 只动这张图
LIGHT_BP = "/Game/trafficlight/trafficlight"
# 「先放行」那组用的子类：和 LIGHT_BP 唯一的差别是 CDO 里 start color = 绿。
# 由 make_light_variant.py 生成，**先跑那个再跑本脚本**。
# 为什么要两个类而不是在实例上写 start color：那个蓝图变量没开
# Instance Editable，实例写入被 PropertyAccessUtil 挡死，只有 CDO 能写。
GREEN_START_BP = "/Game/trafficlight/trafficlight_green_start"
# 哪一组先放行（从绿开始）。必须和 tune_traffic_lights.py 里的同名常量一致，
# 否则两个脚本会各按各的分组走，表现是"某个方向灯绿但车不走"。
GREEN_FIRST_GROUP = 1
INTERSECTION_PREFIX = "BP_Intersection"  # 按类名前缀认路口
LANE_TAG_PREFIX = "ClaudeGenLane"          # 脚本生成的车道才有
CHILD_CLASS_PREFIX = "BP_TrafficLine1_IntersectionChild"   # 手摆的按类名认
TAG = "ClaudeGenTrafficLight"

CENTER_RADIUS = 0.0      # 灯离路口中心多远。0 = 正中央四面背靠背
LIGHT_Z = 0.0            # 相对路口 actor 的高度微调
MODEL_YAW_OFFSET = 0.0   # 模型正面不是 +X 时在这里补；先跑一次看视口再定
CHILD_SEARCH_RADIUS = 3000.0   # 多远以内的 Inter_ 段算这个路口的

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "gen_traffic_lights.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[lights] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def read_prop(obj, name):
    """读蓝图变量。

    **蓝图变量要用原名**（LightNumber），不是 snake_case（light_number）——
    Python 绑定的 snake_case 转换只对 C++ 的 UPROPERTY 有效，蓝图自定义变量
    按原样注册。第一版全用 snake_case，结果四个变量全是 <读不到>。
    这里两种都试一遍，省得再为大小写来回跑。
    """
    for candidate in (name, name.replace("_", ""), "".join(
            w.capitalize() for w in name.split("_"))):
        try:
            return obj.get_editor_property(candidate)
        except Exception:
            continue
    return u"<读不到>"


def dump_intersection(inter):
    """把路口的变量值打出来。同步逻辑要用这些，先确认语义再写代码。"""
    w(u"  路口变量当前值：")
    for name in ("LightNumber", "LightDuration", "StopAtInter", "ChildNodes"):
        val = read_prop(inter, name)
        if isinstance(val, (list, tuple)):
            w(u"    %-16s 共 %d 项" % (name, len(val)))
        else:
            w(u"    %-16s %s" % (name, val))


def child_directions(inter, lanes):
    """这个路口管的 Inter_ 段，各自的水平走向 + LightNumber。

    走向用样条首尾两点作差，不用 get_direction_at_distance_along_spline——
    那个函数在端点返回零向量（CURVE_CLAMPED 把端点切线归零，CLAUDE.md 记过）。
    """
    loc = inter.get_actor_location()
    out = []
    for a in lanes:
        label = a.get_actor_label()
        # 脚本生成的叫 Inter_*，手摆的按类名认——两种都要。
        if not (label.startswith("Inter_")
                or a.get_class().get_name().startswith(CHILD_CLASS_PREFIX)):
            continue
        p = a.get_actor_location()
        if math.sqrt((p.x - loc.x) ** 2 + (p.y - loc.y) ** 2) > CHILD_SEARCH_RADIUS:
            continue
        sp = a.get_component_by_class(unreal.SplineComponent)
        if sp is None or sp.get_number_of_spline_points() < 2:
            continue
        q0 = sp.get_location_at_distance_along_spline(0.0, WS)
        q1 = sp.get_location_at_distance_along_spline(
            min(200.0, sp.get_spline_length()), WS)
        dx, dy = q1.x - q0.x, q1.y - q0.y
        h = math.sqrt(dx * dx + dy * dy)
        if h < 1e-3:
            continue
        out.append((label, dx / h, dy / h, read_prop(a, "LightNumber")))
    return out


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    name = world.get_name() if world else u"?"
    w(u"=== 路口红绿灯摆放 ===")
    w(u"关卡：%s     模式：%s" % (name, u"只看不摆（DRY_RUN）" if DRY_RUN else u"**写入**"))
    if name != REQUIRE_LEVEL:
        w(u"！这个脚本只在 %s 里摆灯，当前是 %s，退出。" % (REQUIRE_LEVEL, name))
        return

    light_class = unreal.EditorAssetLibrary.load_blueprint_class(LIGHT_BP)
    if light_class is None:
        w(u"！加载不到 %s 的生成类。" % LIGHT_BP)
        return

    actors = eas.get_all_level_actors()
    inters = [a for a in actors
              if a.get_class().get_name().startswith(INTERSECTION_PREFIX)]
    # 按**类名**找路口段，不按标签。标签 ClaudeGenLane 只有脚本生成的车道才有，
    # 这张图里的路口段是手摆的，一个标签都没有——第一版因此找到 0 个。
    lanes = [a for a in actors
             if a.get_class().get_name().startswith(CHILD_CLASS_PREFIX)
             or any(str(t).startswith(LANE_TAG_PREFIX) for t in a.tags)]
    w(u"找到路口 %d 个，车道/路口段 %d 个" % (len(inters), len(lanes)))
    if not lanes:
        # 找不到就把关卡里有什么都列出来。来回猜判据已经浪费过两轮了：
        # 先按标签找（0 个）、再按类名找——与其第三次猜，不如一次把现场打出来。
        w(u"")
        w(u"！一个路口段都没找到。关卡里实际有这些类（按数量）：")
        counts = {}
        for a in actors:
            cn = a.get_class().get_name()
            counts[cn] = counts.get(cn, 0) + 1
        for cn, n in sorted(counts.items(), key=lambda kv: -kv[1])[:18]:
            w(u"    %-46s %d 个" % (cn[:46], n))
        w(u"  对照 CHILD_CLASS_PREFIX（当前 %s）改一下再跑。" % CHILD_CLASS_PREFIX)
    if not inters:
        w(u"！这张图里没有 %s* 的 actor。" % INTERSECTION_PREFIX)
        return

    # 先删掉上次摆的，保证可重复运行
    removed = 0
    for a in actors:
        if TAG in [str(t) for t in a.tags]:
            if not DRY_RUN:
                eas.destroy_actor(a)
            removed += 1
    if removed:
        w(u"先删掉上次摆的 %d 个灯" % removed)

    placed = 0
    for inter in inters:
        loc = inter.get_actor_location()
        w(u"")
        w(u"── %s @ (%.0f, %.0f, %.0f)"
          % (inter.get_actor_label(), loc.x, loc.y, loc.z))
        dump_intersection(inter)

        dirs = child_directions(inter, lanes)
        w(u"  管辖的 Inter_ 段 %d 个：" % len(dirs))
        # 按走向归到四个方位，同方位只留一个代表
        buckets = {}
        for label, dx, dy, ln in dirs:
            yaw = math.degrees(math.atan2(dy, dx))
            key = int(round(yaw / 90.0)) % 4          # 0=+X 1=+Y 2=-X 3=-Y
            buckets.setdefault(key, []).append((label, yaw, ln))
        for key in sorted(buckets):
            items = buckets[key]
            lns = sorted({str(x[2]) for x in items})
            w(u"    方位%d（约 %4.0f°）：%d 段，LightNumber=%s，例 %s"
              % (key, items[0][1], len(items), u"/".join(lns), items[0][0][:28]))

        if DRY_RUN:
            continue

        green_class = None
        if unreal.EditorAssetLibrary.does_asset_exist(GREEN_START_BP):
            green_class = unreal.EditorAssetLibrary.load_blueprint_class(GREEN_START_BP)
        if green_class is None:
            w(u"  ！找不到绿起始子类 %s —— 四盏灯会全用父类、全部红起始，" % GREEN_START_BP)
            w(u"    也就分不出两组。先跑 py make_light_variant.py 再跑本脚本。")

        for key in sorted(buckets):
            items = buckets[key]
            travel_yaw = items[0][1]
            # 车沿 travel_yaw 开过来，灯要面朝来车 → 转 180 度
            light_yaw = travel_yaw + 180.0 + MODEL_YAW_OFFSET
            rad = math.radians(travel_yaw)
            # 往来车方向退 CENTER_RADIUS，站在该方向那一侧
            px = loc.x - math.cos(rad) * CENTER_RADIUS
            py = loc.y - math.sin(rad) * CENTER_RADIUS
            # Rotator 的构造参数是 (roll, pitch, yaw)，和 C++ 的 FRotator 顺序不同。
            # 填错位置会把 yaw 当成 pitch，灯直接躺下——CLAUDE.md 记过这个坑。
            rot = unreal.Rotator(0.0, 0.0, light_yaw)
            # 起始色由类决定：先放行那组用绿起始子类，另一组用父类（红起始）。
            try:
                group = int(items[0][2])
            except Exception:
                group = 0
            use_green = (group % 2) == (GREEN_FIRST_GROUP % 2)
            cls = green_class if (use_green and green_class is not None) else light_class
            a = eas.spawn_actor_from_class(
                cls, unreal.Vector(px, py, loc.z + LIGHT_Z), rot)
            if a is None:
                w(u"    ！方位%d 生成失败" % key)
                continue
            a.set_actor_label(u"TrafficLight_%s_%d" % (inter.get_actor_label()[-6:], key))
            a.tags = [TAG, u"%s:%s" % (TAG, items[0][2])]
            placed += 1
            w(u"    方位%d 摆好：组%s 起始%s  yaw %.0f（行进 %.0f + 180 + 偏移 %.0f）"
              % (key, items[0][2], u"绿" if (use_green and green_class is not None)
                 else u"红", light_yaw, travel_yaw, MODEL_YAW_OFFSET))

    w(u"")
    if DRY_RUN:
        w(u"只是预览，什么都没摆。DRY_RUN 改 False 再跑。")
        return
    w(u"共摆了 %d 个灯。**记得 Ctrl+S。**" % placed)
    w(u"")
    w(u"下一步要确认两件事：")
    w(u"  1. 灯头朝向对不对。不对就改 MODEL_YAW_OFFSET（90/180/-90）重跑。")
    w(u"  2. 上面打印的路口变量值——同步逻辑要靠它们，确认语义后才好写。")
    w(u"     重点看 light_number：如果它随时间在 0/1 之间跳，那它就是「当前哪个相位绿灯」。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[lights] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[lights] 写入 %s" % OUT)
