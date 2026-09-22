# -*- coding: utf-8 -*-
"""量交通样条的切线和旋转到底对不对。

起因：车的探测球在路口总是伸向正右方。方向来自蓝图
GetFuturePostionandRotationAlongSpline 里的 GetRotationAtDistanceAlongSpline，
而引擎那个函数内部是 MakeFromXZ(切线, Up)——**切线为零时构造退化**，
得到的前向会跑到一个和真实走向垂直的方向上。

CLAUDE.md 里已经有一条实测结论：get_direction_at_distance_along_spline
在这些样条上返回零向量。当时是绕过去的（改用两个采样点作差），
没往上游追。如果切线真的是零，这两个现象就是同一个根因。

每条样条在几个位置上同时取三样东西对照：
    切线     get_tangent_at_distance_along_spline 的长度
    旋转前向 get_rotation_at_distance_along_spline 转成的前向量
    几何方向 前后两个采样点作差（这个一定是对的）
然后报 旋转前向 与 几何方向 的夹角。**接近 90 度就是退化**。

只读，什么都不改。
用法：py diag_spline_tangent.py
"""

import math
import traceback

import unreal

LANE_TAG_PREFIX = "ClaudeGenLane"
SAMPLES_PER = 3          # 内部再取几个位置（端点固定会测）
MAX_PER_KIND = 0         # 每类看几条；0 = 全部
DELTA = 50.0             # 求几何方向的采样间距
BAD_ANGLE = 30.0         # 夹角超过这个度数就算不对

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "spline_tangent.txt"
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
    lanes, inters, turns = [], [], []
    for a in eas.get_all_level_actors():
        if not any(str(t).startswith(LANE_TAG_PREFIX) for t in a.tags):
            continue
        lbl = a.get_actor_label()
        for c in a.get_components_by_class(unreal.SplineComponent):
            if c.get_number_of_spline_points() < 2:
                continue
            cn = c.get_name()
            if cn in ("SplineLeft", "SplineRight"):
                turns.append((lbl + "." + cn, c))
            elif lbl.startswith("Lane_"):
                lanes.append((lbl, c))
            elif lbl.startswith("Inter_"):
                inters.append((lbl, c))

    w("车道段 %d、路口段 %d、转弯 %d" % (len(lanes), len(inters), len(turns)))
    w("每条取 %d 个位置；几何方向用相距 %.0fcm 的两点作差" % (SAMPLES_PER, DELTA))
    w("旋转前向和几何方向的夹角接近 90 度 = MakeFromXZ 退化（切线为零）")
    w("")
    flush()

    totals = {}
    for kind, items in (("Lane_", lanes), ("Inter_", inters), ("转弯", turns)):
        w("=" * 100)
        w("%s  （共 %d 条，看 %s）"
          % (kind, len(items), "全部" if not MAX_PER_KIND else MAX_PER_KIND))
        w("=" * 100)
        w("%-40s %8s %8s %9s %9s %7s"
          % ("样条", "沿线", "切线长", "旋转前向°", "几何方向°", "夹角"))
        w("-" * 100)
        zero_tan = bad = n = 0
        for name, sp in (items if not MAX_PER_KIND else items[:MAX_PER_KIND]):
            L = sp.get_spline_length()
            # 端点必须单独测。车的探测球只在"未来点被钳到样条末端"的那一两帧
            # 才真的延伸出去，那一瞬间求值的位置就是 d=L。之前只采
            # L*(k+0.5)/N 的内部点，把唯一出问题的地方跳过去了。
            ds = [0.0, 1.0, L * 0.5, L - 1.0, L]
            ds += [L * (k + 0.5) / SAMPLES_PER for k in range(SAMPLES_PER)]
            for d in ds:
                d = max(0.0, min(L, d))
                t = sp.get_tangent_at_distance_along_spline(d, WS)
                tl = (t.x * t.x + t.y * t.y + t.z * t.z) ** 0.5

                r = sp.get_rotation_at_distance_along_spline(d, WS)
                f = r.get_forward_vector()

                a0 = sp.get_location_at_distance_along_spline(
                    max(0.0, d - DELTA), WS)
                a1 = sp.get_location_at_distance_along_spline(
                    min(L, d + DELTA), WS)
                gx, gy = a1.x - a0.x, a1.y - a0.y
                gh = (gx * gx + gy * gy) ** 0.5
                if gh < 1e-4:
                    continue
                gx, gy = gx / gh, gy / gh

                fh = (f.x * f.x + f.y * f.y) ** 0.5
                if fh < 1e-4:
                    ang = 999.0
                    fdeg = 999.0
                else:
                    fx, fy = f.x / fh, f.y / fh
                    dot = max(-1.0, min(1.0, fx * gx + fy * gy))
                    ang = math.degrees(math.acos(dot))
                    fdeg = math.degrees(math.atan2(fy, fx))
                gdeg = math.degrees(math.atan2(gy, gx))

                n += 1
                if tl < 1e-3:
                    zero_tan += 1
                if ang > BAD_ANGLE:
                    bad += 1
                is_edge = d <= 1.0 or d >= L - 1.0
                # 全图模式（MAX_PER_KIND=0）下只打印有问题的，
                # 否则几千行端点会把报告淹掉；抽查模式下端点一律打印。
                if ang > BAD_ANGLE or (is_edge and MAX_PER_KIND):
                    w("%-40s %8.0f %8.1f %9.1f %9.1f %7.1f"
                      % (name[:40], d, tl, fdeg, gdeg, ang))
        totals[kind] = (n, zero_tan, bad)
        w("")
        flush()

    w("=" * 100)
    w("合计")
    w("=" * 100)
    for kind, (n, zt, bad) in totals.items():
        if not n:
            continue
        w("%-8s 采样 %4d 个：切线为零 %4d 个（%.0f%%），旋转前向偏离超过 %.0f° 的 %4d 个（%.0f%%）"
          % (kind, n, zt, 100.0 * zt / n, BAD_ANGLE, bad, 100.0 * bad / n))
    w("")
    w("怎么读：")
    w("  切线长为 0        -> 样条的切线数据没建起来，GetRotationAtDistanceAlongSpline")
    w("                      内部的 MakeFromXZ(切线, Up) 退化，前向变成垂直方向。")
    w("                      车的探测球方向、蓝图里一切依赖样条旋转的地方全受影响。")
    w("  切线正常但夹角大   -> 切线方向本身是错的，不是退化问题，要看写点的方式。")
    w("  三类都正常         -> 那探测球朝右就另有原因，回去查蓝图。")
    w("只读脚本，没有改动任何东西。")
    flush()
    unreal.log("[tan] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[tan] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
