# -*- coding: utf-8 -*-
"""导出关卡里交通线的**人工改动清单**，让下次全图重建能复现。只读。

为什么需要
----------
用户手工做了大量调整（删装饰路的线、删个别段、删某些方向的转弯），
但其中只有一部分能被现有配置复现：
  · 整条路没线        → EXCLUDE_ROADS / NO_JUNCTION_WITH（已有机制）
  · LightNumber 分组   → MERGE_ROADS_BY_AXIS（已编码）
  · **删个别段**       → 没有任何机制，重建就长回来
  · **某处不要转弯**   → 没有机制，fill_turn_splines 下次把曲线画回来
后两类是这份清单要解决的。

怎么识别"被删掉的段"——**按序号空洞，不重算生成器的几何**：
段名是 `Lane_<路>_<偏移>_S<nn>` / `Inter_<路>_<偏移>_I<nn>`，序号连续。
若某条路某个偏移下有 S00/S01/S03 而没有 S02，那 S02 就是被删掉的。
这个判据不需要复刻生成器那套路口检测 + 车道偏移 + 长度门槛的数学，
所以不会因为两边实现漂移而失效（预览脚本镜像判据那次就踩过）。

怎么识别"这里不要转弯"：`SplineLeft`/`SplineRight` 的点和主样条**逐点相同**
= 直行副本。不区分是人删的还是 fix_t_junctions 写的——
两者都该在重建后保持"不转弯"，记下来是安全的。

输出 Design/TrafficManualEdits.json（文本、能进 git、冲突能合并），
外加人读的摘要。**本脚本不改关卡。**
用法：py export_manual_edits.py
"""

import json
import os
import re
import traceback

import unreal

ROAD_FOLDERS = ["PS2DEM/Splines/MainRoad", "PS2DEM/Splines/BranchRoad"]
TAG_PREFIX = "ClaudeGenLane"
SAME_PT_EPS = 5.0        # 逐点比较的容差（cm）
LANE_RE = re.compile(r'^Lane_(.+)_([+-]\d+)_S(\d+)$')
INTER_RE = re.compile(r'^Inter_(.+)_([+-]\d+)_I(\d+)$')

JSON_OUT = unreal.Paths.project_dir() + "Design/TrafficManualEdits.json"
OUT = unreal.Paths.project_saved_dir() + "export_manual_edits.txt"
lines = []
WS = unreal.SplineCoordinateSpace.WORLD


def w(s=""):
    lines.append(str(s))
    unreal.log("[export] %s" % s)


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
    return None


def splines(a):
    return {c.get_name(): c for c in a.get_components_by_class(unreal.SplineComponent)}


def main_of(d):
    for k in ("Spline", "SplineComponent"):
        if k in d:
            return k, d[k]
    for n, c in sorted(d.items()):
        if "Left" not in n and "Right" not in n:
            return n, c
    return (None, None)


def pts_of(sp):
    return [sp.get_location_at_spline_point(i, WS)
            for i in range(sp.get_number_of_spline_points())]


