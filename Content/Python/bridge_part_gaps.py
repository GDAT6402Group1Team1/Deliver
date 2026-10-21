# -*- coding: utf-8 -*-
"""把被切成 _PartNN 的路，在切点处用一段合成样条连起来。

为什么需要：队友把 5 条支路在路口处切成了多段（R024/R035/R036/R038/R039），
两段端点之间有 1283~1677cm 的**物理缺口，里面压根没有源样条**，
所以生成器铺不出车道。而文档里记着 TARGET_GAP 设到 250 时实测车就接不上下一段，
1283 必然接不上——车会脱离样条裸奔、靠 TraceForNewPath 现找路，
找不到就在 5 秒后被 OnRouteLost 瞬移回出生点。

做法：**按已生成车道的实际端点来桥接**，不改主生成器。
理由有两条：
  1. 桥接要知道「两段的哪一头朝着对方」。量生成结果最准——
     spawn_lane 对非前向车道会把点序反转，所以生成出来的样条
     **第 0 点就是入口、最后一点就是出口**，直接拿来配对就天然带方向，
     不用再去推 LEFT_HAND_TRAFFIC 和 offset 符号的组合。
  2. 不动已经调通的那条主路径。这也是 fill_turn_splines.py /
     fix_t_junctions.py 一贯的做法：后置、只读已有结果、按 tag 幂等。

桥接段用 **BP_TrafficLine1_IntersectionChild**（和路口段同一个类）而不是普通车道：
切点处就是路口，横穿的路从这儿过，所以这段应该归 BP_Intersection 管、受红绿灯控制。
用普通车道的话车会无视红灯直接穿过去。

生成物 tag = ClaudeGenBridge，可重复运行（先删同 tag 的再建），
不和 ClaudeGenLane 混，所以重跑 gen_traffic_lanes.py 不会误删它、
也不会被它误删——但**车道一重新生成，端点就挪位了，这个脚本要跟着重跑**。

中文里一律用「」不用半角引号。
用法：py bridge_part_gaps.py
"""

import re
import traceback

import unreal

CHILD_BP = "/Game/PS2DEM/BP_TrafficLine1_IntersectionChild"
TAG = "ClaudeGenBridge"
INTER_BOX_HALF = (32.0, 64.0, 300.0)   # 和 gen_traffic_lanes.py 的 Inter_* 一致
WS = unreal.SplineCoordinateSpace.WORLD

STEP = 200.0            # 桥接样条的采样间距
MAX_GAP = 4000.0        # 超过这个距离就不认为是「切点」，不桥
                        # 实测真切点缺口 537~1677；R036_Part01↔Part03 那种
                        # 相距 24915 的是一条路的两个远端，不是切点

OUT = unreal.Paths.project_saved_dir() + "bridge_part_gaps.txt"
lines = []


def w(s=""):
    lines.append(str(s))


# Lane_BranchRoad_R038_Part01_+0180_S03 / Inter_..._I02
SEG_RE = re.compile(r"^(Lane|Inter)_(.+?)_([+-]\d{4})_[SI]\d+$")


def parse_label(label):
    """拆出 (种类, 路名, 偏移标签)；不是生成物就返回 None。"""
    m = SEG_RE.match(str(label))
    if not m:
        return None
    return m.group(1), m.group(2), m.group(3)


def base_road_name(name):
    return re.sub(r"_[Pp]art\d+$", "", str(name))


def part_index(name):
    m = re.search(r"_[Pp]art(\d+)$", str(name))
    return int(m.group(1)) if m else None


def dist2(a, b):
    return ((a.x - b.x) ** 2 + (a.y - b.y) ** 2) ** 0.5


def mark_edited(sp):
    """不打这个标记，构造脚本一重跑就把点冲回蓝图默认的 2 个点。"""
    for name in ("spline_has_been_edited", "b_spline_has_been_edited"):
        try:
            sp.set_editor_property(name, True)
            return True
        except Exception:
            continue
    return False


