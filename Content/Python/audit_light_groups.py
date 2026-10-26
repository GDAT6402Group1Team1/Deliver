# -*- coding: utf-8 -*-
"""全图审计：哪些路口只有一个灯组（= 交叉路没有交通线的"假路口/单路路口"）。

为什么要全图查
--------------
I08 那个路口四段全是灯组 0、没有组 1，怀疑因此永远不换灯、车卡在前面。
但「交叉路没有交通线」这件事和那个路口没有任何特殊关系——
`XY_CROSS=400` 是**靠得近就算路口**的判据（不是真交叉），
所以任何"支路蹭主路""弯道贴着走"的地方都会多出一个单路路口。
CLAUDE.md 里记过全铺之前这种路有 13 条。先量清范围，再决定是逐个删还是改判据。

判据只看数据，不认几何形状：
  · 一簇 `Inter_` 段（互相 2500cm 内）里出现几种 `LightNumber`
  · 这些段分别属于哪几条路（从标签 `Inter_<路名>_<偏移>_I<序号>` 解析）
  · 一条路 → 单路路口；两条及以上 → 真路口

只读。结果写到 Saved/audit_light_groups.txt
用法：py audit_light_groups.py
"""

import re
import traceback

import unreal

CLUSTER_DIST = 2500.0        # 和 gen_intersections.py 一致
CHILD_PREFIX = "Inter_"
INTER_PREFIX = "BP_Intersection"
LABEL_RE = re.compile(r'^Inter_(.+)_([+-]\d+)_I(\d+)$')

OUT = unreal.Paths.project_saved_dir() + "audit_light_groups.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[audit] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def prop(a, name):
    for n in (name, name.lower()):
        try:
            return a.get_editor_property(n)
        except Exception:
            continue
    return u"<读不到>"


def road_of(label):
    m = LABEL_RE.match(label)
    return m.group(1) if m else u"?"


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()
    w(u"=== 路口灯组审计（只读）===")
    w(u"关卡：%s" % (world.get_name() if world else u"?"))

    children, inters = [], []
    for a in actors:
        if a.get_actor_label().startswith(CHILD_PREFIX):
            children.append(a)
        elif a.get_class().get_name().startswith(INTER_PREFIX) \
                and "Child" not in a.get_class().get_name():
            inters.append(a)
    w(u"Inter_ 段 %d 个、路口 actor %d 个" % (len(children), len(inters)))
    if not children:
        w(u"！没有 Inter_ 段。标签变了？")
        return

    # 聚类：和 gen_intersections.py 同一套（质心 + CLUSTER_DIST）
    clusters = []
    for a in children:
        p = a.get_actor_location()
        got = None
        for c in clusters:
            if (c["center"] - p).length() < CLUSTER_DIST:
                got = c
                break
        if got is None:
            clusters.append({"center": p, "segs": [a]})
        else:
            got["segs"].append(a)
            n = len(got["segs"])
            got["center"] = unreal.Vector(
                sum(s.get_actor_location().x for s in got["segs"]) / n,
                sum(s.get_actor_location().y for s in got["segs"]) / n,
                sum(s.get_actor_location().z for s in got["segs"]) / n)
    w(u"聚类出 %d 个物理路口" % len(clusters))

    single, multi, odd = [], [], []
    for cl in clusters:
        segs = cl["segs"]
        gset, roads = {}, {}
        for s in segs:
            g = str(prop(s, "LightNumber"))
            gset.setdefault(g, []).append(s)
            roads.setdefault(road_of(s.get_actor_label()), []).append(s)
        # 最近的路口 actor
        best, bd = None, 1e18
        for i in inters:
            d = (i.get_actor_location() - cl["center"]).length()
            if d < bd:
                best, bd = i, d
        row = {"center": cl["center"], "segs": segs, "groups": gset,
               "roads": roads, "inter": best, "dist": bd}
        if len(gset) == 1:
            single.append(row)
        elif len(gset) >= 2:
            multi.append(row)
        if u"<读不到>" in gset:
            odd.append(row)

    w(u"")
    w(u"两个灯组（正常）：%d 个    **只有一个灯组**：%d 个    灯组读不到：%d 个"
      % (len(multi), len(single), len(odd)))

    w(u"")
    w(u"=" * 78)
    w(u"只有一个灯组的路口（车可能在这儿等不到绿灯）")
    w(u"=" * 78)
    w(u"%-26s %-5s %-7s %-34s %s"
      % (u"位置", u"段数", u"灯组", u"涉及的路", u"最近的路口 actor"))
    for r in sorted(single, key=lambda x: -len(x["segs"])):
        c = r["center"]
        w(u"(%7.0f,%8.0f) %-5d %-7s %-34s %s (%.0f cm)"
          % (c.x, c.y, len(r["segs"]), u",".join(sorted(r["groups"])),
             u" + ".join(sorted(r["roads"]))[:34],
             r["inter"].get_actor_label() if r["inter"] else u"无", r["dist"]))

    one_road = [r for r in single if len(r["roads"]) == 1]
    w(u"")
    w(u"其中**只涉及一条路**的有 %d 个 —— 这些是典型的单路/假路口：" % len(one_road))
    w(u"  交叉的那条路没有交通线，三条路（直行+左右转）全是直行副本，")
    w(u"  在这儿立一盏灯没有任何意义，只会无故拦车。")
    for r in one_road[:20]:
        c = r["center"]
        w(u"    (%7.0f,%8.0f)  %d 段  %s"
          % (c.x, c.y, len(r["segs"]), list(r["roads"].keys())[0]))
    if len(one_road) > 20:
        w(u"    …… 还有 %d 个" % (len(one_road) - 20))

    w(u"")
    w(u"=" * 78)
    w(u"两个灯组的路口（对照组，确认正常的长什么样）")
    w(u"=" * 78)
    for r in sorted(multi, key=lambda x: -len(x["segs"]))[:8]:
        c = r["center"]
        w(u"(%7.0f,%8.0f) %-5d %-7s %s"
          % (c.x, c.y, len(r["segs"]), u",".join(sorted(r["groups"])),
             u" + ".join(sorted(r["roads"]))[:40]))

    w(u"")
    w(u"=" * 78)
    w(u"怎么用这份报告")
    w(u"=" * 78)
    w(u"  · 单组路口只有个别几个 → 逐个删掉那几段 Inter_、让车道在那儿连续。")
    w(u"  · 单组路口很多（十几个以上）→ 别逐个删，根因在判据：")
    w(u"    XY_CROSS=400 是「靠得近就算路口」，不是「真的交叉」。")
    w(u"    但**别直接调它**——注释里算过，往下要盖住真路口最大的 360cm 缺口、")
    w(u"    往上要低于兄弟段最小的 629cm，距离分布连续、没有干净空隙，")
    w(u"    先跑 diag_junction_miss.py 重新量这两头。")
    w(u"  · 另一条路：让 BP_Intersection 在「只有一个灯组」时不拦车（恒绿）。")
    w(u"    这条不用动关卡数据，但要先确认它现在到底怎么决定放行。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[audit] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[audit] 写入 %s" % OUT)