def same_points(a, b):
    pa, pb = pts_of(a), pts_of(b)
    if len(pa) != len(pb) or not pa:
        return False
    for p, q in zip(pa, pb):
        if ((p.x - q.x) ** 2 + (p.y - q.y) ** 2 + (p.z - q.z) ** 2) ** 0.5 > SAME_PT_EPS:
            return False
    return True


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    w(u"=== 交通线人工改动清单（只读）===")
    w(u"关卡：%s" % (world.get_name() if world else u"?"))

    roads, tagged = {}, {}
    lane_idx, inter_idx = {}, {}
    no_turn, light, inter_rec = [], {}, []
    for a in eas.get_all_level_actors():
        lbl = a.get_actor_label()
        try:
            folder = str(a.get_folder_path()).replace("\\", "/").lower()
        except Exception:
            folder = ""
        if any(f.lower() in folder for f in ROAD_FOLDERS):
            sp = a.get_component_by_class(unreal.SplineComponent)
            if sp and sp.get_number_of_spline_points() >= 2:
                roads[lbl] = round(sp.get_spline_length(), 1)
        for t in a.tags:
            t = str(t)
            if t.startswith(TAG_PREFIX + ":"):
                tagged.setdefault(t[len(TAG_PREFIX) + 1:], 0)
                tagged[t[len(TAG_PREFIX) + 1:]] += 1

        m = LANE_RE.match(lbl)
        if m:
            lane_idx.setdefault((m.group(1), m.group(2)), set()).add(int(m.group(3)))
            continue
        m = INTER_RE.match(lbl)
        if not m:
            continue
        inter_idx.setdefault((m.group(1), m.group(2)), set()).add(int(m.group(3)))
        light[lbl] = prop(a, "LightNumber")
        sd = splines(a)
        mk, ms = main_of(sd)
        if ms is None:
            continue
        # 记**重合关系**而不是"是不是直行副本"：用户把某条转弯改成和别的重合，
        # 只记"直行副本"描述不出来。每条样条标成 unique 或 "=<另一条的名字>"。
        names = [mk] + sorted(n for n in sd if n != mk)
        ident = {}
        for i, nm in enumerate(names):
            same_as = None
            for prev in names[:i]:
                if same_points(sd[nm], sd[prev]):
                    same_as = prev
                    break
            ident[nm] = (u"=" + same_as) if same_as else u"unique"
        p0 = pts_of(ms)[0]
        inter_rec.append({
            "road": m.group(1), "off": m.group(2), "i": int(m.group(3)),
            "x": round(p0.x, 1), "y": round(p0.y, 1),
            "splines": ident,
        })
        if any(v != u"unique" for v in ident.values()):
            no_turn.append([lbl, u"、".join(
                u"%s%s" % (k, v) for k, v in sorted(ident.items()) if v != u"unique")])

    def holes(idx):
        out = []
        for (road, off), got in sorted(idx.items()):
            if not got:
                continue
            for i in range(min(got), max(got) + 1):
                if i not in got:
                    out.append([road, off, i])
        return out

    bare = sorted(nm for nm in roads if nm not in tagged
                  and not re.search(r"_[Pp]art\d+$", nm))
    lane_holes, inter_holes = holes(lane_idx), holes(inter_idx)

    w(u"")
    w(u"道路样条 %d 条，其中**一条交通线都没有**的 %d 条" % (len(roads), len(bare)))
    for nm in bare:
        w(u"    %s" % nm)
    w(u"")
    w(u"序号空洞 = 被单独删掉的段：Lane_ %d 处、Inter_ %d 处"
      % (len(lane_holes), len(inter_holes)))
    for road, off, i in (lane_holes + inter_holes)[:20]:
        w(u"    %s %s 第 %d 段" % (road, off, i))
    if len(lane_holes) + len(inter_holes) > 20:
        w(u"    …… 还有 %d 处" % (len(lane_holes) + len(inter_holes) - 20))
    w(u"")
    w(u"有重合关系的路口段：%d 个（共记录 %d 个路口段的三条样条）"
      % (len(no_turn), len(inter_rec)))
    for lbl, desc in no_turn[:10]:
        w(u"    %-40s %s" % (lbl[:40], desc))
    if len(no_turn) > 10:
        w(u"    …… 还有 %d 个" % (len(no_turn) - 10))

    data = {
        "_说明": [
            "交通线的人工改动清单，由 export_manual_edits.py 导出，供全图重建后复现。",
            "exclude_roads: 一条交通线都没有的路 —— 加进 gen_traffic_lanes.py 的",
            "  EXCLUDE_ROADS **和** NO_JUNCTION_WITH（只加前者的话别的路照样会在",
            "  和它的交叉点被切断、长出 Inter_ 段，路口又回来了）。",
            "deleted_lane_segments / deleted_inter_segments: 按序号空洞识别出的、",
            "  被单独删掉的段，形如 [路名, 偏移, 序号]。重建后要照它删一遍。",
            "intersection_splines: 每个 Inter_ 段的三条样条**怎么重合**。",
            "  splines 里每条标 unique（自己独有的几何）或 =<另一条的名字>（和那条逐点相同）。",
            "  键是 road + off + 坐标(x,y)，**不是 I 序号**——序号在重建后会变位，",
            "  用它匹配会对错位置（SKIP_INTERSECTIONS 按名字记就踩过这个坑）。",
            "  重建完跑 apply_manual_edits.py 按这份记录重新写一遍重合关系。",
            "light_numbers: 每个 Inter_ 段当时的 LightNumber，做兜底对照用；",
            "  正常情况下 fix_light_number_by_axis.py 能自己算出同样的结果。",
        ],
        "exclude_roads": bare,
        "deleted_lane_segments": lane_holes,
        "deleted_inter_segments": inter_holes,
        "intersection_splines": sorted(
            inter_rec, key=lambda r: (r["road"], r["off"], r["i"])),
        "light_numbers": {k: (None if v is None else int(v))
                          for k, v in sorted(light.items())},
    }
    try:
        d = os.path.dirname(JSON_OUT)
        if not os.path.isdir(d):
            os.makedirs(d)
        with open(JSON_OUT, "w", encoding="utf-8") as fh:
            json.dump(data, fh, ensure_ascii=False, indent=1, sort_keys=False)
        w(u"")
        w(u"已写 %s" % JSON_OUT)
        w(u"（文本格式、能进 git、冲突能合并——二进制 uasset 冲突只能整份取舍）")
    except Exception as exc:
        w(u"！写 JSON 失败：%s" % str(exc)[:90])

    w(u"")
    w(u"=" * 74)
    w(u"下一步（要我做就说一声）")
    w(u"=" * 74)
    w(u"  1. 把 exclude_roads 补进 gen_traffic_lanes.py 的两个名单（现在只有 3 条）")
    w(u"  2. 让 gen_traffic_lanes.py 读 deleted_* 两个列表，生成后照它删掉")
    w(u"  3. 让 fill_turn_splines.py 读 no_turn_splines，跳过/写成直行副本")
    w(u"  第 2、3 条是新机制。在那之前，**全图重建会丢掉这些手改**，")
    w(u"  只能靠这份 JSON 人工对照恢复。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[export] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[export] 写入 %s" % OUT)
