# -*- coding: utf-8 -*-
"""把左右转曲线写进路口段 actor 自带的 SplineLeft / SplineRight。

和之前那版独立 Turn actor 的区别：转弯路径就挂在原来那个路口段 actor 上，
和直行的 Spline 共用同一个 Box。车探测到一个 Box 就拿到三条候选路径。

分工按"外道左拐、内道右拐"：
    外道（同向车道里 |偏移| 最大的）-> 只填 SplineLeft
    内道（|偏移| 最小的）          -> 只填 SplineRight
    次路同向只有一条车道时内外道是同一条，两条都填
用不上的那条会被写成**和直行一模一样**的样条。车的选路是在候选里随机选，
选中它就等于直行，无害，蓝图那边不需要任何特判。
（早先是压成零长，但那是个需要人人都记得处理的特殊状态；
  更早是什么都不做，会留下蓝图默认的 2 点 100cm 残桩，车抓到它就开出去了。）

起点 = 本段直行样条的第一个点，终点 = 去向路口段直行样条的最后一个点，
中间三次贝塞尔、两端相切。高度在首尾之间线性过渡，不在路口里打射线——
路口内部两条路的路面叠着（实测差约 200cm），取到哪层看运气。

左右用叉乘判，不看偏移正负号：每条道路样条的走向是作者随手画的，
形状39 的"正方向"和形状13 没有固定关系。
UE 里 Cross(Forward, Right) = +Z，所以 cross(进入方向, 驶出方向).z > 0 是右转。

用法：py fill_turn_splines.py
"""

import math
import re
import traceback

import unreal

# 中间点用 CURVE 还是 CURVE_CLAMPED（首尾一律 CURVE，见下面那段注释）。
# **True（默认）**：CURVE 的自动切线指向邻点、非零，曲线连续。
# 为什么要改：CURVE_CLAMPED 的定义行为是**把端点和局部极值处的切线归零**，
# 而车道的 Z 是逐点打射线测的，路面每个小起伏都制造一个局部极大/极小，
# 于是那些点的切线被钳成 0，曲线在那里出现"平台 + 折角"——
# 肉眼看就是**一条折线而不是曲线**（用户 2026-10-29 报的就是这个）。
# 代价：CURVE 不像 Clamped 那样保证不过冲，点间距不均处可能向外鼓一点。
# 控制点一个都不动，所以**首尾位置和接缝连接完全不受影响**。
# 置 False 回到原来的防过冲行为。
MIDDLE_POINTS_CURVE = True

LANE_TAG_PREFIX = "ClaudeGenLane"
ONLY_INTERSECTION = ""   # 只处理这个路口（填 actor 名，如 "Intersection_04"）；"" = 全部
COLLAPSE_ALL_FIRST = True
# 开工前先把**全图**所有路口段的 SplineLeft/SplineRight 写成直行副本。
# 不这么做的话，没被处理到的路口上这两条组件一直停在蓝图默认值——
# 2 个点、长 100、方向一律朝 +X，和路的走向无关。152 个路口段就是
# 300 根散在全图的小棍子（视口里看着一团乱），而且车有可能抓到一根开出去。
# 只处理单个路口做试验时，这一步尤其必要。
CLUSTER_DIST = 2500.0
# BEZIER_K 已弃用：那种「切线各伸出 k 倍距离」的画法要猜 k，
# 猜大了曲线会在反面鼓出个包变成 S 形（实测就是这么坏的）。
# 现在用两条切线的交点当控制点，不需要任何可调参数。
SAMPLES = 20             # 每条转弯线采样几个点。14 -> 20：进口收紧之后前段曲率很大，
                         # 点太稀的话 CURVE_CLAMPED 会把这段插值得偏圆，
                         # 实际形状和算出来的贝塞尔对不上。
TURN_RADIUS = 400.0      # 圆弧段的半径（cm）。
                         # 半径和直线段长度是同一个旋钮的两端：
                         #   T = R*tan(|偏转角|/2)  是切点到角点的距离
                         #   半径大 -> 切点离角点远 -> 首尾的直线段短（弯更缓）
                         #   半径小 -> 弯更急，但首尾留下更长的直线段
                         # 端点位置是定死的，这两个没法同时要。
                         # 260 时弯的形状合适但直线段偏长，提到 400。
                         # 这是现在的主模型：直线进 -> 定半径圆弧 -> 直线出，
                         # 和真实车辆转弯一致。半径是个能直接判断的量，
                         # 比调贝塞尔手柄的比例直观得多。
                         # 太小的话（相对路口尺寸）会被自动夹住，报告里会写明。
USE_ARC = True           # False 则退回下面的贝塞尔手柄模型
USE_BIARC = False          # **按用户决定关掉：全部回到单圆弧。**
                           # 关掉 = 只用单圆弧，半径被夹到多小都照画。
                           # 留这个开关是为了能直接对比：单圆弧出口前有一整段直线、
                           # 车提前对正，手感确实最好，所以它仍然是主路径
                           # （实测 328 条里 260 条走它）。
                           # 但它**没法同时精确满足两头的方向**——圆弧只有 3 个自由度
                           # （圆心 + 半径），而约束有 4 个（两端各一个位置 + 一个方向）。
                           # 关掉之后那 23 条半径被夹到 2~18cm 的会保持尖角 V，
                           # 车的探测球跟不住，表现是过路口冲出车道或接不上下一条线。
MIN_USABLE_RADIUS = 150.0  # 单圆弧半径被夹到比这还小就改走双圆弧（biarc）。
                           # 为什么要这条：单圆弧**两侧共用同一个切线长**
                           # tl = R*tan(|δ|/2)，而 tl 受
                           # lim = min(|角点-p0|, |p3-角点|) 限制——
                           # 角点一旦贴近某个端点，lim 就塌到接近 0，
                           # 半径被夹到 2~18cm，等于没有圆弧、就是个尖角 V，车跟不住。
                           # 实测全图 328 条转弯里 68 条被夹小（21%），
                           # 其中 23 条半径 <150，**而且最惨的几条是 88~97° 的
                           # 普通直角转弯**（跨度还有 935~1444cm，空间明明够），
                           # 所以这不是「大角度才有的问题」，是单圆弧构造本身
                           # 对某些端点/切线组合没有解。
