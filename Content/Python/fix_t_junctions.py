# -*- coding: utf-8 -*-
"""处理 T 形路口：直行合并到右转，没人走的那条路口段直接删掉。

判据不去认几何形状，只看**每条路口段首尾有没有车道接得上**。
车沿样条从第 0 个点开到最后一个点，所以：

    进得来 = 有某条 Lane_ 的终点落在本段起点附近（车从那条车道开进路口）
    出得去 = 有某条 Lane_ 的起点落在本段终点附近（出了路口还有路可走）

四种组合，三种处理：
    进得来 + 出得去   十字路口的正常情况，不动
    进得来 + 出不去   T 形岔路的**进入段**：直行开出去就掉进虚空了。
                      把直行样条（Spline）写成右转曲线的副本——
                      车抓到三条里的任何一条都是右转，蓝图不用特判。
    进不来 + 出得去   "从对面出来"的段：对面根本没有路，永远不会有车从那儿来。
                      整个 actor 删掉。
    进不来 + 出不去   孤立段，同样删掉。

为什么删 actor 是安全的：这些 actor 是脚本 spawn 的（带 ClaudeGenLane tag），
删除是真删、存得住。**组件**才是删不掉的那种（重载后按蓝图 SCS 重建）。

必须跑在 fill_turn_splines.py **之后**：要拿右转曲线来覆盖直行，
转弯还没写的话拿到的是直行副本，合并等于什么都没做（会在报告里点名）。

用法：py fix_t_junctions.py
"""

import traceback

import unreal

LANE_TAG_PREFIX = "ClaudeGenLane"
LEFT_COMP, RIGHT_COMP, MAIN_COMP = "SplineLeft", "SplineRight", "Spline"

# 端点多近算"接得上"。生成时的空隙是 TARGET_GAP=150，留足余量；
# 但不能太大——路口里几条车道的端点相距也就几百，放太宽会把隔壁车道认成自己的下一段。
LINK_MAX = 600.0
# 直行和右转差多少才算"右转真的存在"。fill_turn_splines 会把用不上的
# 转弯写成直行副本，这种情况下合并毫无意义，要能分辨出来。
TURN_DIFF_MIN = 50.0

# 蓝图默认的 SplineLeft/SplineRight 是 2 个点、长 100cm、方向一律 +X 的残桩。
# 它和直行差得很远，光靠"和直行不一样"会把它误判成转弯——实测把 44 条路口段的
# 直行样条抄成了 2 点残桩（fill_turn_splines 那轮全灭、组件停在蓝图默认值）。
# 点数和长度一起看：真转弯是 SAMPLES+1 个点、长度上千。
STUB_MAX_POINTS = 3
STUB_MAX_LEN = 300.0

DRY_RUN = False          # True = 只报告不动手
DELETE_ORPHANS = True    # False = 孤儿段只报告不删

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "t_junctions.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def comp_by_name(a, name):
    for c in a.get_components_by_class(unreal.SplineComponent):
        if c.get_name() == name:
            return c
    return None


DOT_SAME_DIR = 0.5       # 同向判据，夹角 60 度以内
DIR_SAMPLE = 100.0       # 求方向时两个采样点的间距


def norm2d(dx, dy):
    h = (dx * dx + dy * dy) ** 0.5
    # 取不到方向就返回 None。兜底成 (1,0) 会把"没方向"伪装成"朝 +X"。
    return (dx / h, dy / h) if h > 1e-4 else None


def dir_at(sp, at_start):
    """样条首端的出发方向 / 末端的离开方向。

    get_direction_at_distance_along_spline 在这些样条上返回零向量，
    一律用两个采样点作差。
    """
    L = sp.get_spline_length()
    if L < 1e-3:
        return None
    step = min(DIR_SAMPLE, L * 0.4)
    if at_start:
        a = sp.get_location_at_distance_along_spline(0.0, WS)
        b = sp.get_location_at_distance_along_spline(step, WS)
    else:
        a = sp.get_location_at_distance_along_spline(L - step, WS)
        b = sp.get_location_at_distance_along_spline(L, WS)
    return norm2d(b.x - a.x, b.y - a.y)


def is_stub(sp):
    """是不是蓝图默认的那根 100cm 小棍子，而不是一条真转弯。"""
    try:
        return (sp.get_number_of_spline_points() <= STUB_MAX_POINTS
                and sp.get_spline_length() < STUB_MAX_LEN)
    except Exception:
        return False


def pts_of(sp):
    n = sp.get_number_of_spline_points()
    return [sp.get_location_at_spline_point(i, WS) for i in range(n)]


