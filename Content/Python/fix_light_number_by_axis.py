# -*- coding: utf-8 -*-
"""按实际走向重写 Inter_ 段的 LightNumber：同轴同组、相交异组。

为什么要改
----------
生成器的 `set_light_number` 判的是 `|Δy|` vs `|Δx|`，也就是**沿世界 X 还是沿世界 Y**
——用的是世界坐标轴，不是两条路的相对关系。本图是自由角度路网，于是：
  · 45° 附近落哪边随机。实测 `BranchRoad_R024_Merged` 的对向车道
    133°→组1、135°→组0，**同一条路的两个方向被拆开**（夹角仅 3°）。
  · 两条互相垂直的斜路可能都被判到同一边。实测 (30587,16146) 处
    91° 和 176° 夹角 85°、却都是组 1 → 这个路口永远不交替。
审计结果：55 个路口里 10 个分组错误（audit_light_number_axes.py）。

**分组按哪个来**：按运行时蓝图实际用的那个——`BP_Intersection` 在
`Get Lane Traffic Lights` 里用 `GetOverlappingActors(ClassFilter=IntersectionChild)`
找子节点，也就是**它自己的 Box 和段的 Box 重叠**。
不能按距离聚类：2500cm 的聚类会把相邻的两个路口并成一个
（实测 (38081,25921) 那簇出现 4 个轴向、同一条路的两段差 132°，
就是两个路口被并了），按它染色会把两个路口的段混在一起。

染色：段按轴角（无向，[0,180)）聚类，容差 SAME_AXIS_TOL；
段数最多的那一类给 0，其次给 1，再有给 2、3……
蓝图那套 `Map<LightNumber,段>` + 游标轮换天然支持 N 组（Length 就是组数），
所以三岔路口给 0/1/2 是可行的，代价是每个方向等得更久。

**这一步要排在 gen_intersections.py 之后**（需要路口盒才知道分组），
而且每次重新生成车道都要重跑——重新生成出来的是全新 actor，旧值不会留。

DRY_RUN=True 只报不改。只改 LightNumber，不碰别的属性。
用法：py fix_light_number_by_axis.py
"""

import math
import re
import traceback

import unreal

DRY_RUN = False
SAME_AXIS_TOL = 30.0          # 轴角差小于此值算同轴（和审计脚本一致）

# 两条**不同的路**轴角接近时，要不要并成同一个灯组。
# **默认 False（不并）**，依据是用户 2026-10-29 在 Intersection_35 的手改：
# 那里 R008(147°) 和 R010(163°) 只差 16°、会被并进同一组，但它们是两条
# 交汇的路、车流互相冲突，同时放行不对。用户手动改成「一条路一组」=3 组。
#
# 取舍：并错了 → 两股冲突车流同时绿灯（车是运动学+Overlap，不物理撞车而是
# 互相穿过，另外前车探测会让它们互相停住、MaxBlockedTime 6 秒后强行通过）；
# 不并 → 真正切向汇入的路口多一个相位，车白等一个 LightDuration。
# 后者只是慢，前者是错，所以默认不并。
#
# 代价要知道：原来靠这个合并变成**单组**的那几个切向汇入路口
# （实测 7 个单组里有 4 个是这种）现在会变成 2 组、开始交替，
# 车在那儿会等一个红灯——那里其实没有横向车流。
# 真遇到觉得多余，把这个开关打开、或者单独把那个路口手调回去。
MERGE_ROADS_BY_AXIS = False