MAX_TURN_DEG = 135.0       # |偏转角| 超过这个就不当转弯，写成直行副本。
                           # 原来只在距 180° 不到 2.9°（0.05 rad）时才拒绝，
                           # 于是 165° 这种也会被当转弯画出来。
                           # 为什么是 135 而不是 160：**不是算法不行，是这种转弯
                           # 现实里不存在**。离线实测 134° 的 biarc 最小半径只有
                           # 68cm、153° 只有 33cm（虽然比单圆弧那个 8 / 4 好得多），
                           # 对车来说仍然过紧；而 134° 本身已经是急回头弯了。
                           # 按实测分布，这条门槛会把 120~150° 的 18 条、
                           # 150~170° 的 7 条、170~181° 的 9 条共约 30 条
                           # 改成直行副本——车只能直行通过那个方向，
                           # 而不是硬画一条跟不住的急弯。
                           # 代价是路口可选路径变少，这是取舍、不是 bug。

ENTRY_HANDLE = 0.12      # 仅 USE_ARC=False 时用。进口控制点落在 p0->角点 连线上的比例
EXIT_HANDLE = 1.00       # 出口控制点落在 p3->角点 连线上的比例（上限就是 1.0：
                         # 到 1.0 时控制点正好落在角点上，再大就越过角点、
                         # 曲线会朝反面鼓出来变成 S 形）
                         # 两个都取 2/3 = 以角点为控制点的二次贝塞尔（转弯均匀铺开）。
                         # 现在进口小、出口大：转弯集中在前半段，出口前留一段近似直线，
                         # 车到终点时早就对正了，才扫得到下一条交通线。
                         # 调整轨迹：0.45/0.95 -> 0.25/1.00 -> 0.12/1.00。
                         # EXIT 到顶了（1.0 时控制点正好在角点上，再大就越过角点、
                         # 曲线朝反面鼓成 S 形），所以只能继续收 ENTRY。
                         # ENTRY 越小，转弯越集中在刚进路口那一小段，
                         # 代价是进口处曲率很大、接近折角，车可能跟不住。
TIGHT_K = 0.35           # 角点退化时三次贝塞尔的控制点伸出比例
DIR_SAMPLE = 100.0       # 求首尾朝向时，沿样条取多长的一段作差
MIN_TURN_LEN = 100.0     # 首尾太近的不生成。200 太严：紧角的转弯首尾只差 187cm
                         # （实测 形状16-0270 -> 形状13-0675），那是条合法的短弯

LEFT_COMP, RIGHT_COMP, MAIN_COMP = "SplineLeft", "SplineRight", "Spline"

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "fill_turn_splines.txt"
lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[turnfill] %s" % s)


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


def mark_edited(sp):
    """打 "Override Construction Script" 标记，否则构造脚本重跑会冲掉点。

    实测 spline_has_been_edited 可用、b_spline_has_been_edited 不存在，
    保留后者只是兼容其它版本。
    """
    for n in ("spline_has_been_edited", "b_spline_has_been_edited"):
        try:
            sp.set_editor_property(n, True)
            return True
        except Exception:
            continue
    return False


_mark_failed = []


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
    # modify() 必须在改动之前调：clear/add/update_spline 都是普通函数调用，
    # 只改内存状态、不会把对象标记为已修改，保存时就不被序列化——
    # 实测 8 条转弯只有 1 条存活，而那 1 条恰好是唯一被 set_editor_property
    # 正规写过属性、因而被标脏的 actor。
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
        sp.set_spline_point_type(
            i,
            unreal.SplinePointType.CURVE if MIDDLE_POINTS_CURVE
            else unreal.SplinePointType.CURVE_CLAMPED,
            False)
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
    # 标记已经在最前面打过了，这里只记失败。没打上的话点会被构造脚本
    # 冲回蓝图默认值，而现象是"写进去了又没了"，光看"写入 N 条"看不出来。
    if not marked:
        _mark_failed.append(sp.get_name())


def mirror_straight(a, sp, main):
    """把用不着的转弯样条写成**和直行完全一样**的一条。

    原来是压成零长。改成复制直行的好处：车的选路逻辑是在候选里随机选，
    选中这条就是直行，无害——蓝图那边连 GetSplineLength() < 1 这种
    特判都不用加。零长是个需要人人都记得处理的特殊状态，直行副本不是。
    """
    n = main.get_number_of_spline_points()
    if n < 2:
        o = a.get_actor_location()
        write_spline(sp, [o, o])
        return
    write_spline(sp, [main.get_location_at_spline_point(i, WS)
                      for i in range(n)])


# 从**右往左**锚定：末尾是 I<编号>、倒数第二段是 ±四位偏移，中间全是路名。
# 原来是 lbl.split("_") 按位置取 parts[1]/parts[2]，而那只在「路名里没有下划线」
# 时成立——旧路名是 形状13，新路名是 BranchRoad_R011，字段整个错位，
# int("R011") 抛 ValueError，于是 192 个路口段全被判成「名字对不上格式」
# 跳过，报告写「路口段 0 个 / 没有可处理的路口段」，一条转弯都没生成。
INTER_LABEL_RE = re.compile(r"^Inter_(.+)_([+-]\d+)_(I\d+)$")


def parse_label(lbl):
    m = INTER_LABEL_RE.match(str(lbl))
    if not m:
        return None
    try:
        return m.group(1), int(m.group(2)), m.group(3)
    except ValueError:
        return None