def dist(p, q):
    return ((p.x - q.x) ** 2 + (p.y - q.y) ** 2 + (p.z - q.z) ** 2) ** 0.5


def mark_edited(sp):
    """打 "Override Construction Script" 标记，否则构造脚本重跑会冲掉点。"""
    for n in ("spline_has_been_edited", "b_spline_has_been_edited"):
        try:
            sp.set_editor_property(n, True)
            return True
        except Exception:
            continue
    return False


def dedup_pts(pts, eps=2.0):
    """去掉相邻重合的点。

    两个点坐标相同时，自动切线必然是零，点型改成 CURVE 也救不回来——
    GetRotationAtDistanceAlongSpline 内部的 MakeFromXZ(切线, Up) 照样退化，
    前向掉回世界 +X。实测 形状7（生成时打空 140 点、高度全靠插值）
    的路口段首尾就是这种重合点。
    去重只丢弃冗余信息，曲线形状不变。
    """
    out = []
    for p in pts:
        if out:
            q = out[-1]
            if ((p.x - q.x) ** 2 + (p.y - q.y) ** 2
                    + (p.z - q.z) ** 2) ** 0.5 < eps:
                continue
        out.append(p)
    return out


def write_spline(sp, pts):
    """把点写进样条。modify() 必须在改动之前调。

    clear/add/update_spline 都是普通函数调用，只改内存、不把对象标记为已修改，
    保存时整个对象不被序列化——写完立刻读回是好的，存盘重载就变回默认值。
    """
    # mark_edited 必须在写点**之前**调。它是 set_editor_property，
    # 而在组件上改属性会触发这个 actor 重跑构造脚本、把组件整批重建——
    # 放在最后调的话，刚写进去的点正好落在被丢弃的那批组件上
    # （名字变成 TRASH_SplineComponent_xxxx，回读全部对不上）。
    # 这个标记是实例覆盖、会存住，所以第二次跑时属性值没变、
    # 不再触发重跑，点就留住了——"同一个脚本要跑好几次才生效"就是这么来的。
    marked = mark_edited(sp)
    try:
        sp.modify(True)
        owner = sp.get_owner()
        if owner is not None:
            owner.modify(True)
    except Exception:
        pass
    sp.clear_spline_points(False)
    for p in dedup_pts(pts):
        sp.add_spline_point(p, WS, False)
    for i in range(sp.get_number_of_spline_points()):
        sp.set_spline_point_type(i, unreal.SplinePointType.CURVE_CLAMPED, False)
    # 首尾两点必须用 CURVE 而不是 CURVE_CLAMPED。
    # "Clamped" 的定义行为就是把端点和局部极值处的切线归零，而
    # GetRotationAtDistanceAlongSpline 内部是 MakeFromXZ(切线, Up)——
    # 切线为零就退化，前向掉回世界 +X。实测端点切线长恒为 0、
    # 旋转前向恒为 0.0°，而真实走向是 -96°，车的探测球因此在
    # "未来点被钳到样条末端"的那一两帧甩向正右方。
    # CURVE 的自动切线指向邻点，非零，曲线形状几乎不变。
    n_pts = sp.get_number_of_spline_points()
    if n_pts >= 2:
        for i in (0, n_pts - 1):
            sp.set_spline_point_type(i, unreal.SplinePointType.CURVE, False)
    sp.update_spline()
    return marked


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    lanes, lane_objs, inters = [], [], []
    for a in eas.get_all_level_actors():
        try:
            if not unreal.SystemLibrary.is_valid(a) or a.is_actor_being_destroyed():
                continue
        except Exception:
            pass
        if not any(str(t).startswith(LANE_TAG_PREFIX) for t in a.tags):
            continue
        lbl = a.get_actor_label()
        sp = comp_by_name(a, MAIN_COMP)
        if sp is None:
            sp = a.get_component_by_class(unreal.SplineComponent)
        if sp is None or sp.get_number_of_spline_points() < 2:
            continue
        p = pts_of(sp)
        if lbl.startswith("Lane_"):
            lanes.append((lbl, p[0], p[-1]))
            lane_objs.append((lbl, a, sp))
        elif lbl.startswith("Inter_"):
            inters.append((lbl, a, sp, p[0], p[-1]))

    inters.sort(key=lambda t: t[0])
    w("车道段 %d 条，路口段 %d 条" % (len(lanes), len(inters)))
    w("接得上的判据：端点相距 < %.0fcm" % LINK_MAX)
    w("")
    if not inters:
        w("!! 没有路口段，先跑 gen_traffic_lanes.py")
        flush()
        return

    lane_starts = [(lbl, s) for lbl, s, _e in lanes]
    lane_ends = [(lbl, e) for lbl, _s, e in lanes]

    straight_pts = {}      # 合并之前的直行点，写"直行副本"要用原版
    normal, merge, orphan = [], [], []
    for lbl, a, sp, p0, pn in inters:
        straight_pts[lbl] = pts_of(sp)
        # 进得来：有车道的**终点**落在本段起点附近
        fed = next((n for n, q in lane_ends if dist(q, p0) < LINK_MAX), None)
        # 出得去：有车道的**起点**落在本段终点附近
        out = next((n for n, q in lane_starts if dist(q, pn) < LINK_MAX), None)
        if fed and out:
            normal.append(lbl)
        elif fed:
            merge.append((lbl, a, sp, fed))
        else:
            orphan.append((lbl, a, out))

    w("正常（两头都接得上）    %d 条" % len(normal))
    w("T 形进入段（出不去）    %d 条 -> 直行合并到右转" % len(merge))
    w("孤儿段（进不来）        %d 条 -> 删除" % len(orphan))
    w("")
    flush()

    # --- 第一步：删孤儿段 ---
    # 必须排在最前面。后面判断"转弯有没有下家"要用存活段的起点当锚点，
    # 孤儿段的起点本身就在虚空里，留着它会把废转弯认成有效的。
    w("=" * 86)
    w("① 孤儿段：没有任何车道能开进来")
    w("=" * 86)
    killed = 0
    dead = set()
    for lbl, a, out in orphan:
        tail = ("终点接 %s" % out[:24]) if out else "两头都不接"
        if DRY_RUN or not DELETE_ORPHANS:
            w("  %-34s 会删除（%s）" % (lbl[:34], tail))
            dead.add(lbl)      # DRY_RUN 下也当它已经没了，后面的判断才准
            continue
        try:
            eas.destroy_actor(a)
            killed += 1
            dead.add(lbl)
            w("  %-34s 已删除（%s）" % (lbl[:34], tail))
        except Exception as exc:
            w("  %-34s !! 删除失败：%s" % (lbl[:34], str(exc)[:50]))
    if not orphan:
        w("  没有。")

    # --- 第二步：废转弯写成直行副本 ---
    # T 形路口的主路，往"没有岔路的那一侧"是转不过去的：那个方向的转弯
    # 终点落在刚删掉的孤儿段上（或者干脆没有下家），车拐过去就掉进虚空。
    # 判据和上面同一条——终点有没有下家，只是锚点换成"车道起点 + 存活路口段起点"。
    # 相邻路口之间可能没有车道段（路口挨着路口），所以两种锚点都要认。
    # 锚点要带上"出发方向"。只看位置的话，对向车道的起点也在附近
    # （实测 360cm），会把逆行当成合法下家——Inter_形状13_-0540_I03 和
    # Inter_形状13_+0180_I03 的废转弯就是这么漏过去的。
    anchors = []
    for nm, a2, sp2, p0, _pn in inters:
        if nm in dead:
            continue
        d = dir_at(sp2, True)
        if d is not None:
            anchors.append((nm, p0, d))
    for nm, a2, sp2 in lane_objs:
        d = dir_at(sp2, True)
        if d is not None:
            anchors.append((nm, pts_of(sp2)[0], d))

    w("")
    w("=" * 86)
    w("② 转弯终点没有下家：写成直行副本（等于取消这个方向的转弯）")
    w("=" * 86)
    neutral = nfail = 0
    stubs = []
    for lbl, a, sp, p0, pn in inters:
        if lbl in dead:
            continue
        base = straight_pts[lbl]
        for cname in (LEFT_COMP, RIGHT_COMP):
            c = comp_by_name(a, cname)
            if c is None or c.get_number_of_spline_points() < 2:
                continue
            cp = pts_of(c)
            # 残桩：fill_turn_splines 没写进去（或根本没跑），组件停在蓝图默认值。
            # 顺手写回直行副本，但要报出来——这说明转弯那步失败了，得重跑。
            if is_stub(c):
                stubs.append("%s %s" % (lbl, cname))
                if not DRY_RUN:
                    write_spline(c, base)
                continue
            # 已经是直行副本的不用管
            if max(dist(cp[0], base[0]), dist(cp[-1], base[-1])) < TURN_DIFF_MIN:
                continue
            out = dir_at(c, False)
            nxt = None
            for n, q, nd in anchors:
                if n == lbl or dist(q, cp[-1]) >= LINK_MAX:
                    continue
                dot = (out[0] * nd[0] + out[1] * nd[1]) if out else -9.0
                if dot > DOT_SAME_DIR:   # 反向的不算下家，拐过去是逆行
                    nxt = n
                    break
            if nxt is not None:
                continue
            if DRY_RUN:
                w("  %-34s %-12s 会写成直行副本（终点无下家）"
                  % (lbl[:34], cname))
                neutral += 1
                continue
            if write_spline(c, base):
                w("  %-34s %-12s 已写成直行副本" % (lbl[:34], cname))
                neutral += 1
            else:
                w("  %-34s %-12s !! 打不上 Override 标记" % (lbl[:34], cname))
                nfail += 1
    if not neutral and not nfail:
        w("  没有。每条转弯都接得上下一段。")

    # --- 第三步：直行合并到右转 ---
    # 排在第二步之后：如果右转刚被判定为废转弯、已经写回直行，
    # 这里合并就是个空操作，会被下面的"没得合并"识破并点名。
    w("")
    w("=" * 86)
    w("③ T 形进入段：把直行样条写成右转曲线")
    w("=" * 86)
    done = same = noturn = failed = 0
    for lbl, a, sp, fed in merge:
        if lbl in dead:
            continue
        right = comp_by_name(a, RIGHT_COMP)
        if right is None or right.get_number_of_spline_points() < 2:
            w("  %-34s !! 没有 %s 组件，跳过" % (lbl[:34], RIGHT_COMP))
            noturn += 1
            continue
        # 残桩绝不能当右转抄进直行——那是在破坏直行样条，不是合并。
        # 实测这么干过一次：44 条直行全变成 2 点 100cm 的小棍子。
        if is_stub(right):
            w("  %-34s !! %s 是蓝图默认残桩，拒绝合并" % (lbl[:34], RIGHT_COMP))
            noturn += 1
            continue
        rp = pts_of(right)
        mp = pts_of(sp)
        # 右转如果就是直行副本，合并没有任何意义——说明 fill_turn_splines
        # 当时没给这条段找到右转目标（对向车道缺失），要先解决那个。
        d = max(dist(rp[0], mp[0]), dist(rp[-1], mp[-1]))
        if d < TURN_DIFF_MIN:
            w("  %-34s 右转就是直行副本（端点差 %.0f），没得合并" % (lbl[:34], d))
            same += 1
            continue
        if DRY_RUN:
            w("  %-34s 会合并（右转终点偏离直行 %.0fcm）  上游 %s"
              % (lbl[:34], d, fed[:24]))
            done += 1
            continue
        if write_spline(sp, rp):
            w("  %-34s 已合并，%d 点  上游 %s" % (lbl[:34], len(rp), fed[:24]))
            done += 1
        else:
            w("  %-34s !! 写了但打不上 Override 标记，构造脚本会冲掉" % lbl[:34])
            failed += 1
    if not merge:
        w("  没有。")

    w("")
    w("=" * 86)
    w("合计")
    w("=" * 86)
    w("删除孤儿段 %d 条，废转弯写成直行 %d 条，直行合并到右转 %d 条"
      % (killed, neutral, done))
    if nfail:
        w("!! %d 条废转弯打不上 Override 标记，存不住。" % nfail)
    if stubs:
        w("")
        w("!! 发现 %d 条蓝图默认残桩（2 点 100cm），已写回直行副本。" % len(stubs))
        w("   这说明 fill_turn_splines.py 那步**没有生效**——先去看它的报告，")
        w("   「同一次运行内回读」应该是「全部一致」。它必须**单独一次 py 执行**，")
        w("   和 gen_traffic_lanes.py 挤在同一次里写出来的全是作废组件。")
        w("   修好之后重跑转弯，再跑本脚本。")
    if same:
        w("!! %d 条没得合并：它们的右转本身就是直行副本。" % same)
        w("   两种可能：② 里刚把它判成废转弯写回了直行（那说明这条 T 形岔路")
        w("   连右转都无处可去，整条段其实该删）；或者 fill_turn_splines 当初")
        w("   就没给它找到右转目标。看 ② 的列表里有没有它就能分辨。")
    if noturn:
        w("!! %d 条没有 %s 组件。" % (noturn, RIGHT_COMP))
    if failed:
        w("!! %d 条打不上 Override Construction Script 标记，存不住。" % failed)
    w("")
    w("注意：重跑 gen_traffic_lanes.py 会重建全部 actor，这两项改动都会没。")
    w("      顺序是 车道 -> 路口盒 -> 转弯 -> 本脚本，rebuild_traffic.py 里已固化。")
    if DRY_RUN:
        w("")
        w("DRY_RUN=True，什么都没改。")
    w("关卡尚未保存。")
    flush()
    unreal.log("[tfix] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[tfix] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
