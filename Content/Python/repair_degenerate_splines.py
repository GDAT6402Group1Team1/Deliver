# -*- coding: utf-8 -*-
"""把路口段里**只剩一个点的**样条写成另一条有效样条的副本。只做这一件事。

背景（用户 2026-10-29 的两种手改）
----------------------------------
在不想让车转弯/直行的地方，用户的做法是「先重置成默认、再删掉除首点外的所有点」，
于是那条样条只剩 **1 个点**。两种都有：
  · 删掉左右转、保留直行
  · 删掉直行、只保留一个转向

**但删掉并不能表达"这里不许走这条"。** 车探到一个 Box 就拿到三条候选
（主样条 + SplineLeft + SplineRight），**随机选一条**，它不知道哪条被删过。
选到单点样条的后果比"短"严重得多：长度 0 → 切线为零 →
`GetRotationAtDistanceAlongSpline` 内部 `MakeFromXZ(切线, Up)` 退化 →
**前向掉回世界 +X**，车朝正东飞出去，和路的走向毫无关系。
（同一条机制 CLAUDE.md 记过：车的探测球莫名甩向正右方那次。）

正确的表达方式是**写成想让它走的那条的副本**：选中哪条都走同一条路径，
蓝图不需要任何特判。这也是项目既有惯例（"用不着的那条写成和直行一模一样"）。

和 `fill_turn_splines.py` 的区别——**这是本脚本存在的全部理由**：
那个脚本 `COLLAPSE_ALL_FIRST` 会先把全图转弯压平、再**在能画的地方重新画出转弯曲线**，
把"这里不要转弯"的删除全部复原；`fix_t_junctions.py` 第 3 步还会给 T 形进入段**加**右转。
本脚本**只复制、不画曲线、不删 actor**。

供体（donor）怎么选，顺序有讲究：
  1. 有效（≥2 点、长度 ≥ MIN_LEN）**且终点有同向下家**的，优先主样条，再左、再右
  2. 一条都没有下家时**不写**——那样只是把"飞出去的位置"挪个地方，治不了，
     而且会把用户故意留的唯一方向也覆盖掉。报出来让人去看
主样条退化而某条转弯有效时，donor 就是那条转弯——于是三条都在转，
这才真正表达了"这里必须转弯"。

DRY_RUN=True 只报不改（默认）。
用法：py repair_degenerate_splines.py
"""

import math
import traceback

import unreal

DRY_RUN = False

# 判据（用户 2026-10-29 定）：**只剩 1 个点**，或者**还是蓝图默认的那根残桩**。
# 两种都是"这条路径不可用"的确定形态，不会是有意为之：
#   · 1 个点 —— 「重置成默认 + 删掉除首点外所有点」留下的，长度 0、切线为零
#   · 蓝图默认 —— BP_TrafficLine1_IntersectionChild 自带的 2 点 / 约 100cm 小棍子，
#     方向一律朝世界 +X，和路的走向无关
# 刻意用 150 而不是原来的「≤3 点且 <300cm」：后者会把真实存在的短转弯
# （比如 280cm 的紧弯）也算进来，而那可能是用户要留的。
DEFAULT_LEN_MAX = 150.0  # 2 点且不长于此 = 蓝图默认残桩
LINK_DIST = 600.0        # 下家判定距离，和 fix_t_junctions.py 一致
LINK_DOT = 0.5           # 同向判据：只看位置会把对向车道当成下家

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "repair_degenerate_splines.txt"
lines = []
_mark_failed = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[repair] %s" % s)


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def mark_edited(sp):
    """实测 spline_has_been_edited 可用、b_spline_has_been_edited 不存在。"""
    for n in ("spline_has_been_edited", "b_spline_has_been_edited"):
        try:
            sp.set_editor_property(n, True)
            return True
        except Exception:
            continue
    return False


def dedup_pts(pts, eps=2.0):
    """相邻重合点去重。两点重合时自动切线必然是零，点型改成 CURVE 也救不回来。"""
    out = []
    for p in pts:
        if out:
            q = out[-1]
            if ((p.x - q.x) ** 2 + (p.y - q.y) ** 2 + (p.z - q.z) ** 2) ** 0.5 < eps:
                continue
        out.append(p)
    return out


