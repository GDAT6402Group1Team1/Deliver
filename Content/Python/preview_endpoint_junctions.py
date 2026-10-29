# -*- coding: utf-8 -*-
"""预览：端点规则会多判出哪些路口。**只读，什么都不生成。**

为什么要单独一个预览脚本
------------------------
`gen_traffic_lanes.py` 的 `run()` 写在模块末尾、**import 即执行**，
所以不能 import 它来复用判定——一旦 import 就等于把全图 552 个 actor 重建一遍。
而端点规则是新加的，直接拿全图重建去验证，万一规则误判，代价是整套交通线。

所以这里把两条判据**镜像**一遍（和 gen_traffic_lanes.py 里保持一致，
改那边记得改这边），只输出"现在能判出几个、加上端点规则多出几个、多出来的在哪"。

判据（和生成器完全一致）：
  老规则：两条路中线最近水平距离 < XY_CROSS(400) 且高差 < Z_CROSS(600)
  端点规则：缺口 < ENDPOINT_CROSS(1200) 且高差合格，并且
      · 对方的近点落在**对方端点**上，而我不在自己端点上  → 我是主路（T 形）
      · 我站在**自己端点**上，而对方的近点不是端点        → 我是支路（T 形）
      · 两头都是端点 → 首尾续接，**不算路口**（故意排除）

报告会把每个新增路口标上是哪条子规则命中的、缺口多少，便于逐个核对。
用法：py preview_endpoint_junctions.py
"""

import re
import traceback

import unreal

# ---- 下面这些必须和 gen_traffic_lanes.py 一致 ----
ROAD_FOLDERS = ["PS2DEM/Splines/MainRoad", "PS2DEM/Splines/BranchRoad"]
# 和 gen_traffic_lanes.py 保持一致（判据是镜像的，改一边记得改另一边）。
# R020 在生成器里同时进了 EXCLUDE_ROADS 和 NO_JUNCTION_WITH，这里只需排掉即可。
EXCLUDE_ROADS = [u"形状 43", "River", "BranchRiver",
                 "BranchRoad_R020", "MainRoad_R004", "MainRoad_R005",
                 "BranchRoad_R031"]
DETECT_STEP = 250.0
XY_CROSS = 400.0
Z_CROSS = 600.0
ENDPOINT_CROSS = 900.0
ENDPOINT_TOL = 300.0
MERGE_DIST = 2500.0

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "preview_endpoint_junctions.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[preview] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def base_road_name(label):
    return re.sub(r"_[Pp]art\d+$", "", str(label))


def road_splines():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    out, skipped_part = [], []
    for a in eas.get_all_level_actors():
        try:
            folder = str(a.get_folder_path()).replace("\\", "/").lower()
        except Exception:
            continue
        if not any(f.lower() in folder for f in ROAD_FOLDERS):
            continue
        label = a.get_actor_label()
        # _PartNN 已由 merge_part_roads.py 合成 _Merged，两边都收会把同一条路铺两遍
        if re.search(r"_[Pp]art\d+$", label):
            skipped_part.append(label)
            continue
        if any(x in label for x in EXCLUDE_ROADS):
            continue
        sp = a.get_component_by_class(unreal.SplineComponent)
        if sp and sp.get_number_of_spline_points() >= 2:
            out.append((label, sp))
    return out, skipped_part


def endpoint_near(sp, point, tol=ENDPOINT_TOL):
    n = sp.get_number_of_spline_points()
    for i in (0, n - 1):
        e = sp.get_location_at_spline_point(i, WS)
        if ((e.x - point.x) ** 2 + (e.y - point.y) ** 2) ** 0.5 < tol:
            return True
    return False


def detect(label, tsp, others):
    """返回 (老规则命中, 端点规则命中)，元素是 (沿线距离, 对方路名, 缺口, 子规则)。"""
    total = tsp.get_spline_length()
    old_hits, new_hits = [], []
    d = 0.0
    while d <= total:
        p = tsp.get_location_at_distance_along_spline(d, WS)
        for name, osp in others:
            if base_road_name(name) == base_road_name(label):
                continue
            q = osp.find_location_closest_to_world_location(p, WS)
            dxy = ((q.x - p.x) ** 2 + (q.y - p.y) ** 2) ** 0.5
            if abs(q.z - p.z) >= Z_CROSS:
                continue
            if dxy < XY_CROSS:
                old_hits.append((d, name, dxy, u"中段"))
            elif dxy < ENDPOINT_CROSS:
                near_own = (d < ENDPOINT_TOL) or (total - d < ENDPOINT_TOL)
                other_end = endpoint_near(osp, q)
                if other_end and not near_own:
                    new_hits.append((d, name, dxy, u"对方端点(我是主路)"))
                elif near_own and not other_end:
                    new_hits.append((d, name, dxy, u"我方端点(我是支路)"))
                # 两头都是端点 = 续接，不收
        d += DETECT_STEP
    return old_hits, new_hits