def norm2d(v):
    h = (v.x * v.x + v.y * v.y) ** 0.5
    if h < 1e-6:
        return None          # 不要兜底成 (1,0,0)：那会把"取不到方向"
                             # 伪装成"方向朝 +X"，后面所有判断都建立在假数据上
    return unreal.Vector(v.x / h, v.y / h, 0.0)


def dir_from_points(sp, d_a, d_b):
    """用样条上两个采样点作差求方向。

    不用 get_direction_at_distance_along_spline——实测它在这些样条上
    返回零向量（摊开的几何里所有朝向都打成了 +0.0°）。
    而 get_location_at_distance_along_spline 的结果是对的（首尾坐标都正常），
    所以从坐标反推方向是可靠的那条路。
    """
    a = sp.get_location_at_distance_along_spline(d_a, WS)
    b = sp.get_location_at_distance_along_spline(d_b, WS)
    return norm2d(unreal.Vector(b.x - a.x, b.y - a.y, 0.0))


def corner_point(p0, d0, p3, d3):
    """两条切线的交点：从 p0 沿进入方向的射线，和从 p3 沿驶出方向倒推的射线。

    这就是这个弯的"角点"。用它当控制点画二次贝塞尔，曲线天然与两端相切，
    而且**只会朝角点鼓**——不可能像"切线各伸出 k 长度"那种画法一样，
    k 猜大了就在反面鼓出个包、整条线变成 S 形。

    解 p0 + t*d0 + s*d3 = p3：两边同时叉乘 d3 得 t，叉乘 d0 得 s。
    t<=0 说明交点在来向后方、s<=0 说明在去向前方——两种都意味着
    切线方向取错了，返回 None 让调用方报出来，而不是硬画一条怪线。
    """
    den = d0.x * d3.y - d0.y * d3.x
    if abs(den) < 1e-6:
        return None, "两端切线平行"
    ex, ey = p3.x - p0.x, p3.y - p0.y
    t = (ex * d3.y - ey * d3.x) / den
    sgn = (d0.x * ey - d0.y * ex) / den
    if t <= 0.0:
        return None, "交点在来向后方(t=%.0f)" % t
    if sgn <= 0.0:
        return None, "交点在去向前方(s=%.0f)" % sgn
    return unreal.Vector(p0.x + d0.x * t, p0.y + d0.y * t, 0.0), ""


def arc_path(p0, d0, p3, d3, c):
    """直线进 -> 定半径圆弧 -> 直线出。返回点列，几何不成立时返回 None。

    标准的道路缓和做法：偏转角 delta 下，切点到角点的距离 T = R*tan(|delta|/2)。
    切点 A = 角点 - 进入方向*T，B = 角点 + 驶出方向*T，中间用半径 R 的圆弧接上。
    p0->A 和 B->p3 都是直线，所以车在出口前有一整段是正对目标方向的，
    比"整段都在转"的贝塞尔更早对正。
    """
    a_in = math.atan2(d0.y, d0.x)
    a_out = math.atan2(d3.y, d3.x)
    delta = (a_out - a_in + math.pi) % (2 * math.pi) - math.pi   # 偏转角，带符号
    if abs(delta) < 0.05:
        return None, "偏转角 %.0f 度，不适合圆弧" % math.degrees(delta)
    if abs(math.degrees(delta)) > MAX_TURN_DEG:
        # 近掉头，不是转弯（见 MAX_TURN_DEG）
        return None, "偏转角 %.0f 度接近掉头，不生成转弯" % math.degrees(delta)

    # 切点不能越过两端的端点
    lim = min(((c.x - p0.x) ** 2 + (c.y - p0.y) ** 2) ** 0.5,
              ((p3.x - c.x) ** 2 + (p3.y - c.y) ** 2) ** 0.5) * 0.98
    t_half = math.tan(abs(delta) / 2.0)
    r = TURN_RADIUS
    tl = r * t_half
    note = ""
    if tl > lim:
        tl = lim
        r = tl / t_half
        note = "半径被夹到 %.0f" % r
        if r < MIN_USABLE_RADIUS:
            # 夹到这么小已经不是圆弧了，是个尖角 V，车的探测球跟不住。
            # 怎么处理由调用方按 USE_BIARC 决定（走 biarc 还是写成直行副本）。
            return None, "半径只能夹到 %.0f，太尖" % r

    ax, ay = c.x - d0.x * tl, c.y - d0.y * tl
    bx, by = c.x + d3.x * tl, c.y + d3.y * tl
    # 圆心在进入方向的左侧或右侧，取决于偏转方向
    sgn = 1.0 if delta > 0 else -1.0
    ox, oy = ax - d0.y * r * sgn, ay + d0.x * r * sgn
    a0 = math.atan2(ay - oy, ax - ox)

    n_arc = max(6, SAMPLES - 4)
    pts2 = [(p0.x, p0.y), (ax, ay)]
    for i in range(1, n_arc):
        ang = a0 + delta * (float(i) / n_arc)
        pts2.append((ox + r * math.cos(ang), oy + r * math.sin(ang)))
    pts2 += [(bx, by), (p3.x, p3.y)]

    # 高度按累计弧长在首尾之间过渡
    total = 0.0
    segl = [0.0]
    for i in range(1, len(pts2)):
        total += ((pts2[i][0] - pts2[i - 1][0]) ** 2
                  + (pts2[i][1] - pts2[i - 1][1]) ** 2) ** 0.5
        segl.append(total)
    out = []
    for (x, y), d in zip(pts2, segl):
        f = (d / total) if total > 1e-6 else 0.0
        out.append(unreal.Vector(x, y, p0.z + (p3.z - p0.z) * f))
    out[0], out[-1] = p0, p3
    return out, note


