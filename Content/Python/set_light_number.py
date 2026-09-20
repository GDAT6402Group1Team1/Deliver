# -*- coding: utf-8 -*-
"""把**左右走向（沿 Y 轴）**的路口样条线的 LightNumber 设成 1。

红绿灯要交替放行，横竖两个方向必须分在不同的组里。
沿 X 走的保持原值（默认 0），沿 Y 走的设成 1。

走向按样条首尾连线的水平分量判断：|dy| > |dx| 就算沿 Y。
接近 45 度的会在报告里单独点名——那种路口该归哪组得人来定，
脚本按 |dy|>|dx| 归了，但你要能看见它是谁。

属性名先自动找：从蓝图的成员变量清单里挑名字像 light number 的，
所以 LightNumber / lightnumber / Light_Number 哪种写法都能对上。

DRY_RUN=True 只报告不改。
用法：py set_light_number.py
"""

import traceback

import unreal

CHILD_BP = "/Game/PS2DEM/BP_TrafficLine1_IntersectionChild"
TAG_PREFIX = "ClaudeGenLane"
LABEL_PREFIX = "Inter_"       # 只动路口段，不动普通车道段
Y_VALUE = 1                   # 沿 Y 的设成这个值
DRY_RUN = False
AMBIGUOUS_RATIO = 1.25        # 长短轴之比小于这个值就算"接近 45 度"，单独点名
CLUSTER_DIST = 2500.0         # 这个距离内的路口段算同一个物理路口（和 gen_intersections 一致）

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "light_number.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[light] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def find_prop_name(sample_actor):
    """找出 LightNumber 在 Python 侧的确切属性名。"""
    names = []
    try:
        bp = unreal.EditorAssetLibrary.load_asset(CHILD_BP)
        names = [str(n) for n in
                 unreal.BlueprintEditorLibrary.list_member_variable_names(bp)]
        w("蓝图成员变量 %d 个: %s" % (len(names), ", ".join(names)))
    except Exception as exc:
        w("列不出蓝图成员变量（%s），改用候选名硬试" % str(exc)[:60])

    cands = [n for n in names
             if "light" in n.lower() and "num" in n.lower()]
    cands += ["LightNumber", "lightnumber", "light_number", "LightNum"]

    for n in cands:
        try:
            v = sample_actor.get_editor_property(n)
            w("属性名确定为 '%s'（当前值 %s）" % (n, v))
            return n
        except Exception:
            continue
    w("!! 这些名字都读不到: %s" % ", ".join(cands))
    return None


def verify_junctions(inters, prop):
    """按物理路口聚类，检查每个路口里是不是真的有两个不同的灯组。

    按轴分组是全局规则，但红绿灯要的是"同一路口内两条路分属不同组"。
    斜路口可能两条都归进同一组，那个路口就永远不会交替放行——
    这种错光看 Y/X 的条数看不出来，必须按路口验。
    """
    clusters = []
    for a, _sp in inters:
        pos = a.get_actor_location()
        got = None
        for c in clusters:
            if (c["center"] - pos).length() < CLUSTER_DIST:
                got = c
                break
        if got is None:
            clusters.append({"center": pos, "items": [(a, pos)]})
        else:
            got["items"].append((a, pos))
            n = len(got["items"])
            got["center"] = unreal.Vector(
                sum(p.x for _x, p in got["items"]) / n,
                sum(p.y for _x, p in got["items"]) / n,
                sum(p.z for _x, p in got["items"]) / n)

    w("按物理路口验算：共 %d 个路口" % len(clusters))
    bad = []
    for i, c in enumerate(clusters):
        vals = set()
        for a, _p in c["items"]:
            try:
                vals.add(a.get_editor_property(prop))
            except Exception:
                pass
        if len(vals) < 2:
            bad.append((i, c, vals))
    if not bad:
        w("  每个路口都含有至少两个不同的灯组，交替放行成立。")
        return
    w("  !! %d 个路口里只有一个灯组，这些路口不会交替放行：" % len(bad))
    for i, c, vals in bad[:12]:
        names = ", ".join(a.get_actor_label()[:26] for a, _p in c["items"][:4])
        w("     路口%02d @ (%.0f, %.0f)  灯组 %s  共 %d 段  %s"
          % (i, c["center"].x, c["center"].y, vals, len(c["items"]), names))
    w("  这些多半是斜路口（两条路走向接近，按轴分到了同一组），")
    w("  或者那个位置只有一条路。需要手动指定灯组。")


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    inters = []
    for a in eas.get_all_level_actors():
        if not any(str(t).startswith(TAG_PREFIX) for t in a.tags):
            continue
        if not a.get_actor_label().startswith(LABEL_PREFIX):
            continue
        sp = a.get_component_by_class(unreal.SplineComponent)
        if sp is None or sp.get_number_of_spline_points() < 2:
            continue
        inters.append((a, sp))

    w("路口样条线 %d 条" % len(inters))
    if not inters:
        w("!! 一条都没有，先跑 gen_traffic_lanes.py")
        flush()
        return

    prop = find_prop_name(inters[0][0])
    if prop is None:
        flush()
        return
    w("")

    along_y, along_x, ambiguous = [], [], []
    for a, sp in inters:
        L = sp.get_spline_length()
        p0 = sp.get_location_at_distance_along_spline(0.0, WS)
        p1 = sp.get_location_at_distance_along_spline(L, WS)
        dx, dy = abs(p1.x - p0.x), abs(p1.y - p0.y)
        lo, hi = min(dx, dy), max(dx, dy)
        ratio = (hi / lo) if lo > 1e-3 else 999.0
        if dy > dx:
            along_y.append((a, dx, dy, ratio))
        else:
            along_x.append((a, dx, dy, ratio))
        if ratio < AMBIGUOUS_RATIO:
            ambiguous.append((a, dx, dy, ratio))

    w("沿 Y（左右走向）%d 条  ->  %s = %d" % (len(along_y), prop, Y_VALUE))
    w("沿 X（上下走向）%d 条  ->  保持原值" % len(along_x))
    w("")
    w("沿 Y 的：")
    for a, dx, dy, r in sorted(along_y, key=lambda t: t[0].get_actor_label()):
        try:
            cur = a.get_editor_property(prop)
        except Exception:
            cur = "?"
        w("   %-34s dx %6.0f  dy %6.0f   现值 %s" %
          (a.get_actor_label()[:34], dx, dy, cur))

    if ambiguous:
        w("")
        w("接近 45 度、归组存疑的 %d 条（已按 |dy|>|dx| 归类）：" % len(ambiguous))
        for a, dx, dy, r in ambiguous:
            w("   %-34s dx %6.0f  dy %6.0f  长短轴比 %.2f"
              % (a.get_actor_label()[:34], dx, dy, r))

    if DRY_RUN:
        w("")
        w("DRY_RUN=True，什么都没改。确认分类无误后改成 False 再跑。")
        flush()
        return

    done = fail = 0
    for a, _dx, _dy, _r in along_y:
        try:
            a.modify(True)
            a.set_editor_property(prop, Y_VALUE)
            done += 1
        except Exception as exc:
            fail += 1
            if fail <= 3:
                w("!! %s: %s" % (a.get_actor_label(), str(exc)[:70]))
    w("")
    w("已设置 %d 条，失败 %d 条。" % (done, fail))
    w("")
    verify_junctions(inters, prop)
    w("注意：重跑 gen_traffic_lanes.py 会重建这些 actor。")
    w("生成器里已内置同样的逻辑（LIGHT_NUMBER_BY_AXIS），所以重建后不用再跑本脚本。")
    w("关卡尚未保存。")
    flush()


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[light] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
