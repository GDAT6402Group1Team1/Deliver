# -*- coding: utf-8 -*-
u"""按车**实际**的探测几何算「接得上接不上」，而不是按体检那个 600cm 的宽判据。

为什么要有这个脚本（用户 2026-10-29：`Inter_MainRoad_R003_-0540_I06`
有时候查不到裸奔）
--------------------------------------------------------------------
`audit_traffic_health.py` 问的是「终点 600cm 内有没有一条起点同向（dot>0.5）的
样条」。那是个**几何存在性**判据，和车那一帧能不能扫到下一段的 Box 是两件事。
从 `Saved/bp_graph.txt` 的 `TraceForNewPath` 实测：

    Start  = GetActorLocation()
    End    = Start + Forward × GetFrameRate() × (CurrentSpeed × 5)
    球半径 = 150
    ObjectTypes = 创建数组(...)        ← 本脚本会把真实通道打出来

**扫描长度和车速成正比**，于是有效触及距离是

    reach ≈ 150（球半径）+ FrameRate × 5 × 车速 + 下家 Box 沿行驶方向的半尺寸

车速 500 和 1300 的 reach 差出几十到上百厘米。接缝落在「慢车不够、快车够」
那一段的话，**同一个段会时而裸奔时而正常**——这就是「有时候」。体检的 600
差不多是最好情况 reach 的两倍，所以这类段它一律报「有下家」。

而且扫描是**沿车头的一条直线**：弯道上下家的 Box 会横向偏出去（偏移 ≈ L²/2R），
所以除了纵向距离还要量**横向偏移**，否则会把「够长但偏出去了」误判成能接上。

本脚本对每个目标段的每条样条（主 + SplineLeft + SplineRight，车是**随机选一条**的）
算出：纵向距离、横向偏移、以及**接得上所需的最低车速**。
TARGETS 留空则扫全图，列出所有「所需车速 > SLOW_SPEED」的段。

只读，不改任何东西。结果写到 Saved/diag_handoff_reach.txt
用法：py diag_handoff_reach.py
"""

import math
import traceback

import unreal

# 留空 = 扫全图。填了就只看这几个（名字写 actor 标签）。
TARGETS = ["Inter_BranchRoad_R037_-0180_I03",
           "Lane_MainRoad_R003_-0540_S06"]

SWEEP_RADIUS = 150.0     # TraceForNewPath 的球半径，实测写死在图里
SPEED_MUL = 5.0          # End 公式里那个 × 5，实测写死在图里
SLOW_SPEED = 500.0       # 当前 spawn_test_cars.py 的最低车速
FAST_SPEED = 1300.0      # 当前最高车速
SEARCH_R = 2000.0        # 找候选下家的半径，比体检的 600 大，才看得到"差一点"的
LINK_DOT = 0.5           # 同向判据：只看位置会把对向车道当成下家
MAIN_KEYS = ("Spline", "SplineComponent")
CAR_BP = "/Game/Vehicle/BP_car_base"

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "diag_handoff_reach.txt"
lines = []


