# -*- coding: utf-8 -*-
"""隔离实验：往 SplineLeft 写点，立刻读回，看数据留不留得住。

现象：fill_turn_splines 报告"写入左转 4 条右转 4 条"，但关卡里所有转弯样条
都是零长。报告只能证明 write_spline 被调用过，证明不了结果留住了。

要分清两种情况，处理方式完全不同：
  写不进去        -> add_spline_point / update_spline 在这两个后加的组件上不工作
  写进去又被冲掉  -> 构造脚本重跑覆盖了，是 spline_has_been_edited 没打上

所以这里写完**立刻**读回，并且单独报告 mark_edited 的返回值——
fill_turn_splines 里这个返回值被静默吞掉了，正是看不出问题的原因。

只碰一个 actor 的 SplineLeft，写一条 5 点的测试折线。
用法：py probe_turn_write.py
"""

import traceback

import unreal

TARGET = "Inter_形状13_-0540_I04"
COMP = "SplineLeft"

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "probe_turn_write.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[probe] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def describe(sp, when):
    n = sp.get_number_of_spline_points()
    L = sp.get_spline_length()
    w("  [%s] 点数 %d  长度 %.1f" % (when, n, L))
    for i in range(min(n, 6)):
        p = sp.get_location_at_spline_point(i, WS)
        w("        点%d (%.0f, %.0f, %.0f)" % (i, p.x, p.y, p.z))
    return n, L


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

    sp = None
    w("%s 的样条组件：" % TARGET)
    for c in a.get_components_by_class(unreal.SplineComponent):
        w("   %-14s 点数 %d  长度 %.1f"
          % (c.get_name(), c.get_number_of_spline_points(),
             c.get_spline_length()))
        if c.get_name() == COMP:
            sp = c
    if sp is None:
        w("!! 没有叫 %s 的组件" % COMP)
        flush()
        return
    w("")

    w("写之前：")
    describe(sp, "before")
    w("")

    # 以 actor 为原点，铺一条 5 点、每段 200cm 的折线，好认
    o = a.get_actor_location()
    pts = [unreal.Vector(o.x + i * 200.0, o.y + (i % 2) * 100.0, o.z)
           for i in range(5)]

    w("写入 5 个点（每段 200cm 的折线，总长应约 800+）")
    sp.clear_spline_points(False)
    for p in pts:
        sp.add_spline_point(p, WS, False)
    for i in range(sp.get_number_of_spline_points()):
        sp.set_spline_point_type(i, unreal.SplinePointType.CURVE_CLAMPED, False)
    sp.update_spline()

    ok = []
    for name in ("spline_has_been_edited", "b_spline_has_been_edited"):
        try:
            sp.set_editor_property(name, True)
            ok.append(name)
        except Exception as exc:
            w("  设 %s 失败: %s" % (name, str(exc)[:70]))
    w("  mark_edited 成功的属性: %s" % (ok or "一个都没成功  <<<"))
    w("")

    w("写之后立刻读回：")
    n, L = describe(sp, "after")
    w("")

    w("=" * 64)
    if n >= 5 and L > 100:
        w("结论：**写得进去**。数据当场就在。")
        w("那么关卡里看到的零长，是写完之后又被冲掉的——")
        w("构造脚本重跑覆盖了这两个后加的组件。")
        if not ok:
            w("而且 spline_has_been_edited 一个都没设上，正好对得上：")
            w("没有这个标记，构造脚本每次重跑都会把点冲回蓝图默认值。")
        else:
            w("但 spline_has_been_edited 设上了（%s），" % ",".join(ok))
            w("说明这个标记在后加的组件上不起作用，得换别的办法。")
    else:
        w("结论：**写不进去**。add_spline_point/update_spline 在这个组件上不工作。")
        w("和 Spline 那条的差别值得再查（Spline 一直写得进去）。")
    w("")
    w("请在编辑器里点一下这个 actor、或者存一次关卡，再跑一遍本脚本，")
    w("对比'写之前'那几行——如果又变回零长/100cm，就坐实是被冲掉的。")
    w("关卡尚未保存。")
    flush()


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[probe] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常：")
    lines.append(err)
    flush()