def write_spline(sp, pts):
    """照抄 fill_turn_splines.py 的写入顺序，两条铁律都在注释里。"""
    # mark_edited 必须在写点**之前**：它是 set_editor_property，而在组件上改属性
    # 会触发这个 actor 重跑构造脚本、组件整批重建——放最后调的话刚写进去的点
    # 正好落在被丢弃的那批上（名字变成 TRASH_SplineComponent_xxxx，回读全不对）。
    marked = mark_edited(sp)
    # modify() 也必须在改动之前：clear/add/update_spline 都只改内存、不标记已修改，
    # 保存时整个对象不被序列化（实测 8 条只活 1 条）。
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
    n = sp.get_number_of_spline_points()
    for i in range(n):
        sp.set_spline_point_type(i, unreal.SplinePointType.CURVE, False)
    sp.update_spline()
    if not marked:
        _mark_failed.append(sp.get_name())
    return n


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


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    w(u"=== 退化样条修复（写成有效那条的副本）===")
    w(u"关卡：%s     模式：%s"
      % (world.get_name() if world else u"?", u"只报不改" if DRY_RUN else u"**写入**"))

    inter_actors, starts = [], []
    for a in eas.get_all_level_actors():
        lbl = a.get_actor_label()
        if not (lbl.startswith("Lane_") or lbl.startswith("Inter_")):
            continue
        comps = {c.get_name(): c for c in a.get_components_by_class(unreal.SplineComponent)}
        if not comps:
            continue
        if lbl.startswith("Inter_"):
            inter_actors.append((a, lbl, comps))
        for c in comps.values():
            if c.get_number_of_spline_points() >= 2:
                starts.append((c.get_location_at_spline_point(0, WS), dir_at(c, True)))
    w(u"路口段 %d 个；全图可作下家的样条起点 %d 个" % (len(inter_actors), len(starts)))

    def has_downstream(sp):
        e_pt = sp.get_location_at_spline_point(
            sp.get_number_of_spline_points() - 1, WS)
        dout = dir_at(sp, False)
        if dout is None:
            return False
        for s_pt, sdir in starts:
            if d2(s_pt, e_pt) >= LINK_DIST or sdir is None:
                continue
            if (sdir[0] * dout[0] + sdir[1] * dout[1]) > LINK_DOT:
                return True
        return False

    def classify(c):
        n = c.get_number_of_spline_points()
        L = c.get_spline_length()
        if n <= 1 or L < 1.0:
            return u"**只剩 %d 点 / %.0f cm**" % (n, L), False
        if n <= 2 and L <= DEFAULT_LEN_MAX:
            return u"**蓝图默认残桩（%d 点 / %.0f cm）**" % (n, L), False
        return u"正常（%d 点 / %.0f cm）" % (n, L), True

    fixed, skipped, nodonor = 0, 0, []
    w(u"")
    for a, lbl, comps in sorted(inter_actors, key=lambda t: t[1]):
        order = []
        for key in ("Spline", "SplineComponent"):
            if key in comps:
                order.append(key)
        for nm in sorted(comps):
            if nm not in order:
                order.append(nm)

        info, bad = {}, []
        for nm in order:
            desc, ok = classify(comps[nm])
            down = has_downstream(comps[nm]) if ok else False
            info[nm] = (desc, ok, down)
            if not ok:
                bad.append(nm)
        if not bad:
            skipped += 1
            continue

        donor = None
        for nm in order:                     # 主样条优先，再左、再右
            if info[nm][1] and info[nm][2]:
                donor = nm
                break
        loc = a.get_actor_location()
        w(u"── (%8.0f,%9.0f) %s" % (loc.x, loc.y, lbl))
        for nm in order:
            desc, ok, down = info[nm]
            w(u"      %-16s %-26s %s" % (nm, desc,
              (u"有同向下家" if down else u"**没有下家**") if ok else u""))
        if donor is None:
            nodonor.append(u"(%8.0f,%9.0f) %s" % (loc.x, loc.y, lbl))
            w(u"      → **没有可用供体**（有效的那条也没有下家），不写。"
              u"这个路口段本身是死的，得去看它为什么没下家。")
            continue
        w(u"      → 供体 %s；把 %s 写成它的副本"
          % (donor, u"、".join(bad)))
        if DRY_RUN:
            continue
        src = pts_of(comps[donor])
        for nm in bad:
            got = write_spline(comps[nm], src)
            back = comps[nm].get_spline_length()
            if got < 2 or back < 1.0:
                w(u"      ！%s 写完回读还是 %d 点 / %.0f cm" % (nm, got, back))
            else:
                fixed += 1

    w(u"")
    w(u"=" * 74)
    if DRY_RUN:
        w(u"只报不改。上面每条都列了三条样条的状态和选中的供体，")
        w(u"确认供体选得对（尤其「主样条退化、用转弯当供体」那些）再把 DRY_RUN 改 False。")
    else:
        w(u"写好 %d 条样条。**记得 Ctrl+S 保存关卡。**" % fixed)
        if _mark_failed:
            w(u"！%d 条没打上已编辑标记，它们的点可能被构造脚本冲回蓝图默认值：%s"
              % (len(_mark_failed), u"、".join(_mark_failed[:6])))
            w(u"  现象是「写进去了又没了」，重启编辑器后用体检脚本复核。")
    w(u"三条样条都正常、没动的路口段：%d 个" % skipped)
    if nodonor:
        w(u"")
        w(u"**没有可用供体的 %d 处**（三条里有效的那条也没下家，本脚本治不了）：" % len(nodonor))
        for x in nodonor:
            w(u"    %s" % x)
        w(u"  这些要么是路到头了（正常），要么是下家被删掉了——去坐标处看一眼。")


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[repair] fail:" + chr(10) + err)
    lines.append(u"")
    lines.append(u"！中途异常：")
    lines.append(err)
finally:
    flush()
    unreal.log("[repair] 写入 %s" % OUT)