# 手动调过灯组的路口，填路口 actor 名（如 "Intersection_40"）。
# **手改过就必须填进来**：否则下次跑本脚本会按轴向重新染色、把手改冲掉。
# 尤其是"把两个不同轴向并进同一组"这种做法——在本脚本眼里就是
# 「相交却同组」的错误，它会好心地拆回去。
# 方向多于两个的那 5 个路口见 Document/TrafficLightGroups.md。
SKIP_INTERSECTIONS = []
# **按名字记路口是不可靠的，所以这个名单默认空着。**
# `gen_intersections.py` 每次按簇顺序重新编号（`Intersection_%02d`），
# 重建一次 `Intersection_40` 就可能变成别的物理路口。
# 2026-10-29 实测踩到：名单里留着上一轮的 33/38/39/40/41，重建后
# 这些名字指向别处——真正要人工处理的那个路口不在名单里、被自动写成了 3 组，
# 而几个正常路口被白白跳过。
#
# 要手动维护某个路口就**按坐标记**（见 Document/TrafficLightGroups.md），
# 临时填进来跑一次之后清空，别把它当长期配置。
CHILD_PREFIX = "Inter_"
INTER_PREFIX = "BP_Intersection"

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "fix_light_number_by_axis.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[lnfix] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def light_prop_name(a):
    """LightNumber 的真实属性名。蓝图变量要用原名，snake_case 只适用于原生属性。"""
    for n in ("LightNumber", "lightnumber", "light_number"):
        try:
            a.get_editor_property(n)
            return n
        except Exception:
            continue
    return None


def box_aabb(a):
    """actor 上 Box 的世界 AABB。Box 跟着行驶方向转过，所以逐轴外接（保守偏大）。"""
    for c in a.get_components_by_class(unreal.BoxComponent):
        e = c.get_unscaled_box_extent()
        sc = a.get_actor_scale3d()
        hx, hy, hz = e.x * sc.x, e.y * sc.y, e.z * sc.z
        try:
            center, rot = c.get_world_location(), c.get_world_rotation()
        except Exception:
            center, rot = a.get_actor_location(), a.get_actor_rotation()
        f = unreal.MathLibrary.get_forward_vector(rot)
        r = unreal.MathLibrary.get_right_vector(rot)
        u = unreal.MathLibrary.get_up_vector(rot)
        return center, unreal.Vector(
            abs(f.x) * hx + abs(r.x) * hy + abs(u.x) * hz,
            abs(f.y) * hx + abs(r.y) * hy + abs(u.y) * hz,
            abs(f.z) * hx + abs(r.z) * hy + abs(u.z) * hz)
    return None


def overlaps(a_box, b_box):
    (ac, ah), (bc, bh) = a_box, b_box
    for axis in ("x", "y", "z"):
        if abs(getattr(ac, axis) - getattr(bc, axis)) > getattr(ah, axis) + getattr(bh, axis):
            return False
    return True


def road_of(label):
    """Inter_<路名>_<偏移>_I<序号> -> 路名。认不出来返回整个标签，
    那样它只会和自己同组，不会误并别的路。"""
    m = re.match(r'^Inter_(.+)_([+-]\d+)_I(\d+)$', str(label))
    return m.group(1) if m else str(label)


def main_spline(a):
    best = None
    for c in a.get_components_by_class(unreal.SplineComponent):
        nm = c.get_name()
        if "Left" in nm or "Right" in nm:
            continue
        if best is None or nm in ("Spline", "SplineComponent"):
            best = c
    return best


def axis_angle(sp):
    n = sp.get_number_of_spline_points()
    if n < 2:
        return None
    p = sp.get_location_at_spline_point(0, WS)
    q = sp.get_location_at_spline_point(n - 1, WS)
    dx, dy = q.x - p.x, q.y - p.y
    if abs(dx) < 1e-6 and abs(dy) < 1e-6:
        return None
    return math.degrees(math.atan2(dy, dx)) % 180.0


