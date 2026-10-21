# -*- coding: utf-8 -*-
"""查清「道路样条」现在是什么形态。只读。

为什么要查：队友做了 348e00b「将 landscape spline 逆向同步到蓝图 spline actor」
之后重跑 rebuild_traffic.py，报告是

    关卡道路样条共 2 条
    排除不铺车道的路 2 条：River、BranchRiver
    合计 路段 0 + 路口段 0 = 0 个 actor

也就是一条车道都没生成（之前是 38 条路 → 552 个 actor）。
原因在 gen_traffic_lanes.py:965：它按**类名精确匹配**
`BP_PS2DEMSplineActor_v3_C`，现在全关卡只有两条河是这个类。
新的道路一定换了形态——可能是新蓝图类、改了名（_v4？）、
或者路只存在 landscape spline 里、蓝图 actor 只是个代理。

本脚本把关卡里**所有带样条组件的 actor**按类分组列出来，附点数、长度、标签，
并单独统计 landscape spline。看完就知道该把那个类名改成什么、
或者要不要改成按组件类型而不是按类名来找。

用法：py find_road_splines.py
"""

import traceback

import unreal

OUT = unreal.Paths.project_saved_dir() + "find_road_splines.txt"
WS = unreal.SplineCoordinateSpace.WORLD
lines = []


def w(s=""):
    lines.append(str(s))


def main():
    eas = unreal.EditorActorSubsystem()
    actors = eas.get_all_level_actors()
    w("关卡 actor 总数 %d" % len(actors))
    w()

    # ---------------------------------------------------------------- 普通样条
    by_class = {}
    landscape_splines = []
    for a in actors:
        if not a:
            continue
        try:
            cls = a.get_class().get_name()
        except Exception:
            continue

        # 普通 USplineComponent（脚本生成的车道、河、以及原来的道路都是这种）
        sp = None
        try:
            sp = a.get_component_by_class(unreal.SplineComponent)
        except Exception:
            pass

        # landscape spline 是另一套组件，不是 USplineComponent
        try:
            ls = a.get_component_by_class(unreal.LandscapeSplinesComponent)
            if ls:
                landscape_splines.append((cls, a.get_actor_label()))
        except Exception:
            pass

        if sp:
            try:
                n = sp.get_number_of_spline_points()
                length = sp.get_spline_length()
            except Exception:
                n, length = -1, -1.0
            by_class.setdefault(cls, []).append((a.get_actor_label(), n, length))

    w("=" * 70)
    w("带 SplineComponent 的 actor，按类分组")
    w("=" * 70)
    for cls in sorted(by_class, key=lambda c: -len(by_class[c])):
        items = by_class[cls]
        w()
        w("%s   —— %d 个" % (cls, len(items)))
        # 车道那种有几百个，只列前 12 个样本
        for label, n, length in sorted(items)[:12]:
            w("    %-40s 点数 %-4s 长度 %.0f" % (label, n, length))
        if len(items) > 12:
            w("    ...（还有 %d 个，省略）" % (len(items) - 12))

    # ---------------------------------------------------------------- landscape
    w()
    w("=" * 70)
    w("带 LandscapeSplinesComponent 的 actor")
    w("=" * 70)
    if landscape_splines:
        for cls, label in sorted(set(landscape_splines)):
            w("    %-40s [%s]" % (label, cls))
        w()
        w("如果道路只在这里、蓝图 actor 那边是空的，那生成器就得改成读 landscape spline，")
        w("那是另一套 API（ULandscapeSplinesComponent 的控制点和段，不是样条点），")
        w("工作量明显更大——优先确认有没有一个带 USplineComponent 的新蓝图类可用。")
    else:
        w("    没有")

    # ---------------------------------------------------------------- 按名字找路
    # 新的交通线要依据 mainroad / branchroad 这两条样条（不再是以前的 形状xx）。
    # 按标签找而不是按类名找：报告里 BP_PS2DEMSplineActor_v3_C 只有 River/BranchRiver
    # 两条，说明这两条路不是那个类，但我们还不知道是什么类——所以反过来按名字捞，
    # 捞到之后把它的类名和组件打出来，才能决定生成器怎么改。
    w()
    w("=" * 70)
    w("按名字找 mainroad / branchroad（不限类）")
    w("=" * 70)
    keys = ("mainroad", "branchroad", "main_road", "branch_road", "road")
    hits = []
    for a in actors:
        if not a:
            continue
        try:
            label = a.get_actor_label()
        except Exception:
            continue
        low = label.lower()
        if not any(k in low for k in keys):
            continue
        try:
            cls = a.get_class().get_name()
        except Exception:
            cls = "<类名取不到>"
        hits.append((label, cls, a))

    if not hits:
        w("一个都没找到。那这两条样条的 actor 标签不含 road，")
        w("去上面按类分组那一段里找：个数少、点数几十、长度上万的那个类就是。")
    for label, cls, a in sorted(hits)[:40]:
        w()
        w("%-44s [%s]" % (label, cls))
        # 它身上到底挂了什么组件，决定生成器能不能直接读
        try:
            comps = a.get_components_by_class(unreal.SceneComponent)
        except Exception:
            comps = []
        seen = {}
        for c in comps:
            try:
                cn = c.get_class().get_name()
            except Exception:
                continue
            seen[cn] = seen.get(cn, 0) + 1
        w("    组件：" + ("、".join("%s×%d" % (k, v) for k, v in sorted(seen.items()))
                        or "<读不到>"))
        try:
            sp = a.get_component_by_class(unreal.SplineComponent)
            if sp:
                w("    SplineComponent：点数 %d，长度 %.0f"
                  % (sp.get_number_of_spline_points(), sp.get_spline_length()))
            else:
                w("    没有 USplineComponent —— 生成器现在这套读不了它")
        except Exception as exc:
            w("    读样条失败：%s" % exc)
    if len(hits) > 40:
        w()
        w("...（共 %d 个，只列前 40）" % len(hits))

    # ---------------------------------------------------------------- 结论提示
    w()
    w("=" * 70)
    w("怎么看这份报告")
    w("=" * 70)
    w("1. 找一个类：个数几十、点数十几到几十、长度上万 —— 那就是新的道路样条类，")
    w("   把 gen_traffic_lanes.py:965 的 BP_PS2DEMSplineActor_v3_C 换成它。")
    w("2. 脚本自己生成的车道也带 SplineComponent，别认错：它们标签形如")
    w("   Lane_形状13_+0180_S01 / Inter_...，而且数量上百。")
    w("3. 河（River / BranchRiver）仍在排除名单里，看它们现在是什么类——")
    w("   如果新道路和河同类，排除名单继续有效；如果河留在老类、路换了新类，")
    w("   那排除名单里那两条可以保留但已经不起作用，要重新确认新类里有没有水体。")
    w("4. 真实路宽可以用 diag_road_width.py 复核（它按两侧路面实测半宽，取中位数）。")


try:
    main()
except Exception:
    w("抛异常：")
    w(traceback.format_exc())

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

print("\n".join(lines))
print()
print("报告：" + OUT)