def w(s=u""):
    lines.append(str(s))
    unreal.log("[reach] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def pts_of(sp):
    return [sp.get_location_at_spline_point(i, WS)
            for i in range(sp.get_number_of_spline_points())]


def dir_at(sp, at_start):
    n = sp.get_number_of_spline_points()
    if n < 2:
        return None
    i, j = (0, 1) if at_start else (n - 1, n - 2)
    p, q = sp.get_location_at_spline_point(i, WS), sp.get_location_at_spline_point(j, WS)
    dx, dy = (q.x - p.x, q.y - p.y) if at_start else (p.x - q.x, p.y - q.y)
    L = math.sqrt(dx * dx + dy * dy)
    return None if L < 1e-3 else (dx / L, dy / L)


def d2(p, q):
    return math.sqrt((p.x - q.x) ** 2 + (p.y - q.y) ** 2)


def read_frame_rate(eas):
    u"""读 UpdateFrameRate。**优先从关卡里的车实例读，不是 CDO。**

    实例可能带覆盖（CLAUDE.md 记过：实测 10 辆车还存着好几代之前的
    MaxBlockedTimeAtIntersection=8），而我们要算的正是真实跑着的那些车的扫描长度。
    值不一致时全部列出来——「不同的车 reach 不一样」本身就是偶发裸奔的来源之一。

    `UBlueprint.generated_class` 用 get_editor_property **读不到**（protected，
    实测报 Failed to find property）。所以 CDO 兜底走 BlueprintEditorLibrary
    或 load_object 拿 `_C` 类，并且每条路都打日志——静默返回 None 会让
    「读不到」伪装成「这段代码没跑」。
    """
    names = ("UpdateFrameRate", "update_frame_rate", "FrameRate")
    vals = {}
    for a in eas.get_all_level_actors():
        cn = a.get_class().get_name()
        if not cn.startswith("BP_car_base"):
            continue
        for n in names:
            try:
                v = float(a.get_editor_property(n))
                vals.setdefault(round(v, 6), []).append(a.get_actor_label())
                break
            except Exception:
                continue
    if vals:
        notes = [u"关卡里 %d 辆车" % sum(len(x) for x in vals.values())]
        for v in sorted(vals):
            notes.append(u"%.6f × %d 辆" % (v, len(vals[v])))
        if len(vals) > 1:
            notes.append(u"**值不一致**，下面按最小值算（最保守）")
        return min(vals), u"；".join(notes)

    bp = unreal.EditorAssetLibrary.load_asset(CAR_BP)
    if bp is None:
        return None, u"关卡里没有车，且加载不到 %s" % CAR_BP
    if bp.get_class().get_name() == "ObjectRedirector":
        return None, u"%s 是 ObjectRedirector（资产搬过位置），改 CAR_BP" % CAR_BP
    gen = None
    for how, fn in ((u"BlueprintEditorLibrary.generated_class",
                     lambda: unreal.BlueprintEditorLibrary.generated_class(bp)),
                    (u"load_object(_C)",
                     lambda: unreal.load_object(None, CAR_BP + "." +
                                                CAR_BP.rsplit("/", 1)[-1] + "_C"))):
        try:
            gen = fn()
            if gen is not None:
                w(u"  （CDO 兜底：%s 成功）" % how)
                break
        except Exception as exc:
            w(u"  （CDO 兜底：%s 失败 —— %s）" % (how, exc))
    if gen is None:
        return None, u"关卡里没有车，且拿不到生成类"
    try:
        cdo = unreal.get_default_object(gen)
    except Exception as exc:
        return None, u"get_default_object 失败：%s" % exc
    for n in names:
        try:
            return float(cdo.get_editor_property(n)), u"%s（读自 CDO，关卡里没有车）" % n
        except Exception:
            continue
    return None, u"CDO 上也读不到 UpdateFrameRate"


def box_frame(a):
    u"""下家 Box 的中心、沿行驶方向半尺寸、横向半尺寸。Box 跟着行驶方向转过，
    所以要用它自己的 forward/right，不能用世界轴。"""
    for c in a.get_components_by_class(unreal.BoxComponent):
        e = c.get_unscaled_box_extent()
        sc = a.get_actor_scale3d()
        try:
            center, rot = c.get_world_location(), c.get_world_rotation()
        except Exception:
            center, rot = a.get_actor_location(), a.get_actor_rotation()
        f = unreal.MathLibrary.get_forward_vector(rot)
        r = unreal.MathLibrary.get_right_vector(rot)
        return center, e.x * sc.x, e.y * sc.y, (f.x, f.y), (r.x, r.y)
    return None


def min_speed_for(longitudinal, half_along, frame_rate):
    u"""扫到它所需的最低车速。reach = R + FrameRate×5×v + 半尺寸 ≥ 纵向距离。"""
    need = longitudinal - SWEEP_RADIUS - half_along
    if need <= 0:
        return 0.0
    denom = frame_rate * SPEED_MUL
    if denom <= 0:
        return float("inf")
    return need / denom


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    w(u"=== 交接触及距离诊断（只读）===")
    w(u"关卡：%s" % (world.get_name() if world else u"?"))

    frame_rate, fr_note = read_frame_rate(eas)
    w(u"UpdateFrameRate：%s" % fr_note)
    if frame_rate is None:
        w(u"!! 读不到 FrameRate，**不按估值继续**——估出来的「所需车速」会被当成实测看。")
        return
    w(u"扫描长度 = %.4f × 5 × 车速 → 车速 %.0f 时 %.0f cm、%.0f 时 %.0f cm"
      % (frame_rate, SLOW_SPEED, frame_rate * SPEED_MUL * SLOW_SPEED,
         FAST_SPEED, frame_rate * SPEED_MUL * FAST_SPEED))
    w(u"球半径 %.0f cm；有效触及 = 球半径 + 扫描长度 + 下家 Box 沿行驶方向半尺寸"
      % SWEEP_RADIUS)
    w(u"")

    segs = []
    for a in eas.get_all_level_actors():
        lbl = a.get_actor_label()
        if not (lbl.startswith("Lane_") or lbl.startswith("Inter_")):
            continue
        comps = {c.get_name(): c for c in a.get_components_by_class(unreal.SplineComponent)}
        if not comps:
            continue
        bf = box_frame(a)
        starts = {}
        for nm, c in comps.items():
            if c.get_number_of_spline_points() >= 2:
                starts[nm] = (pts_of(c)[0], dir_at(c, True))
        segs.append({"a": a, "lbl": lbl, "comps": comps, "box": bf, "starts": starts})
    w(u"全图段 %d 个" % len(segs))

    targets = [s for s in segs if s["lbl"] in TARGETS] if TARGETS else segs
    if TARGETS:
        miss = [t for t in TARGETS if not any(s["lbl"] == t for s in segs)]
        for t in miss:
            w(u"!! 找不到 %s（名字变了？重建后序号会变）" % t)
    w(u"目标段 %d 个%s" % (len(targets), u"" if TARGETS else u"（全图模式）"))
    w(u"")

    flagged = []
    for s in sorted(targets, key=lambda x: x["lbl"]):
        header_done = False
        for nm in sorted(s["comps"]):
            sp = s["comps"][nm]
            npt, slen = sp.get_number_of_spline_points(), sp.get_spline_length()
            e_dir = dir_at(sp, False) if npt >= 2 else None
            if e_dir is None:
                # 零切线 → GetRotationAtDistanceAlongSpline 内部 MakeFromXZ 退化
                # → 前向掉回世界 +X，车朝正东开进虚空。**选到它的车必裸奔。**
                if not header_done:
                    w(u"%s" % s["lbl"])
                    header_done = True
                w(u"  %s   %d 点 / %.0f cm   → **切线为零，选到它的车必裸奔**"
                  u"（前向退化成世界 +X）" % (nm, npt, slen))
                flagged.append((s["lbl"], nm, float("inf"), u"切线为零"))
                continue
            e_pt = pts_of(sp)[-1]

            cands = []
            for o in segs:
                if o is s or o["box"] is None:
                    continue
                for onm, (o_pt, o_dir) in o["starts"].items():
                    if onm not in MAIN_KEYS:
                        continue          # Box 只有一个，按主样条的起点代表这个段
                    if o_dir is None or d2(o_pt, e_pt) > SEARCH_R:
                        continue
                    if (o_dir[0] * e_dir[0] + o_dir[1] * e_dir[1]) <= LINK_DOT:
                        continue
                    bc, half_along, half_lat, bf, br = o["box"]
                    vx, vy = bc.x - e_pt.x, bc.y - e_pt.y
                    lon = vx * e_dir[0] + vy * e_dir[1]          # 沿车头
                    lat = abs(-vx * e_dir[1] + vy * e_dir[0])    # 横向偏移
                    if lon <= 0:
                        continue                                 # 在身后
                    # 横向能不能罩住：球半径 + Box 横向半尺寸
                    lat_ok = lat <= (SWEEP_RADIUS + half_lat)
                    v_need = min_speed_for(lon, half_along, frame_rate)
                    cands.append((v_need, o["lbl"], lon, lat, lat_ok,
                                  half_along, half_lat))
            cands.sort()

            best = cands[0] if cands else None
            risky = (best is None) or (not best[4]) or (best[0] > SLOW_SPEED)
            if not risky and not TARGETS:
                continue
            if not header_done:
                w(u"%s" % s["lbl"])
                header_done = True
            w(u"  %s   %d 点 / %.0f cm   终点 (%.0f, %.0f, %.0f)  出向 (%.2f, %.2f)"
              % (nm, npt, slen, e_pt.x, e_pt.y, e_pt.z, e_dir[0], e_dir[1]))
            if not cands:
                w(u"    下家：无（%.0f cm 内没有同向的）→ **每辆车都裸奔**" % SEARCH_R)
                flagged.append((s["lbl"], nm, float("inf"), u"无下家"))
                continue
            for v_need, olbl, lon, lat, lat_ok, ha, hl in cands[:3]:
                if v_need == 0.0:
                    verdict = u"任何车速都接得上"
                elif v_need > FAST_SPEED:
                    verdict = u"**连最快的 %.0f 都接不上**" % FAST_SPEED
                elif v_need > SLOW_SPEED:
                    verdict = u"**车速 ≥ %.0f 才接得上（慢车裸奔）**" % v_need
                else:
                    verdict = u"车速 ≥ %.0f（低于当前最低车速，没问题）" % v_need
                w(u"    → %-44s 纵向 %.0f  横向 %.0f（上限 %.0f）%s  %s"
                  % (olbl, lon, lat, SWEEP_RADIUS + hl,
                     u"" if lat_ok else u" **偏出扫描管**", verdict))
            if risky:
                flagged.append((s["lbl"], nm, best[0],
                                u"横向偏出" if not best[4] else u"需车速 %.0f" % best[0]))
        if header_done:
            w(u"")

    w(u"=" * 70)
    if TARGETS:
        w(u"目标段查完。上面每条样条都给了「所需最低车速」——车是**随机选三条之一**的，"
          u"所以只要有一条不行，表现就是偶发裸奔。")
    else:
        w(u"全图共 %d 条样条属于「慢车接不上 / 偏出扫描管 / 没有下家」：" % len(flagged))
        for lbl, nm, v, why in sorted(flagged, key=lambda x: -x[2])[:60]:
            w(u"  %-46s %-12s %s" % (lbl, nm, why))
    w(u"")
    w(u"注意两条**本脚本的近似**，别当成精确结论：")
    w(u"  1. 扫描起点取的是上一段样条的终点，车实际位置会差小半帧的位移。")
    w(u"  2. 球扫 vs Box 的命中按「纵向 / 横向」分开判，真实是扫掠体和 OBB 求交；"
      u"擦边的情形两者会有出入。")
    w(u"要真实证据就在 PIE 里用 watch_stuck_intersection.py 那种每帧回调去采样。")


try:
    run()
except Exception:
    w(u"!! 脚本异常：")
    w(traceback.format_exc())
flush()
w(u"报告：%s" % OUT)
