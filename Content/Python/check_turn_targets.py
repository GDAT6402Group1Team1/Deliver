# -*- coding: utf-8 -*-
"""全图审计：每条转弯样条的终点到底有没有**车能开进去**的下家。

起因：Inter_形状13_-0540_I03 和 Inter_形状13_+0180_I03 的左右转本该写成
直行副本，却留着真曲线。查下来两条都是拐进 Inter_形状44_+0180_I00，
而终点附近最近的东西是 Inter_形状44_-0180_I00——形状44 的**反向车道**，
距离才 360cm。fix_t_junctions 的②只看"附近有没有下家"、不看方向，
就把对向车道当成合法下家放过去了。

所以这里判"有下家"要同时满足两条：
    位置  —— 下家的**起点**落在转弯终点 LINK_MAX 内
    方向  —— 下家的出发方向和转弯的**出口方向**同向（dot > DOT_SAME_DIR）
对向车道的方向正好相反（dot ≈ -1），一票否决。

分类：
    OK       终点有同向下家，真能开过去
    反向     附近只有反向的段（就是这次的 bug）
    无下家   附近什么都没有
    直行副本 这条转弯已经被写成直行了，正常
    残桩     蓝图默认的 2 点 100cm 小棍子 —— 说明 fill_turn_splines 没生效

只读，什么都不改。
用法：py check_turn_targets.py
"""

import traceback

import unreal

LANE_TAG_PREFIX = "ClaudeGenLane"
LEFT_COMP, RIGHT_COMP, MAIN_COMP = "SplineLeft", "SplineRight", "Spline"

LINK_MAX = 600.0         # 端点多近算"接得上"，和 fix_t_junctions 保持一致
DOT_SAME_DIR = 0.5       # 同向判据。0.5 = 夹角 60 度以内，路口出口有点斜也能过
TURN_DIFF_MIN = 50.0     # 和直行差多少才算"是一条真转弯"
STUB_MAX_POINTS = 3
STUB_MAX_LEN = 300.0
DIR_SAMPLE = 100.0       # 求方向时两个采样点的间距
MAX_ROWS = 60

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "turn_targets.txt"
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


def pts_of(sp):
    return [sp.get_location_at_spline_point(i, WS)
            for i in range(sp.get_number_of_spline_points())]


def dist(p, q):
    return ((p.x - q.x) ** 2 + (p.y - q.y) ** 2 + (p.z - q.z) ** 2) ** 0.5


def norm2d(dx, dy):
    h = (dx * dx + dy * dy) ** 0.5
    # 取不到方向就返回 None。兜底成 (1,0) 是有害的——它把"没方向"伪装成
    # "朝 +X"，会让同向判断得出和真实情况无关的结论。
    return (dx / h, dy / h) if h > 1e-4 else None


