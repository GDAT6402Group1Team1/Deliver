# -*- coding: utf-8 -*-
"""查 spline_has_been_edited 在 SplineLeft/SplineRight 上到底留不留得住。

现象：填进去的转弯曲线，过一阵变回**蓝图默认的 100cm 两点直线**。
不是零长（零长是脚本写的），是 CDO 默认值——说明构造脚本重跑后
按默认值重建了这两个组件，spline_has_been_edited 没能保护住它们。
而主 Spline 一直安然无恙，差别就在这个标记上。

set_editor_property 当时是返回成功的，但"设置成功"和"值留住了"是两回事，
所以这里设完立刻读回，并且拿主 Spline 做对照。

只读 + 设一个布尔，不改任何样条点。
用法：py probe_spline_flag.py
"""

import traceback

import unreal

TARGET = "Inter_形状13_-0540_I04"
FLAG = "spline_has_been_edited"

OUT = unreal.Paths.project_saved_dir() + "probe_spline_flag.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[flag] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def read_flag(c):
    try:
        return c.get_editor_property(FLAG)
    except Exception as exc:
        return "读不到(%s)" % str(exc)[:40]


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    a = None
    for x in eas.get_all_level_actors():
        if x.get_actor_label() == TARGET:
            a = x
            break
    if a is None:
        w("!! 找不到 %s" % TARGET)
        flush()
        return
    w("试验对象 %s" % TARGET)
    w("")

    comps = list(a.get_components_by_class(unreal.SplineComponent))
    w("%-14s %5s %9s  %-22s %-22s" % ("组件", "点数", "长度", "当前 " + FLAG, "设 True 后读回"))
    w("-" * 84)
    for c in comps:
        n = c.get_number_of_spline_points()
        L = c.get_spline_length()
        before = read_flag(c)
        try:
            c.set_editor_property(FLAG, True)
            err = ""
        except Exception as exc:
            err = " 设置抛错:%s" % str(exc)[:40]
        after = read_flag(c)
        mark = ""
        if after is not True:
            mark = "   <<< 没留住"
        w("%-14s %5d %9.1f  %-22s %-22s%s%s"
          % (c.get_name(), n, L, str(before), str(after), mark, err))

    w("")
    w("=" * 84)
    w("判读")
    w("=" * 84)
    w("  主 Spline 读回 True、SplineLeft/Right 读回 False")
    w("     -> 标记在后加的组件上设不进去，构造脚本重跑必然把它们重建成默认值。")
    w("        解决方向：在蓝图构造脚本里给这两条加保护（比如点数 > 2 就不重建），")
    w("        或者干脆不让构造脚本碰它们。")
    w("  三条都读回 True")
    w("     -> 标记是留住了，但没能阻止重建。那说明蓝图的构造脚本**主动**")
    w("        往这两条里写了点（Override Construction Script 只挡自动重建，")
    w("        挡不住脚本里显式的 SetSplinePoints/ClearSplinePoints）。")
    w("        那就得去看构造脚本里对 SplineLeft/SplineRight 做了什么。")
    w("  当前点数是 2、长度 100")
    w("     -> 确认它现在就是被重建过的默认状态。")
    w("")
    w("只设了一个布尔，没有改动任何样条点。")
    flush()


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[flag] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常：")
    lines.append(err)
    flush()
