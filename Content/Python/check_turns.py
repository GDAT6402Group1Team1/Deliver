# -*- coding: utf-8 -*-
"""全图核对所有路口段的转弯样条处于什么状态。

重启前后各跑一次，比数字就知道有没有丢。
whats_here.py 只看一个路口周围，这个扫全部 152 个路口段 × 2 条。

每条分成四类：
    转弯曲线    多点，终点不是自己主 Spline 的终点 —— 真的转弯
    直行副本    点数和终点都和主 Spline 一致 —— 这条车道没有这个方向的转弯
    默认残桩    2 点、长约 100 —— 蓝图出厂值，说明**被构造脚本冲掉了**
    零长        长度 < 1 —— 早期版本的压制值，现在不该再出现
    没有组件    蓝图里缺 SplineLeft/SplineRight

只读。
用法：py check_turns.py
"""

import traceback

import unreal

LANE_TAG_PREFIX = "ClaudeGenLane"
WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "check_turns.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[turns] %s" % s)


def comp_by_name(a, name):
    for c in a.get_components_by_class(unreal.SplineComponent):
        if c.get_name() == name:
            return c
    return None


def classify(sp, main):
    if sp is None:
        return "没有组件"
    n = sp.get_number_of_spline_points()
    L = sp.get_spline_length()
    if L < 1.0:
        return "零长"
    if n == 2 and 80.0 < L < 120.0:
        return "默认残桩"
    try:
        e = sp.get_location_at_spline_point(n - 1, WS)
        me = main.get_location_at_spline_point(
            main.get_number_of_spline_points() - 1, WS)
        if (e - me).length() < 1.0:
            return "直行副本"
    except Exception:
        pass
    return "转弯曲线"


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    kinds = {}
    bad = []
    n_actor = 0
    for a in eas.get_all_level_actors():
        if not any(str(t).startswith(LANE_TAG_PREFIX) for t in a.tags):
            continue
        if not a.get_actor_label().startswith("Inter_"):
            continue
        main = comp_by_name(a, "Spline")
        if main is None or main.get_number_of_spline_points() < 2:
            continue
        n_actor += 1
        for cn in ("SplineLeft", "SplineRight"):
            k = classify(comp_by_name(a, cn), main)
            kinds[k] = kinds.get(k, 0) + 1
            if k in ("默认残桩", "零长", "没有组件"):
                bad.append((a.get_actor_label(), cn, k))

    w("路口段 %d 个，转弯样条 %d 条" % (n_actor, n_actor * 2))
    w("")
    for k in ("转弯曲线", "直行副本", "默认残桩", "零长", "没有组件"):
        if k in kinds:
            w("  %-10s %d" % (k, kinds[k]))
    w("")
    if bad:
        w("!! 有 %d 条处于异常状态（前 15 条）：" % len(bad))
        for lbl, cn, k in bad[:15]:
            w("   %-32s %-12s %s" % (lbl[:32], cn, k))
        w("")
        w("  「默认残桩」= 被构造脚本冲回蓝图出厂值，写入没留住。")
        w("  「没有组件」= 蓝图里缺这个组件，实例侧补不回来。")
    else:
        w("全部正常：每条要么是转弯曲线，要么是直行副本，没有残桩。")
    w("")
    w("重启前后各跑一次，对比「转弯曲线」和「直行副本」两个数字；")
    w("只要出现「默认残桩」，就是没存住。")
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[turns] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