def dir_at(sp, at_start):
    """样条首端的出发方向 / 末端的离开方向。

    get_direction_at_distance_along_spline 在这些样条上返回零向量，
    所以一律用两个采样点作差。
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
    try:
        return (sp.get_number_of_spline_points() <= STUB_MAX_POINTS
                and sp.get_spline_length() < STUB_MAX_LEN)
    except Exception:
        return False


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    anchors = []       # (名字, 起点, 出发方向) —— 车可以接上去的下一段
    inters = []
    for a in eas.get_all_level_actors():
        try:
            if not unreal.SystemLibrary.is_valid(a) or a.is_actor_being_destroyed():
                continue
        except Exception:
            pass
        if not any(str(t).startswith(LANE_TAG_PREFIX) for t in a.tags):
            continue
        lbl = a.get_actor_label()
        if not (lbl.startswith("Lane_") or lbl.startswith("Inter_")):
            continue
        sp = comp_by_name(a, MAIN_COMP) or a.get_component_by_class(
            unreal.SplineComponent)
        if sp is None or sp.get_number_of_spline_points() < 2:
            continue
        p = pts_of(sp)
        d = dir_at(sp, True)
        if d is not None:
            anchors.append((lbl, p[0], d))
        if lbl.startswith("Inter_"):
            inters.append((lbl, a, sp, p))

    inters.sort(key=lambda t: t[0])
    w("路口段 %d 条，可接续锚点 %d 个（车道段 + 路口段的起点）"
      % (len(inters), len(anchors)))
    w("判据：下家起点落在转弯终点 %.0fcm 内，**且**出发方向与转弯出口同向"
      % LINK_MAX)
    w("      同向 = dot > %.2f（夹角 60 度以内）" % DOT_SAME_DIR)
    w("")
    flush()

    ok = copy = 0
    bad_rev, bad_none, stubs = [], [], []
    for lbl, a, sp, base in inters:
        for cname in (LEFT_COMP, RIGHT_COMP):
            c = comp_by_name(a, cname)
            if c is None or c.get_number_of_spline_points() < 2:
                continue
            if is_stub(c):
                stubs.append("%s %s" % (lbl, cname))
                continue
            cp = pts_of(c)
            if max(dist(cp[0], base[0]),
                   dist(cp[-1], base[-1])) < TURN_DIFF_MIN:
                copy += 1
                continue
            out = dir_at(c, False)
            near = []
            for nm, q, nd in anchors:
                if nm == lbl:
                    continue
                dd = dist(q, cp[-1])
                if dd < LINK_MAX:
                    dot = (out[0] * nd[0] + out[1] * nd[1]) if out else -9.0
                    near.append((dd, nm, dot))
            near.sort()
            good = [t for t in near if t[2] > DOT_SAME_DIR]
            if good:
                ok += 1
            elif near:
                dd, nm, dot = near[0]
                bad_rev.append((lbl, cname, nm, dd, dot))
            else:
                bad_none.append((lbl, cname))

    w("=" * 96)
    w("① 终点附近只有**反向**的段 —— 车拐过去是逆行，这条转弯该写成直行副本")
    w("=" * 96)
    if bad_rev:
        w("%-32s %-12s %-32s %7s %7s"
          % ("路口段", "转弯", "最近的段", "距离", "同向度"))
        w("-" * 96)
        for lbl, cname, nm, dd, dot in sorted(bad_rev)[:MAX_ROWS]:
            w("%-32s %-12s %-32s %7.0f %+7.2f"
              % (lbl[:32], cname, nm[:32], dd, dot))
        if len(bad_rev) > MAX_ROWS:
            w("... 还有 %d 条" % (len(bad_rev) - MAX_ROWS))
    else:
        w("  没有。")

    w("")
    w("=" * 96)
    w("② 终点附近什么都没有 —— 拐过去直接掉进虚空")
    w("=" * 96)
    if bad_none:
        for lbl, cname in sorted(bad_none)[:MAX_ROWS]:
            w("  %-32s %s" % (lbl[:32], cname))
        if len(bad_none) > MAX_ROWS:
            w("  ... 还有 %d 条" % (len(bad_none) - MAX_ROWS))
    else:
        w("  没有。")

    w("")
    w("=" * 96)
    w("合计")
    w("=" * 96)
    w("真转弯且下家同向（正常）     %d 条" % ok)
    w("只有反向下家（该改直行）     %d 条" % len(bad_rev))
    w("没有下家（该改直行）         %d 条" % len(bad_none))
    w("已经是直行副本               %d 条" % copy)
    w("蓝图默认残桩                 %d 条" % len(stubs))
    if stubs:
        w("")
        w("!! 有 %d 条残桩，说明 fill_turn_splines.py 那步没生效。" % len(stubs))
        for s in stubs[:8]:
            w("   %s" % s)
    w("")
    w("要修的一共 %d 条。fix_t_junctions.py 里的②已经按同样的判据加了方向校验，"
      % (len(bad_rev) + len(bad_none)))
    w("跑它就会把这些写成直行副本。")
    w("只读脚本，没有改动任何东西。")
    flush()
    unreal.log("[tgt] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[tgt] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
