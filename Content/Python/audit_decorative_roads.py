# -*- coding: utf-8 -*-
"""找出「有道路样条、但没有交通线」的路——那些是人工删掉、只当装饰的路。

为什么要查
----------
用户手动删掉了一些装饰路的车道和路口（不想车跑上去）。但**删除本身是不可复现的**：
`gen_traffic_lanes.py` 只认 `EXCLUDE_ROADS` / `NO_JUNCTION_WITH` 两个名单，
名单里没有的路，下次全图重建会原样铺回来——手工删除的成果全丢。

而且**两个名单都要加**，这一条反复踩过：
  · `EXCLUDE_ROADS`   —— 不给它铺车道
  · `NO_JUNCTION_WITH` —— **不参与相交检测**
只加前者的话，别的路照样会在和它的交叉点被切断、长出 `Inter_` 段，
`gen_intersections.py` 再给那儿配个盒子，于是"路口"又回来了（等于白删）。
河（River/BranchRiver）当初就是因为只排除了铺车道、仍参与检测，
把过桥的地方切成路口、车开到桥上反而断线。

判据：道路样条在 ROAD_FOLDERS 里存在，但关卡里找不到任何
`ClaudeGenLane:<路名>` 标签的 actor。

只读。结果写到 Saved/audit_decorative_roads.txt，并直接给出可粘贴的名单。
用法：py audit_decorative_roads.py
"""

import re
import traceback

import unreal

# 和 gen_traffic_lanes.py 保持一致
ROAD_FOLDERS = ["PS2DEM/Splines/MainRoad", "PS2DEM/Splines/BranchRoad"]
TAG_PREFIX = "ClaudeGenLane"
ALREADY = ["River", "BranchRiver", u"形状 43",
           "BranchRoad_R020", "MainRoad_R004", "MainRoad_R005"]

OUT = unreal.Paths.project_saved_dir() + "audit_decorative_roads.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[deco] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    w(u"=== 装饰路审计（有道路样条但没有交通线）===")
    w(u"关卡：%s" % (world.get_name() if world else u"?"))

    roads, tagged = {}, {}
    for a in eas.get_all_level_actors():
        label = a.get_actor_label()
        try:
            folder = str(a.get_folder_path()).replace("\\", "/").lower()
        except Exception:
            folder = ""
        if any(f.lower() in folder for f in ROAD_FOLDERS):
            sp = a.get_component_by_class(unreal.SplineComponent)
            if sp and sp.get_number_of_spline_points() >= 2:
                roads[label] = sp.get_spline_length()
        for t in a.tags:
            t = str(t)
            if t.startswith(TAG_PREFIX + ":"):
                nm = t[len(TAG_PREFIX) + 1:]
                tagged[nm] = tagged.get(nm, 0) + 1

    w(u"道路样条 %d 条；有交通线的路 %d 条" % (len(roads), len(tagged)))

    # _PartNN 已被 merge_part_roads.py 合成 _Merged，生成器跳过它们，不算装饰路
    def is_part(nm):
        return re.search(r"_[Pp]art\d+$", nm) is not None

    bare, parts = [], []
    for nm, length in sorted(roads.items()):
        if nm in tagged:
            continue
        (parts if is_part(nm) else bare).append((nm, length))

    if parts:
        w(u"")
        w(u"跳过 %d 条 _PartNN 分段（已合并成 _Merged，生成器本来就不铺它们）" % len(parts))

    w(u"")
    w(u"=" * 74)
    w(u"没有任何交通线的路：%d 条" % len(bare))
    w(u"=" * 74)
    w(u"%-34s %10s   %s" % (u"路名", u"长度cm", u"两个名单里有没有"))
    need = []
    for nm, length in bare:
        listed = any(x in nm for x in ALREADY)
        w(u"%-34s %10.0f   %s" % (nm, length, u"已排除" if listed else u"**没有 ← 要加**"))
        if not listed:
            need.append(nm)

    w(u"")
    if not need:
        w(u"全部已在排除名单里，手工删除是可复现的——重建不会把它们铺回来。")
        return
    w(u"=" * 74)
    w(u"下面这 %d 条要加进 gen_traffic_lanes.py 的**两个**名单" % len(need))
    w(u"=" * 74)
    w(u"EXCLUDE_ROADS   += %s" % repr(need))
    w(u"NO_JUNCTION_WITH += %s" % repr(need))
    w(u"")
    w(u"只加 EXCLUDE_ROADS 不够：被排除的样条**仍然参与相交检测**，")
    w(u"别的路照样会在和它的交叉点被切断、长出 Inter_ 段，")
    w(u"gen_intersections 再配个盒子，路口就又回来了。")
    w(u"加完记得同步 preview_endpoint_junctions.py 的 EXCLUDE_ROADS（判据是镜像的）。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[deco] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[deco] 写入 %s" % OUT)