def _arc_2d(ax, ay, tx, ty, bx, by, n):
    """从 A（切线 T）到 B 的单段圆弧，返回 n 段的 (x, y) 列表（不含起点）。

    圆心必在过 A 且垂直于 T 的那条直线上：O = A + N * (|chord|² / (2 * chord·N))，
    其中 N 是 T 的左法向。chord·N 接近 0 表示 B 就在切线方向上，
    此时半径趋于无穷，直接退化成直线——这是合法情形，不是错误。
    """
    cx, cy = bx - ax, by - ay
    nx, ny = -ty, tx                       # T 的左法向
    h = cx * nx + cy * ny
    chord2 = cx * cx + cy * cy
    if abs(h) < 1e-6 or chord2 < 1e-12:
        return [(ax + cx * (float(i) / n), ay + cy * (float(i) / n))
                for i in range(1, n + 1)]
    rr = chord2 / (2.0 * h)                # 带符号半径：符号决定往哪边弯
    ox, oy = ax + nx * rr, ay + ny * rr
    r = abs(rr)
    a0 = math.atan2(ay - oy, ax - ox)
    a1 = math.atan2(by - oy, bx - ox)
    # 扫过的角要和切线方向一致：rr > 0 时逆时针，< 0 时顺时针
    sweep = a1 - a0
    if rr > 0:
        while sweep < 0:
            sweep += 2 * math.pi
    else:
        while sweep > 0:
            sweep -= 2 * math.pi
    return [(ox + r * math.cos(a0 + sweep * (float(i) / n)),
             oy + r * math.sin(a0 + sweep * (float(i) / n)))
            for i in range(1, n + 1)]