def axis_diff(a, b):
    d = abs(a - b) % 180.0
    return min(d, 180.0 - d)


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    w(u"=== 按走向重写 LightNumber ===")
    w(u"关卡：%s     模式：%s"
      % (world.get_name() if world else u"?", u"只报不改（DRY_RUN）" if DRY_RUN else u"**写入**"))

    inters, segs = [], []
    for a in eas.get_all_level_actors():
        cls = a.get_class().get_name()
        lbl = a.get_actor_label()
        if cls.startswith(INTER_PREFIX) and "Child" not in cls:
            bb = box_aabb(a)
            if bb:
                inters.append((a, bb))
        elif lbl.startswith(CHILD_PREFIX):
            sp = main_spline(a)
            bb = box_aabb(a)
            if sp is None or bb is None:
                continue
            ang = axis_angle(sp)
            if ang is None:
                continue
            segs.append({"a": a, "label": lbl, "ang": ang, "box": bb})
    w(u"路口 actor %d 个、可处理的 Inter_ 段 %d 个" % (len(inters), len(segs)))
    if not inters or not segs:
        w(u"！缺一边。先跑 gen_traffic_lanes.py + gen_intersections.py。")
        return

    # 按「哪个路口盒罩住它」分组——和蓝图运行时的 GetOverlappingActors 一致
    groups = {}
    orphan = []
    for s in segs:
        owners = [ia.get_actor_label() for ia, ib in inters if overlaps(ib, s["box"])]
        if not owners:
            orphan.append(s)
            continue
        if len(owners) > 1:
            s["multi"] = owners
        groups.setdefault(owners[0], []).append(s)
    w(u"归到 %d 个路口；没有任何盒子罩住的段 %d 个" % (len(groups), len(orphan)))
    if orphan:
        w(u"  这些段运行时拿不到灯态（蓝图靠 Box 重叠找子节点），顺手列出来：")
        for s in orphan[:10]:
            w(u"      %s" % s["label"])
        w(u"  → 跑 gen_intersections.py 重算路口盒能修掉。")
    multi = [s for g in groups.values() for s in g if "multi" in s]
    if multi:
        w(u"  被**两个以上**盒子同时罩住的段 %d 个（两个路口会抢着写它的灯态）：" % len(multi))
        for s in multi[:8]:
            w(u"      %-34s %s" % (s["label"][:34], u",".join(s["multi"])))

    prop_cache = {}
    changed, same, failed = 0, 0, 0
    multi_axis = []
    w(u"")
    skipped_names = []
    for iname, ss in sorted(groups.items()):
        if iname in SKIP_INTERSECTIONS:
            skipped_names.append(iname)
            continue
        # ---- 先按**路**分组，再用轴角决定路与路之间是否合并 ----
        # 为什么不直接按轴角聚：轴角是按段的**首末连线**量的，而路在路口处
        # 可能正好弯着。实测 Intersection_38 是标准 T 字（R010 直行 4 段 +
        # R003 的 2 段），但 R003 那两条车道的弦方向差了 38°，超过 30° 容差
        # 就被拆成两个"轴"，于是一个 T 字路口被误判成三岔、要开三相位。
        # **同一条路在一个路口里必然同组**，这个判据比弦角可靠得多。
        by_road = {}
        for s in sorted(ss, key=lambda x: x["ang"]):
            by_road.setdefault(s.get("road") or road_of(s["label"]), []).append(s)
        axes = []
        for _rd, members in sorted(by_road.items()):
            ang = sum(x["ang"] for x in members) / len(members)
            hit = None
            if MERGE_ROADS_BY_AXIS:
                for ax in axes:
                    if axis_diff(ax["ang"], ang) < SAME_AXIS_TOL:
                        hit = ax
                        break
            if hit is None:
                axes.append({"ang": ang, "segs": list(members)})
            else:
                # 两条路走向接近（切向汇入）→ 合并成一组，它们之间不该互相等灯
                hit["segs"].extend(members)
                hit["ang"] = sum(x["ang"] for x in hit["segs"]) / len(hit["segs"])
        axes.sort(key=lambda ax: -len(ax["segs"]))
        if len(axes) > 2:
            multi_axis.append((iname, axes))

        # ---- 定每个轴向组的目标编号：**尽量沿用现有值** ----
        # 第一版直接按段数排名赋 0/1/2，结果 246 段里 151 段"要改"，
        # 而其中绝大多数只是把本来就正确的 (1,0) 对调成 (0,1)——
        # 行为毫无变化，却弄脏 151 个 actor，还把真正的 10 处修复淹没在噪声里。
        # 现在：每组先要它成员里最常见的那个现有编号，有冲突才让小组另选。
        for ax in axes:
            tally = {}
            for s in ax["segs"]:
                pn = prop_cache.get(id(s["a"]))
                if pn is None:
                    pn = light_prop_name(s["a"])
                    prop_cache[id(s["a"])] = pn
                try:
                    cur = int(s["a"].get_editor_property(pn)) if pn else None
                except Exception:
                    cur = None
                if cur is not None:
                    tally[cur] = tally.get(cur, 0) + 1
            # 票数相同时取较小的编号，保证结果稳定、重复运行不来回翻
            ax["want"] = (sorted(tally.items(), key=lambda kv: (-kv[1], kv[0]))[0][0]
                          if tally else None)
        taken = set()
        for ax in axes:                      # 段数多的先占，它改动代价最大
            if ax["want"] is not None and ax["want"] not in taken:
                ax["gid"] = ax["want"]
                taken.add(ax["gid"])
            else:
                ax["gid"] = None
        nxt = 0
        for ax in axes:
            if ax["gid"] is None:
                while nxt in taken:
                    nxt += 1
                ax["gid"] = nxt
                taken.add(nxt)

        rows = []
        for ax in axes:
            gi = ax["gid"]
            for s in ax["segs"]:
                pn = prop_cache.get(id(s["a"]))
                if pn is None:
                    pn = light_prop_name(s["a"])
                    prop_cache[id(s["a"])] = pn
                if pn is None:
                    failed += 1
                    rows.append((s, None, gi, u"读不到 LightNumber 属性"))
                    continue
                try:
                    cur = int(s["a"].get_editor_property(pn))
                except Exception:
                    cur = None
                if cur == gi:
                    same += 1
                    continue
                if not DRY_RUN:
                    try:
                        s["a"].modify(True)
                        s["a"].set_editor_property(pn, gi)
                        back = int(s["a"].get_editor_property(pn))
                        if back != gi:
                            failed += 1
                            rows.append((s, cur, gi, u"写了但回读是 %d" % back))
                            continue
                    except Exception as exc:
                        failed += 1
                        rows.append((s, cur, gi, str(exc)[:50]))
                        continue
                changed += 1
                rows.append((s, cur, gi, u""))
        if rows:
            w(u"── %s   轴向 %s" % (iname, u" / ".join(
                u"%.0f°×%d" % (ax["ang"], len(ax["segs"])) for ax in axes)))
            for s, cur, gi, note in rows:
                w(u"     %-36s %.0f°  组 %s → %d  %s"
                  % (s["label"][:36], s["ang"],
                     u"?" if cur is None else str(cur), gi, note))

    w(u"")
    w(u"=" * 76)
    w(u"要改 %d 段，本来就对 %d 段，失败 %d 段" % (changed, same, failed))
    if skipped_names:
        w(u"按 SKIP_INTERSECTIONS 跳过 %d 个路口（手动维护）：%s"
          % (len(skipped_names), u"、".join(skipped_names)))
    if multi_axis:
        w(u"")
        w(u"**方向多于两个的路口 %d 个**，按轴向数分成了 3 组以上：" % len(multi_axis))
        for iname, axes in multi_axis[:10]:
            w(u"  %-20s %s" % (iname, u" / ".join(
                u"%.0f°×%d→组%s" % (ax["ang"], len(ax["segs"]), ax.get("gid"))
                for ax in axes)))
        w(u"  蓝图支持 N 组（Length 就是组数，游标会轮 N 轮），所以能跑；")
        w(u"  代价是每个方向等的时间变成 (N-1)×LightDuration。要不要这样是设计决定，")
        w(u"  想强行压成两组就把第 3 组及以后并进组 1（说一声我加个开关）。")
    w(u"")
    if DRY_RUN:
        w(u"只报不改。确认上面的「组 X → Y」都合理之后，把 DRY_RUN 改 False 再跑。")
    else:
        w(u"**记得 Ctrl+S 保存关卡。**")
        w(u"复核：再跑 py audit_light_number_axes.py，应该是「有问题 0 个」。")
    w(u"注意：重新跑 gen_traffic_lanes.py 会生成全新 actor、这些值会丢，")
    w(u"      所以这一步要排在 gen_intersections.py 之后、每次重建都补跑。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[lnfix] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[lnfix] 写入 %s" % OUT)
