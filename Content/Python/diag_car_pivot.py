# -*- coding: utf-8 -*-
"""量每种车型的「网格底面离 actor 原点多远」，这是车浮空里和车道无关的那一半。

为什么要单独量这个
------------------
车在运行时的高度**每帧由样条决定**（BP_car_base 沿样条 SetActorLocation），
所以挪 actor 只对静止的车有效，一开起来就被样条覆盖。真正决定"看起来浮多高"
的是 actor 原点到车身网格底面的距离——这个量存在**蓝图**里，只能在蓝图里改。

实测过的背景数字（diag_car_float.py，2026-09-24）：
    车底面高出路面：平均 +14.8，最小 -18.8，最大 +81.3
平均那 14.8 是车道的 Z_OFFSET=15 带来的系统性偏移（已经整体降 13 解决），
但 ±80 的离散度是各车型 pivot 不一致造成的，降车道治不了。

量法
----
对每个车型的一个关卡实例：取它所有 StaticMeshComponent 的世界包围盒，
底面最低的那个减去 actor 原点 Z。结果 = 把 actor 放在路面上时，车会浮多高。
理想值 0（pivot 正好在轮子底下）。

只读。用法：py diag_car_pivot.py
"""

import traceback

import unreal

CAR_CLASS_PREFIX = "BP_car_base"
OUT = unreal.Paths.project_saved_dir() + "diag_car_pivot.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[pivot] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def mesh_bottom(actor):
    """所有静态网格组件里最低的底面（世界 Z），以及参与统计的组件数。

    只看 StaticMeshComponent，不用 get_actor_bounds——后者会把碰撞 Box
    算进去，而 Box 的上下范围和看得见的车身没关系（车道那边的 Box 半高
    是 600，足以说明这类盒子有多不可信）。
    """
    lowest = None
    count = 0
    for c in actor.get_components_by_class(unreal.StaticMeshComponent):
        if c.get_editor_property("static_mesh") is None:
            continue
        try:
            origin, extent = c.k2_get_component_bounds()[:2]
        except Exception:
            # 退回用组件世界位置 + 网格本地包围盒
            try:
                sm = c.get_editor_property("static_mesh")
                b = sm.get_bounding_box()
                loc = c.get_world_location()
                sc = c.get_world_scale().z
                origin = unreal.Vector(loc.x, loc.y,
                                       loc.z + (b.min.z + b.max.z) * 0.5 * sc)
                extent = unreal.Vector(0, 0, (b.max.z - b.min.z) * 0.5 * abs(sc))
            except Exception:
                continue
        bottom = origin.z - extent.z
        lowest = bottom if lowest is None else min(lowest, bottom)
        count += 1
    return lowest, count


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    by_class = {}
    for a in eas.get_all_level_actors():
        cls = a.get_class().get_name()
        if not cls.startswith(CAR_CLASS_PREFIX):
            continue
        by_class.setdefault(cls, []).append(a)

    w(u"=== 车型 pivot 诊断：网格底面离 actor 原点多远 ===")
    if not by_class:
        w(u"这张图里没有 %s* 的车。" % CAR_CLASS_PREFIX)
        return
    w(u"理想值 0 = pivot 正好在轮子底下；正数 = 车会浮这么高；负数 = 会埋进去。")
    w(u"")
    w(u"%-18s %5s %10s %10s %10s %8s"
      % (u"车型", u"辆数", u"actorZ", u"网格底Z", u"差值", u"网格数"))

    fixes = []
    for cls in sorted(by_class):
        actors = by_class[cls]
        a = actors[0]
        az = a.get_actor_location().z
        bottom, n = mesh_bottom(a)
        if bottom is None:
            w(u"%-18s %5d %10.1f %10s %10s %8d  ← 读不到网格"
              % (cls[:18], len(actors), az, u"-", u"-", n))
            continue
        dev = bottom - az
        w(u"%-18s %5d %10.1f %10.1f %+10.1f %8d"
          % (cls[:18], len(actors), az, bottom, dev, n))
        if abs(dev) > 3.0:
            fixes.append((cls, dev))

    w(u"")
    w(u"—— 结论 ——")
    if not fixes:
        w(u"所有车型的 pivot 都在轮子底下（差值都在 ±3 以内），")
        w(u"那浮空就不是 pivot 的问题，另找原因。")
    else:
        w(u"以下车型的网格需要在**蓝图里**往下移（改网格组件的相对 Z）：")
        for cls, dev in fixes:
            w(u"    %-18s 相对 Z %+.1f" % (cls[:18], -dev))
        w(u"")
        w(u"为什么必须在蓝图里改、而不是挪关卡里的车：")
        w(u"  车开起来之后高度每帧由样条决定，actor 位置会被覆盖；")
        w(u"  只有 actor 原点到网格的相对偏移是跟着蓝图走的，改它才对行驶中的车有效。")
        w(u"注意每种车型只抽了一个实例来量。若同车型的实例缩放不同，")
        w(u"差值会跟着缩放变，那就得按实例处理而不是按车型。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[pivot] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[pivot] 写入 %s" % OUT)