def main():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    # ---------------------------------------------------------------- 先清旧的
    old = 0
    for a in actors:
        try:
            if TAG in set(str(t) for t in a.tags):
                eas.destroy_actor(a)
                old += 1
        except Exception:
            continue
    w("清除旧桥接段 %d 个" % old)

    # ---------------------------------------------------------------- 收集链段
    # key = (基路名, 偏移标签, 分段号) -> [(第0点, 最后一点, 标签)]
    chains = {}
    for a in actors:
        try:
            parsed = parse_label(a.get_actor_label())
        except Exception:
            continue
        if not parsed:
            continue
        _kind, road, otag = parsed
        pidx = part_index(road)
        if pidx is None:
            continue                      # 没被切的路不用桥
        try:
            sp = a.get_component_by_class(unreal.SplineComponent)
            n = sp.get_number_of_spline_points()
            if n < 2:
                continue
            p0 = sp.get_location_at_spline_point(0, WS)
            p1 = sp.get_location_at_spline_point(n - 1, WS)
        except Exception:
            continue
        chains.setdefault((base_road_name(road), otag, pidx), []).append(
            (p0, p1, a.get_actor_label()))

    w("找到被切路的车道链 %d 组（基路名 × 偏移 × 分段）" % len(chains))
    if not chains:
        w("")
        w("一组都没有——先跑 rebuild_traffic.py 生成车道，再跑本脚本。")
        return

    # ---------------------------------------------------------------- 逐个桥
    cls = unreal.EditorAssetLibrary.load_asset(CHILD_BP).generated_class()
    groups = {}
    for (base, otag, pidx) in chains:
        groups.setdefault((base, otag), []).append(pidx)

    made = 0
    skipped = 0
    for (base, otag) in sorted(groups):
        idxs = sorted(groups[(base, otag)])
        for k in range(len(idxs) - 1):
            ia, ib = idxs[k], idxs[k + 1]
            A = chains[(base, otag, ia)]
            B = chains[(base, otag, ib)]

            # 方向靠「谁的出口挨着谁的入口」定：生成出来的样条第 0 点是入口、
            # 最后一点是出口（spawn_lane 对反向车道已经把点序反转过），
            # 所以这一步天然带方向，不用再推 LEFT_HAND_TRAFFIC 和偏移符号。
            best = None
            for (a0, a1, la) in A:
                for (b0, b1, lb) in B:
                    d = dist2(a1, b0)          # A 的出口 -> B 的入口
                    if best is None or d < best[0]:
                        best = (d, a1, b0, la, lb)
                    d = dist2(b1, a0)          # B 的出口 -> A 的入口
                    if d < best[0]:
                        best = (d, b1, a0, lb, la)
            gap, src, dst, lsrc, ldst = best

            if gap > MAX_GAP:
                w("  跳过 %s %s Part%02d->Part%02d：缺口 %.0f 超过 %.0f，"
                  "不像切点（更像一条路的两个远端）"
                  % (base, otag, ia, ib, gap, MAX_GAP))
                skipped += 1
                continue
            if gap < 1.0:
                w("  跳过 %s %s Part%02d->Part%02d：缺口 %.0f，已经接上了"
                  % (base, otag, ia, ib, gap))
                skipped += 1
                continue

            # 直线采样。两端点的类型设成 CURVE，自动切线指向邻点、非零——
            # CURVE_CLAMPED 会把端点切线归零，而车的朝向取
            # GetRotationAtDistanceAlongSpline（内部 MakeFromXZ(切线, Up)），
            # 切线为零时前向退化成世界 +X，探测球会甩向正右方。
            n = max(2, int(gap / STEP) + 1)
            pts = []
            for i in range(n + 1):
                t = float(i) / n
                pts.append(unreal.Vector(src.x + (dst.x - src.x) * t,
                                         src.y + (dst.y - src.y) * t,
                                         src.z + (dst.z - src.z) * t))

            label = "Bridge_%s_%s_P%02d%02d" % (base.replace(" ", ""), otag, ia, ib)
            a = eas.spawn_actor_from_class(cls, pts[0])
            a.set_actor_label(label)
            a.tags = [TAG]
            sp = a.get_component_by_class(unreal.SplineComponent)
            sp.clear_spline_points(False)
            for p in pts:
                sp.add_spline_point(p, WS, False)
            np_ = sp.get_number_of_spline_points()
            for i in range(np_):
                sp.set_spline_point_type(i, unreal.SplinePointType.CURVE_CLAMPED, False)
            for i in (0, np_ - 1):
                sp.set_spline_point_type(i, unreal.SplinePointType.CURVE, False)
            sp.update_spline()
            if not mark_edited(sp):
                w("  !! %s 的 spline_has_been_edited 没设上，点会被构造脚本冲掉" % label)
            for c in a.get_components_by_class(unreal.BoxComponent):
                try:
                    c.modify(True)
                    c.set_editor_property("box_extent",
                                          unreal.Vector(*INTER_BOX_HALF))
                except Exception as exc:
                    w("  !! %s 的 Box 设尺寸失败: %s" % (label, str(exc)[:60]))
                break

            w("  %s  缺口 %.0f  %d 点   %s -> %s"
              % (label, gap, np_, lsrc, ldst))
            made += 1

    w("")
    w("=" * 60)
    w("桥接 %d 段，跳过 %d 组" % (made, skipped))
    w("=" * 60)
    w("桥接段用的是路口段同一个类（BP_TrafficLine1_IntersectionChild），")
    w("所以切点处仍然受 BP_Intersection 的红绿灯管——那儿确实是路口。")
    w("")
    w("接下来：")
    w("  1. **跑 gen_intersections.py**：路口盒按 Inter_* 的 Box 位置聚类，")
    w("     刚加的桥接段带 Box，不重算盒子的话它们罩不住、BP_Intersection 找不到。")
    w("  2. 重启编辑器后再跑一次本脚本比数字（应为「清除旧桥接段 %d 个」），" % made)
    w("     确认点真的存住了——脚本说写入多少只证明函数被调用过。")
    w("  3. 车道一重新生成端点就挪位了，**本脚本要跟着重跑**。")


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