def cluster(hits):
    """把同一条路上连续命中的采样点合并成一个路口中心。

    **按路各维护一组**，和 gen_traffic_lanes.py 里的分组逻辑一致。
    第一版写成"只看 out[-1]"，错了两处，而且错的方向是**把一处接头report成七八处**：
      1. 新命中缺口更大时直接 continue、没更新该组的 last，于是 d 一直涨、
         迟早超过合并窗口，一处接头裂成好几个；
      2. 按沿线距离排序后不同路会交错，只看最后一个就跟丢了。
    症状极具迷惑性——报告里全是"连续站点、同一对路、各自成条"，
    看着像规则在疯狂误判，其实是统计方式错了。
    """
    groups = []
    for d, name, dxy, kind in sorted(hits):
        hit = None
        for g in groups:
            if g["road"] == name and d - g["last"] <= DETECT_STEP * 3:
                hit = g
                break
        if hit is None:
            groups.append({"road": name, "last": d,
                           "best": (d, name, dxy, kind)})
        else:
            hit["last"] = d          # 每次命中都要更新，不管缺口大小
            if dxy < hit["best"][2]:
                hit["best"] = (d, name, dxy, kind)
    return [g["best"] for g in groups]


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    w(u"=== 端点规则预览（只读，不生成任何东西）===")
    w(u"关卡：%s" % (world.get_name() if world else u"?"))
    roads, skipped = road_splines()
    w(u"参与检测的道路样条 %d 条" % len(roads))
    if skipped:
        w(u"跳过 %d 条 _PartNN 分段（已被合并成 _Merged）：%s"
          % (len(skipped), u"、".join(skipped[:6])))
    if len(roads) < 2:
        w(u"！道路太少，没法检测。ROAD_FOLDERS 对不上？")
        return

    tot_old, tot_new = 0, 0
    detail = []
    for label, sp in roads:
        others = [(n, s) for n, s in roads if n != label]
        old_hits, new_hits = detect(label, sp, others)
        o, n = cluster(old_hits), cluster(new_hits)
        tot_old += len(o)
        tot_new += len(n)
        if n:
            detail.append((label, o, n))

    w(u"")
    w(u"=" * 76)
    w(u"合计：老规则命中 %d 处，端点规则**新增** %d 处" % (tot_old, tot_new))
    w(u"（这是「每条路各自看到的路口数」，同一个物理路口会被两条路各算一次，")
    w(u" 所以物理路口数大致是这个数的一半）")
    w(u"=" * 76)

    if not detail:
        w(u"")
        w(u"端点规则一处都没多判出来。两种可能，必须分清：")
        w(u"  · T 形接头的缺口比 ENDPOINT_CROSS(%.0f) 还大 → 跑 diag_junction_miss.py"
          % ENDPOINT_CROSS)
        w(u"    看实测缺口到底多少，再决定要不要调这个值。")
        w(u"  · 缺口本来就小于 XY_CROSS(%.0f)，老规则已经判出来了 →"
          % XY_CROSS)
        w(u"    那 T 形路口判不出来是**别的原因**，不是检测阈值，")
        w(u"    得去看那几段 Inter_ 有没有生成、路口盒罩不罩得住。")
        return

    w(u"")
    for label, o, n in sorted(detail, key=lambda t: -len(t[2])):
        w(u"── %s（老规则 %d 处，新增 %d 处）" % (label, len(o), len(n)))
        for d, name, dxy, kind in n:
            w(u"     沿线 %7.0f  与 %-34s 缺口 %6.0f cm   %s"
              % (d, name, dxy, kind))

    gaps = sorted(dxy for _lbl, _o, n in detail for _d, _nm, dxy, _k in n)
    if gaps:
        w(u"")
        w(u"=" * 76)
        w(u"新增命中的缺口分布（阈值要从这里挑，不要凭手感）")
        w(u"=" * 76)
        edges = [400, 500, 600, 700, 800, 900, 1000, 1100, 1200]
        prev = XY_CROSS
        for e in edges:
            cnt = len([g for g in gaps if prev <= g < e])
            w(u"  %4.0f ~ %4.0f cm : %3d 处  %s" % (prev, e, cnt, u"#" * min(cnt, 50)))
            prev = e
        w(u"")
        w(u"  中位数 %.0f    最大 %.0f" % (gaps[len(gaps) // 2], gaps[-1]))
        w(u"  **找数值空隙**：如果某一档是 0 处、两边都有，阈值就卡在那儿最安全。")
        w(u"  一路连续没有空隙的话，说明「真接头」和「只是路过」在距离上分不开，")
        w(u"  得换判据（比如再要求两条路在该处的朝向接近垂直），不能只靠调这个数。")

    w(u"")
    w(u"=" * 76)
    w(u"核对要点")
    w(u"=" * 76)
    w(u"  · 逐条看「与谁」：应该是真的有个 T 形接头。若出现「两条平行的主路」")
    w(u"    互相命中，说明 ENDPOINT_CROSS 放太宽了，调小再预览。")
    w(u"  · 同一对路在**同一条路上出现多处**新增，要警惕：T 形接头只该有一处，")
    w(u"    多处往往是那条路在另一条路旁边绕了一段。")
    w(u"  · 「我是支路」的那些，沿线距离应该贴近 0 或贴近该路总长。")
    w(u"  · 确认没问题再跑生成链，顺序不能换：")
    w(u"      py gen_traffic_lanes.py   →   py gen_intersections.py")
    w(u"      然后**另起一次执行** py fill_turn_splines.py   →   py fix_t_junctions.py")
    w(u"    （后两步必须各自单独一次 py 执行，挤在一起写进去的组件会全部作废）")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[preview] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[preview] 写入 %s" % OUT)