def biarc_path(p0, d0, p3, d3):
    """双圆弧（biarc）：两段圆弧在中间 G1 相接。返回 (点列, 说明)。

    **为什么需要它**：单圆弧（arc_path）两侧共用同一个切线长，是个过约束的
    构造——角点贴近端点时半径只能夹到几厘米，就是个尖角 V。biarc 对
    **任意** (起点+进入方向) → (终点+驶出方向) 都有解，不存在这种退化。

    而且它同时避开了文档里记录的那两次失败：
      * 三次贝塞尔（切线各伸 k 倍）—— k 要猜，猜大了曲线在反面鼓包、整条变 S 形
      * 以角点为控制点的二次贝塞尔 —— 不会 S 形，但整段都在转，
        车到终点还没转到目标朝向、扫不到下一条线
    biarc 两头的切线**精确等于** d0 / d3，所以出口天然对正下一条线；
    而曲率只在中间接点处跳变一次，不会鼓包。

    等切线长解法：令两段的切线长都是 d，接点
        J = ((p0 + d*d0) + (p3 - d*d3)) / 2
    G1 条件化成一元二次方程 a·d² + b·d + c = 0：
        a = 2(d0·d3 - 1)    （≤ 0，两切线平行时为 0）
        b = 2 V·(d0 + d3)
        c = -|V|²           （V = p3 - p0）
    a 接近 0（两切线几乎同向）时退化成线性解 d = |V|² / (2 V·(d0+d3))。
    """
    vx, vy = p3.x - p0.x, p3.y - p0.y
    v2 = vx * vx + vy * vy
    if v2 < 1e-9:
        return None, "首尾重合"

    tx, ty = d0.x + d3.x, d0.y + d3.y
    vt = vx * tx + vy * ty
    # 方程是 (|T|²-4)d² - 2(V·T)d + |V|² = 0。
    # **这两个符号写反过一次**（b 取 +2V·T、c 取 -|V|²），那等于解了另一个方程，
    # 判别式会变成 b²-4ac 里 4ac>0 → 可能为负，而且**恰好在要修的那几条上为负**
    # （离线实测 97°/134°/153° 全部「判别式<0」）。
    # 正确形式下 disc = 4(V·T)² + 8(1-d0·d3)|V|² ≥ 0 **恒成立**，biarc 永远有解。
    a = (tx * tx + ty * ty) - 4.0        # = 2(d0·d3 - 1)，≤ 0
    b = -2.0 * vt
    c = v2

    if abs(a) < 1e-6:
        if abs(b) < 1e-9:
            return None, "两切线同向且首尾不共线，biarc 无解"
        d = -c / b                       # 退化成线性
    else:
        disc = b * b - 4.0 * a * c
        if disc < 0.0:
            return None, "biarc 判别式 <0"
        sq = math.sqrt(disc)
        cands = [(-b + sq) / (2.0 * a), (-b - sq) / (2.0 * a)]
        cands = [x for x in cands if x > 1e-6]
        if not cands:
            return None, "biarc 切线长无正解"
        d = min(cands)      # 取小的那个：弯更紧凑、不会绕到路口外面去

    jx = ((p0.x + d0.x * d) + (p3.x - d3.x * d)) / 2.0
    jy = ((p0.y + d0.y * d) + (p3.y - d3.y * d)) / 2.0

    half = max(4, (SAMPLES - 1) // 2)
    pts2 = [(p0.x, p0.y)]
    pts2 += _arc_2d(p0.x, p0.y, d0.x, d0.y, jx, jy, half)
    # 第二段从 p3 往回画（切线 -d3），再反过来接上，这样两头的切线都精确等于给定值
    back = _arc_2d(p3.x, p3.y, -d3.x, -d3.y, jx, jy, half)
    back.reverse()
    pts2 += back[1:]            # back[0] 是接点 J，上一段末尾已经有了
    pts2.append((p3.x, p3.y))

    # 高度按累计弧长在首尾之间过渡（和 arc_path 同一套）
    total = 0.0
    segl = [0.0]
    for i in range(1, len(pts2)):
        total += ((pts2[i][0] - pts2[i - 1][0]) ** 2
                  + (pts2[i][1] - pts2[i - 1][1]) ** 2) ** 0.5
        segl.append(total)
    out = []
    for (x, y), dd in zip(pts2, segl):
        f = (dd / total) if total > 1e-6 else 0.0
        out.append(unreal.Vector(x, y, p0.z + (p3.z - p0.z) * f))
    out[0], out[-1] = p0, p3
    return out, "biarc 切线长 %.0f" % d


def bezier(src, dst):
    """返回 (点列, 首尾直线距离, 说明)。画不出来时点列为 None。"""
    p0, p3 = src["p_in"], dst["p_out"]
    span = ((p3.x - p0.x) ** 2 + (p3.y - p0.y) ** 2) ** 0.5
    if span < MIN_TURN_LEN:
        return None, span, "首尾过近"

    # **转角门槛放在这里、不放在 arc_path 里。**
    # 放在 arc_path 里拦不住一类情形：两条路在路口反向平行时，
    # 两切线近乎平行 -> corner_point 求不出交点（实测 t = -1.4 亿），
    # arc_path 压根不会被调用，于是直接落到三次贝塞尔画出一个 180° 发卡弯。
    # 实测跑出来过：Inter_MainRoad_R003_+0540_I00 左转 -> BranchRoad_R016_+0180，
    # 转 -180°，而转弯终点 150cm 处就是 Lane_MainRoad_R003_-0180——
    # **开过去等于掉头回到同一条路的反向车道**，真实路口没有这种走法。
    # 注意「目标是另一条路」这个判据拦不住它（目标确实是 BranchRoad_R016），
    # 能拦住的只有转角。
    a_in = math.atan2(src["d_in"].y, src["d_in"].x)
    a_out = math.atan2(dst["d_out"].y, dst["d_out"].x)
    turn = math.degrees((a_out - a_in + math.pi) % (2 * math.pi) - math.pi)
    if abs(turn) > MAX_TURN_DEG:
        return None, span, "转 %.0f° 超过 %.0f°，接近掉头，不生成" % (turn, MAX_TURN_DEG)

    c, why = corner_point(p0, src["d_in"], p3, dst["d_out"])
    arc_note = ""
    if c is not None and USE_ARC:
        pts, note = arc_path(p0, src["d_in"], p3, dst["d_out"], c)
        if pts is not None:
            return pts, span, note
        arc_note = note
        # 近掉头不是「画不出来」而是「不该画」，直接放弃，别退到后面的兜底
        # 又把它画出来（见 MAX_TURN_DEG）。
        if "接近掉头" in note:
            return None, span, note
        # 半径太尖、而且 biarc 关着：**写成直行副本**，不要落到后面的贝塞尔兜底。
        # 贝塞尔会照样画出一条曲线，那就既不是单圆弧、又还是过紧，
        # 两头的好处都占不到。直行副本是现成机制：这个方向没有转弯，
        # 车只能直行通过，干净且不会冲出车道。
        if "太尖" in note and not USE_BIARC:
            return None, span, note + "，写成直行副本"

    # 单圆弧不成立（最常见的是半径只能夹到几厘米）时走双圆弧。
    # biarc 对任意端点/切线都有解，而且两头切线精确等于 d0/d3，出口天然对正。
    pts, note = (None, "USE_BIARC=False")
    if USE_BIARC:
        pts, note = biarc_path(p0, src["d_in"], p3, dst["d_out"])
    if pts is not None:
        return pts, span, (arc_note + " -> " + note) if arc_note else note

    if c is not None:
        # 三次贝塞尔，两个控制点都落在 p0->角点 和 p3->角点 的连线上。
        # 都取 2/3 时等价于以角点为控制点的二次贝塞尔（转弯均匀分布在全程）。
        # 这里刻意取不对称：
        #   进口手柄短 -> 一进路口就开始转
        #   出口手柄长 -> 出口前有一段近似直线，车提前对正
        # 为什么要这样：实测转弯终点、目标 Box、距离都和直行完全一致（149~151cm），
        # 唯一差别是车到达时还没转到位、扫不到下一条线。把转弯提前完成就解决了。
        # 两个控制点都在三角形 p0-角点-p3 内，曲线只朝角点一侧鼓，不会出现 S 形。
        p1 = unreal.Vector(p0.x + (c.x - p0.x) * ENTRY_HANDLE,
                           p0.y + (c.y - p0.y) * ENTRY_HANDLE, 0.0)
        p2 = unreal.Vector(p3.x + (c.x - p3.x) * EXIT_HANDLE,
                           p3.y + (c.y - p3.y) * EXIT_HANDLE, 0.0)
        pts = []
        for i in range(SAMPLES + 1):
            t = float(i) / SAMPLES
            u = 1.0 - t
            pts.append(unreal.Vector(
                u * u * u * p0.x + 3 * u * u * t * p1.x
                + 3 * u * t * t * p2.x + t * t * t * p3.x,
                u * u * u * p0.y + 3 * u * u * t * p1.y
                + 3 * u * t * t * p2.y + t * t * t * p3.y,
                u * p0.z + t * p3.z))
        # 首尾钉死成 p0/p3：多项式在 t=0/1 理论上就等于它们，但浮点会留下残差，
        # 而这两个点必须和直行段的端点**完全一致**，车才接得上下一条线
        pts[0], pts[-1] = p0, p3
        return pts, span, ""

    # 角点退化（首尾几乎重合的紧角，实测 t=-14）。不能就这么丢掉——
    # 那是路口最里侧一个合法的急弯。退回三次贝塞尔，控制点只伸出
    # span 的一小段：这里 span 本来就短，不会像当初 k 猜大时那样鼓成 S 形。
    k = span * TIGHT_K
    p1x, p1y = p0.x + src["d_in"].x * k, p0.y + src["d_in"].y * k
    p2x, p2y = p3.x - dst["d_out"].x * k, p3.y - dst["d_out"].y * k
    pts = []
    for i in range(SAMPLES + 1):
        t = float(i) / SAMPLES
        u = 1.0 - t
        pts.append(unreal.Vector(
            u * u * u * p0.x + 3 * u * u * t * p1x + 3 * u * t * t * p2x + t * t * t * p3.x,
            u * u * u * p0.y + 3 * u * u * t * p1y + 3 * u * t * t * p2y + t * t * t * p3.y,
            u * p0.z + t * p3.z))
    pts[0], pts[-1] = p0, p3
    return pts, span, "紧角(%s)，用三次贝塞尔" % why


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()

    focus = None
    if ONLY_INTERSECTION:
        for a in actors:
            if a.get_actor_label() == ONLY_INTERSECTION:
                focus = a.get_actor_location()
                break
        if focus is None:
            w("!! 找不到 %s，改为处理全部路口" % ONLY_INTERSECTION)
        else:
            w("只处理 %s @ (%.0f, %.0f)" % (ONLY_INTERSECTION, focus.x, focus.y))

    segs = []
    unparsed = []
    no_comp = []
    endpoint_drift = []
    missing = bad_dir = 0
    trashed = 0
    for a in actors:
        # 已销毁但还没被 GC 回收的 actor 仍会出现在 get_all_level_actors() 里。
        # 同一个 Python 进程里先跑 gen_traffic_lanes.py（它删掉全部旧 actor）
        # 再跑这里时，这些"坟墓"和新 actor 标签一模一样，转弯会全部写进
        # 已销毁的组件（组件名带 TRASH_ 前缀），回读全部对不上、存盘什么都没有。
        # 实测 rebuild_traffic.py 一次跑完三步时 126 条全军覆没就是这个。
        try:
            if not unreal.SystemLibrary.is_valid(a) or a.is_actor_being_destroyed():
                trashed += 1
                continue
        except Exception:
            pass
        if not any(str(t).startswith(LANE_TAG_PREFIX) for t in a.tags):
            continue
        lbl = a.get_actor_label()
        info = parse_label(lbl)
        if info is None:
            # 带着车道 tag 却认不出名字——多半是被改过名。
            # 静默跳过的话它就从所有统计里消失了，而少一条车道会连带
            # 改变内外道的判定（实测少了 -0225，南向就没有内道做右转了）。
            if lbl.startswith("Inter"):
                unparsed.append(lbl)
            continue
        main = comp_by_name(a, MAIN_COMP)
        left = comp_by_name(a, LEFT_COMP)
        right = comp_by_name(a, RIGHT_COMP)
        if main is None or main.get_number_of_spline_points() < 2:
            continue
        if left is None and right is None:
            missing += 1
            continue
        # 只缺一条的话照常处理另一条。原来是缺一条就整个 actor 跳过——
        # 蓝图里删掉 SplineRight 之后，左转也跟着一条都生成不了。
        L = main.get_spline_length()
        step = min(DIR_SAMPLE, L * 0.4)
        d_in = dir_from_points(main, 0.0, step)
        d_out = dir_from_points(main, L - step, L)
        if d_in is None or d_out is None:
            bad_dir += 1
            continue

        # 端点取**控制点本身**，不按弧长采样。
        # get_location_at_distance_along_spline(L) 用的是近似弧长，
        # 曲线样条上采到 L 处未必正好落在最后一个控制点上，
        # 差的那一点会让转弯线的终点和直行线的终点对不齐，
        # 车转完弯就接不上下一段。
        npts = main.get_number_of_spline_points()
        p_in = main.get_location_at_spline_point(0, WS)
        p_out = main.get_location_at_spline_point(npts - 1, WS)
        # 顺便量一下两种取法差多少，确认这就是偏差的来源
        alt = main.get_location_at_distance_along_spline(L, WS)
        endpoint_drift.append(((alt - p_out).length(), a.get_actor_label()))

        segs.append({
            "road": info[0], "offset": info[1], "actor": a,
            "label": a.get_actor_label(), "left": left, "right": right,
            "main": main,
            "p_in": p_in, "d_in": d_in,
            "p_out": p_out, "d_out": d_out,
        })
    w("路口段 %d 个" % len(segs))
    if trashed:
        w("跳过已销毁未回收的 actor %d 个（上一步刚删掉的旧车道，GC 还没跑）"
          % trashed)
    if endpoint_drift:
        endpoint_drift.sort(reverse=True)
        worst, wlbl = endpoint_drift[0]
        avg = sum(d for d, _l in endpoint_drift) / len(endpoint_drift)
        w("端点取法差异（按弧长采样 vs 直接取控制点）：平均 %.1f、最大 %.1f cm（%s）"
          % (avg, worst, wlbl[:30]))
        w("  现在一律用控制点，转弯线的终点和直行线的终点完全一致。")
    if COLLAPSE_ALL_FIRST:
        n = 0
        for s_ in segs:
            for c in (s_["left"], s_["right"]):
                if c is not None:
                    mirror_straight(s_["actor"], c, s_["main"])
                    n += 1
        w("先把全图 %d 条转弯样条写成直行副本（清掉蓝图默认的 100cm 残桩）" % n)
    if unparsed:
        w("!! 有 %d 个 actor 带着车道 tag 但名字对不上 Inter_<路>_<偏移>_<编号> 格式，"
          % len(unparsed))
        w("   已跳过——它们不会参与内外道判定，可能导致某个方向少一条转弯：")
        for lbl in unparsed[:10]:
            w("     %s" % lbl)
    if bad_dir:
        w("!! 有 %d 个段算不出方向（样条退化），已跳过" % bad_dir)
    if missing:
        w("!! 有 %d 个 actor 上 %s 和 %s 都找不到，已跳过。"
          % (missing, LEFT_COMP, RIGHT_COMP))
        w("   要么是加组件之前生成的旧实例（重跑 gen_traffic_lanes.py 即可），")
        w("   要么是蓝图里把这两个组件都删了。")
    if not segs:
        w("!! 没有可处理的路口段。")
        flush()
        return

    def mid(s_):
        return unreal.Vector((s_["p_in"].x + s_["p_out"].x) / 2.0,
                             (s_["p_in"].y + s_["p_out"].y) / 2.0,
                             (s_["p_in"].z + s_["p_out"].z) / 2.0)

    if focus is not None:
        # 指定了路口就直接按到它的距离圈一遍，不做贪心聚类。
        # 贪心 + 中心不更新会把同一个物理路口劈成两簇：实测 Intersection_04
        # 少了 Inter_形状13_-0225_I04，四车道主路只进来 3 条，
        # 南向就没有内道可以做右转了。
        clusters = [{"center": focus,
                     "segs": [s for s in segs
                              if (mid(s) - focus).length() < CLUSTER_DIST]}]
    else:
        clusters = []
        for s in segs:
            c = mid(s)
            got = None
            for cl in clusters:
                if (cl["center"] - c).length() < CLUSTER_DIST:
                    got = cl
                    break
            if got is None:
                clusters.append({"center": c, "segs": [s]})
            else:
                got["segs"].append(s)
                n = len(got["segs"])
                # 中心要跟着更新，否则先来的那条段的位置会一直当基准，
                # 路口边缘的段就圈不进来
                got["center"] = unreal.Vector(
                    sum(mid(x).x for x in got["segs"]) / n,
                    sum(mid(x).y for x in got["segs"]) / n,
                    sum(mid(x).z for x in got["segs"]) / n)
    w("要处理的物理路口 %d 个" % len(clusters))

    w("")
    flush()

    # 车靠 Box 找下一条线，Box 在 actor 原点。把全图的 Box 收齐，
    # 待会儿量"转弯终点离最近的 Box 有多远"，并和直行通过的同一数字对比。
    boxes = []
    for a in actors:
        if not any(str(t).startswith(LANE_TAG_PREFIX) for t in a.tags):
            continue
        lbl2 = a.get_actor_label()
        if lbl2.startswith("Lane_") or lbl2.startswith("Inter_"):
            boxes.append((a.get_actor_location(), lbl2))

    def nearest_box(pt, exclude):
        best = None
        for bp, blbl in boxes:
            if blbl in exclude:
                continue
            d = (bp - pt).length()
            if best is None or d < best[0]:
                best = (d, blbl)
        return best or (9e9, "-")

    handoff = []
    written = []
    n_left = n_right = n_empty = 0
    for ci, cl in enumerate(clusters):
        roads = {}
        for s in cl["segs"]:
            roads.setdefault(s["road"], []).append(s)
        w("路口 %02d  中心(%.0f, %.0f)  道路: %s"
          % (ci, cl["center"].x, cl["center"].y, " x ".join(sorted(roads))))
        # 先把每条段的实际几何摊开——方向一旦取错，后面所有判断都是错的，
        # 而"切线平行"这种报错本身分不出是数据问题还是取值问题。
        w("  %-30s %-22s %-22s %7s %7s %7s" %
          ("段", "起点", "终点", "进°", "出°", "长度"))
        for s_ in sorted(cl["segs"], key=lambda x: x["label"]):
            ai = math.degrees(math.atan2(s_["d_in"].y, s_["d_in"].x))
            ao = math.degrees(math.atan2(s_["d_out"].y, s_["d_out"].x))
            seglen = ((s_["p_out"].x - s_["p_in"].x) ** 2
                      + (s_["p_out"].y - s_["p_in"].y) ** 2) ** 0.5
            w("  %-30s (%7.0f,%7.0f) (%7.0f,%7.0f) %+7.1f %+7.1f %7.0f" %
              (s_["label"][:30], s_["p_in"].x, s_["p_in"].y,
               s_["p_out"].x, s_["p_out"].y, ai, ao, seglen))
        w("")
        if len(roads) < 2:
            w("  只有一条路，两条转弯样条都写成直行副本")
            for s in cl["segs"]:
                for c in (s["left"], s["right"]):
                    if c is not None:
                        mirror_straight(s["actor"], c, s["main"])
                        n_empty += 1
            continue

        names = sorted(roads)
        for a_name in names:
            # 内外道**按每条路各自**的 |偏移| 算，不能按全局一刀切。
            # 主路四车道：540 是外道只左转、180 是内道只右转。
            # 次路两车道：根本没有内外道之分（max==min），进路口的那条样条
            # 左转右转都要有——按全局值判的话它的 180 会被当成内道、
            # 只剩右转，次路就永远左转不了了。
            offs = [abs(x["offset"]) for x in roads[a_name]]
            outer_abs, inner_abs = max(offs), min(offs)
            if outer_abs == inner_abs:
                w("  %s 同向只有一种 |偏移|=%d，没有内外道之分，左右转都生成"
                  % (a_name, outer_abs))

            for src in roads[a_name]:
                ao = abs(src["offset"])
                do_left = (ao == outer_abs)
                do_right = (ao == inner_abs)

                for want_right, comp, do_it in (
                        (False, src["left"], do_left),
                        (True, src["right"], do_right)):
                    if comp is None:
                        no_comp.append((src["label"],
                                        "右" if want_right else "左"))
                        continue
                    if not do_it:
                        mirror_straight(src["actor"], comp, src["main"])
                        n_empty += 1
                        continue
                    cands, rejected = [], []
                    for b_name in names:
                        if b_name == a_name:
                            continue
                        for dst in roads[b_name]:
                            cz = (src["d_in"].x * dst["d_out"].y
                                  - src["d_in"].y * dst["d_out"].x)
                            if (cz > 0) == want_right:
                                pts, span, why = bezier(src, dst)
                                if pts is None:
                                    rejected.append((dst["label"], why))
                                else:
                                    cands.append((span, dst, pts, why))
                    if not cands:
                        w("  %-30s %s转 无候选，写成直行副本"
                          % (src["label"][:30], "右" if want_right else "左"))
                        for lbl, why in rejected[:3]:
                            w("        排除 %-28s %s" % (lbl[:28], why))
                        mirror_straight(src["actor"], comp, src["main"])
                        n_empty += 1
                        continue
                    # 多个候选时取最短的那条——转弯自然走最近的目标车道
                    cands.sort(key=lambda t: t[0])
                    span, dst, pts, note = cands[0]
                    write_spline(comp, pts)
                    # 记下来，全部写完后在**同一次运行内**回读一遍。
                    # 计数器只能证明 write_spline 被调用过，证明不了结果留住了。
                    written.append((src["actor"], src["label"], comp,
                                    len(pts), pts[-1]))
                    # 转弯终点 与 直行通过后终点 各自离最近的 Box 多远。
                    # 两者终点是同一个（实测差 0.0），所以这两个数应该完全一样；
                    # 不一样就说明"接不上"另有来源。
                    d_turn = nearest_box(pts[-1], {src["label"], dst["label"]})
                    d_thru = nearest_box(dst["p_out"], {dst["label"]})
                    handoff.append((src["label"], "右" if want_right else "左",
                                    dst["label"], d_turn, d_thru))
                    if want_right:
                        n_right += 1
                    else:
                        n_left += 1
                    others = ("，另有 %d 个候选" % (len(cands) - 1)) if len(cands) > 1 else ""
                    a_in = math.degrees(math.atan2(src["d_in"].y, src["d_in"].x))
                    a_out = math.degrees(math.atan2(dst["d_out"].y, dst["d_out"].x))
                    turn = (a_out - a_in + 540.0) % 360.0 - 180.0
                    w("  %-30s %s转 -> %-28s 跨度 %.0f  进%+.0f° 出%+.0f° 转%+.0f°%s"
                      % (src["label"][:30], "右" if want_right else "左",
                         dst["label"][:28], span, a_in, a_out, turn,
                         others + ("  " + note if note else "")))
        flush()

    w("")
    w("转弯终点的交接情况（车靠 Box 找下一条线）：")
    w("  %-30s %-2s %-28s %-30s %s"
      % ("来向", "转", "去向", "转弯终点最近的Box", "直行终点最近的Box"))
    for sl, kd, dl, dt, dh in handoff:
        flag = "" if abs(dt[0] - dh[0]) < 1.0 else "   <<< 两者不一致"
        w("  %-30s %-2s %-28s %-24s %6.0fcm  %-24s %6.0fcm%s"
          % (sl[:30], kd, dl[:28], dt[1][:24], dt[0], dh[1][:24], dh[0], flag))
    w("  转弯终点和直行终点是同一个点，所以这两列本该完全相同。")
    w("  若相同而车仍接不上，问题不在样条几何，在车的探测/行为。")
    w("")
    if no_comp:
        kinds = {}
        for _l, k in no_comp:
            kinds[k] = kinds.get(k, 0) + 1
        w("!! 有 %d 处因为**组件不存在**而没能生成：%s"
          % (len(no_comp), "、".join("%s转 %d 处" % (k, v) for k, v in sorted(kinds.items()))))
        w("   蓝图里缺 %s / %s 的话，对应方向的转弯就无法生成——"
          % (LEFT_COMP, RIGHT_COMP))
        w("   组件是在蓝图 SCS 上定义的，实例侧补不回来，只能去蓝图里加回去。")
    # --- 同一次运行内回读，看写进去的还在不在 ---
    bad = []
    for _a, lbl, comp, want_n, want_end in written:
        try:
            got_n = comp.get_number_of_spline_points()
            got_end = comp.get_location_at_spline_point(got_n - 1, WS)
            drift = ((got_end.x - want_end.x) ** 2
                     + (got_end.y - want_end.y) ** 2) ** 0.5
        except Exception as exc:
            bad.append((lbl, comp.get_name(), "回读异常 %s" % str(exc)[:40]))
            continue
        # 点数比期望的少是正常的：写入时会去掉相邻重合点（dedup_pts），
        # 而 want_n 记的是去重**之前**的采样点数。圆弧在很短的弯上会采出
        # 几个相距不到 2cm 的点，去掉它们不改变形状——真正要看的是**终点**。
        # 拿去重前的点数当判据的话，这类会被报成"对不上"，白查一轮
        # （实测 362 条里误报 15 条，终点偏离全都只有 0~2cm）。
        if drift > 1.0:
            bad.append((lbl, comp.get_name(),
                        "期望终点(%.0f,%.0f)，实际偏离 %.0f（%d 点）"
                        % (want_end.x, want_end.y, drift, got_n)))
        elif got_n > want_n:
            # 反过来多出点就不正常了，去重只会变少
            bad.append((lbl, comp.get_name(),
                        "点数变多了：期望 %d，实际 %d" % (want_n, got_n)))
        elif got_n < 3:
            bad.append((lbl, comp.get_name(),
                        "只剩 %d 个点，去重去过头了" % got_n))
    w("同一次运行内回读 %d 条转弯：%s"
      % (len(written), "全部一致" if not bad else "**%d 条对不上**" % len(bad)))
    for lbl, cn, why in bad[:8]:
        w("   %-30s %-12s %s" % (lbl[:30], cn, why))
    if bad:
        w("   -> 写完在同一个脚本里就已经不是刚写的值了，说明本轮内部有东西覆盖它。")
    else:
        w("   -> 脚本内部是好的。若之后再看变回直行/默认，就是脚本跑完之后被冲的。")
    w("")
    w("写入左转 %d 条，右转 %d 条，写成直行副本 %d 条" % (n_left, n_right, n_empty))
    if _mark_failed:
        w("!! 有 %d 条没能打上 Override Construction Script 标记，"
          % len(_mark_failed))
        w("   构造脚本一重跑就会把它们冲回蓝图默认值：%s"
          % ", ".join(sorted(set(_mark_failed))[:5]))
    w("直行副本 = 这条车道没有这个方向的转弯，选中它就等于直行，")
    w("蓝图那边不需要任何特判。")
    w("")
    w("蓝图侧的选路逻辑已经改好（车会在三条里随机挑一条），这里不再提示。")
    w("关卡尚未保存。")
    flush()


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[turnfill] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
