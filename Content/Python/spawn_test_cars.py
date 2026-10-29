# -*- coding: utf-8 -*-
"""清掉关卡里所有 BP_car_base，在车道段起点重新摆一批测试车。

**摆位规则是从蓝图图表反推出来的，不是手感。** 见 Saved/bp_graph.txt：

  TraceForNewPath:  球半径 150
                    Start = GetActorLocation()
                    End   = Start + Forward × FrameRate × CurrentSpeed × 5

  GetFuturePostionandRotationAlongSpline:
                    每帧用 GetDistanceAlongSplineAtLocation(GetActorLocation())
                    **重新**算自己在样条上的位置

由此得到三条硬约束：

1. **出生那一刻 CurrentSpeed = 0，End == Start，这是零长度扫描——
   UE 的零长度 sweep 不返回命中。** 所以车不可能「一出生就接上路」，
   它一定要先动起来（加速计时器 0.1 秒一跳）探测才有长度。
2. **接上路的判据是「Box 落在球里」，而球半径只有 150。**
   Box 半尺寸 (83, 83, 390) 挂在车道段的**第一个控制点**上，
   所以出生点到第一个控制点的距离必须 < 150 + 83 = 233，否则车只能
   靠裸奔瞎撞；撞不到就 RouteLostTimeout(3 秒) 缩小消失、瞬移回出生点，
   再撞一次——表现就是「这辆车永远走不上交通线」。
   BEHIND_BOX = 250 是**刻意略大于 233** 的：这样出生姿势下球还没碰到
   Box（250 - 83 = 167 > 150），第一次有速度的那一跳是一次**真正的
   sweep 命中**，不依赖「初始重叠算不算命中」这个没实测过的语义；
   而要走的距离只有 17cm，裸奔几乎不发生。
   **原来写的是 300**，正好落在 233 外侧——那批车全靠裸奔撞运气。
3. 位置是每帧现算的，所以车不必落在样条起点，也不怕「开过了起点」——
   原来注释里「放在起点之后就永远接不上路」那条是错的，
   真正的门槛是距离，不是先后。

另外几道筛子，都是为了「摆下去的车都能走」而不是「摆得多」：
  * **出生点底下必须有路面**（逐点打射线只认 SplineMesh），没有就跳过。
    没有这道筛子，车会出生在虚空里或者陷在地形下面。
  * **出生点附近不能有别的段的第一个控制点**（COMPETE_R），
    否则车可能焊到横向车流或者对向的段上，当场横向瞬移出去。
  * **前方 FOLLOW_CLEAR 的锥体里不能已经有车**：避让逻辑现在是真生效的
    （ForwardTraceDistance 900 / MinFollowDistance 350），一出生就互相
    判成前车的两辆车会集体停住。判据是**有向的**（沿前方 + 横向偏移），
    所以同一横断面上的 4 条平行车道不会被误杀——它们横向只差 360，
    但互相不在对方的探测锥里。原来那套「每隔 STRIDE 条取一条」做不到这件事：
    排序是按离锚点的距离，相邻两条可能是同一横断面的平行车道，也可能
    首尾相接，STRIDE 既误杀又漏放。
  * **车道末端必须有下家**（NEXT_MAX + 同向），不然车开到头就裸奔，
    那不是「接不上路」而是「路到头了」，混在一起会把真问题盖住。

挑哪些车道：以 ANCHOR_ROAD 为中心按距离排序，依次过上面几道筛子，
凑够 MAX_CARS 就停。只影响 BP_car_base 实例，不碰交通线。
用法：py spawn_test_cars.py
"""

import math
import random
import traceback

import unreal

VEHICLE_DIR = "/Game/Vehicle"      # 车型都在这儿（不递归，Motorbike 子目录自动排除）
NAME_PREFIX = "BP_car_base"        # 车型蓝图和已放置实例的类名都按这个前缀认
SCALE = 0.7                        # 默认缩放
SCALE_BY_VARIANT = {               # 个别车型模型大小不同，单独给
    "BP_car_base2": 0.5,
    "BP_car_base3": 0.8,
}
LANE_TAG_PREFIX = "ClaudeGenLane"

ANCHOR_ROAD = "MainRoad_R003"   # 以这条路为中心往外挑；"" = 全图均匀挑。
                           # 两个极端都不要：挨着放会互相判成前车集体停住，
                           # 撒太开（实测 952m x 918m）视口里只看得见一两辆，
                           # 看着像「车都不见了」。锚在 MainRoad_R003 上是因为
                           # R024/R035/R036/R038 几条合并路都横穿它，
                           # 正好顺带验证 _Part 切点处车过不过得去。
MAX_CARS = 30              # **这是「摆放数」，不是最终车数。**
                           # 组件默认 bSpawnExtraWaves=true，每辆车会在
                           # **自己的出生点**再克隆两波（10 秒、再 15 秒），
                           # 所以 25 秒后实际车数 = 摆放数 × 3 = 90 辆。
                           # 别直接设 90：那是 270 辆，而且三波是在同一个
                           # 出生点叠出来的，加上避让真生效，容易连锁堵死。
                           # 想一眼看到全部车、不等 25 秒，去 BP_car_base 的
                           # Delivery Traffic Car 组件上关掉 bSpawnExtraWaves。

BEHIND_BOX = 250.0         # 沿行进方向退到车道段第一个控制点**之前**这么远。
                           # 为什么正好是这个数：见文件头第 2 条。
                           # 调小到 233 以内 → 出生姿势下球已经压着 Box，
                           # 变成「初始重叠算不算命中」的赌注；
                           # 调大到 233 以上越多 → 裸奔距离越长，
                           # 期间车是直线走的，弯道上会横向漂出车道。
MAX_SPAWN_DIST = 233.0     # 150(球半径) + 83(Box 沿行驶方向的半尺寸)。
                           # 能接上路的最远出生距离，报告里要打出来。

SURFACE_TOL = 150.0        # 出生点实测路面和车道点 Z 差超过这个就跳过这条车道。
                           # 车道点自己带 Z_OFFSET=2，正常只差几厘米；
                           # 差上百厘米说明退到的那 250cm 已经不在这条路面上
                           # （路口桥接段、或者路到头了）。
COMPETE_R = 300.0          # 出生点这么近有别的段的第一个控制点就跳过：擦着边的
                           # 那种，焊到谁身上取决于 sweep 先碰到哪一个，不确定。
                           # **上下界都是硬的，不能随手调**：
                           #   下界 233（= MAX_SPAWN_DIST，真正能被探到的范围）
                           #   上界 360（主路同向两条车道 ±180 / ±540 的横向间距）
                           # 超过 360 会把主路一半的候选误杀——那条平行车道的箱子
                           # 在 360 cm 外，压根进不了半径 150 的球，不构成竞争。
FOLLOW_CLEAR = 1050.0      # 前方这么远的锥体里已经有车就跳过。
                           # = ForwardTraceDistance(900) + TraceStartForwardOffset(150)。
FOLLOW_HALF_WIDTH = 250.0  # 锥体的横向半宽。ForwardTraceRadius 是 80，
                           # 留到 250 是因为车本身有宽度、样条还会弯。
NEXT_MAX = 700.0           # 车道末端这么远内要有下家段的第一个控制点。
                           # 正常空隙是 TARGET_GAP=150，给到 700 容下
                           # 路口段加长和 _Part 切点处的缺口。
NEXT_DOT = 0.5             # 下家必须大致同向，否则会把对向车道认成下家。

CAR_Z = 0.0                # 车高度微调（加到**实测路面**的 Z 上，不再加到车道点上）。
                           # 注意：**车开起来的高度和这个值无关**——
                           # 焊上样条之后高度完全由 GetLocationAtDistanceAlongSpline
                           # 决定，也就是车道点自己的 Z。所以「车浮着」调这里没用，
                           # 要去调车道的高度剖面（跑 diag_lane_fit.py 逐点测）。
                           # 这里只管「出生那一两帧站在地上」。
MIN_LANE_LEN = 1500.0      # 太短的段不放（车还没加速就到头了）
TAG = "ClaudeTestCar"

# 车速随机：600~1500 的 100 倍数。MaxSpeed 在整张蓝图里没有 Set 节点、
# 是纯配置变量，所以实例上改了就一直有效。
SPEED_MIN, SPEED_MAX, SPEED_STEP = 500, 1300, 100

# 车速**偏向低速**：权重 = (档位数 - 下标) ** SPEED_LOW_BIAS，所以最低档权重最大。
# 为什么不用均匀分布：均匀取时快车占一半，而快车在这张图上问题最多——
# 前车探测只有 900cm（弯道上不能再调大），1300 下只有 0.69 秒前瞻；
# 而且 MinFollowDistance 350 的停车距离是按中低速调的。
# bias 越大越偏慢：0 = 均匀，1 = 线性递减，2 = 平方递减。
# 选 1.0：各档 500→1300 分别是 20/18/16/13/11/9/7/4/2%，期望均值 767
#（均匀分布会是 900）。**没选 2.0**，那样 1300 只剩 0.4%、1200 只剩 1%，
# 等于把上限砍掉了、"500~1300"这个区间名不副实。
# 想更慢就调到 1.5~2.0，但要接受高速档基本不出现。
SPEED_LOW_BIAS = 1.0

# 车型权重。`find_variants()` 把 VEHICLE_DIR 下所有 BP_car_base* 列出来，
# 原来是 `variants[made % len(variants)]` **轮流**用，每种一样多。
# 这里改成带权随机，没列出来的车型按 1.0 算。
# base3 调到 0.25：用户要求它少一些。
VARIANT_WEIGHTS = {
    "BP_car_base3": 0.25,
}

# 车型**轮换**着用（不随机）：六种车自带不同材质（plastic_red / plastic_yellow /
# plastic_white / plastic_dark …），本来就互相区分得开，轮换还能保证每种都出场。
# 原来那套 paint() 往蓝图变量/材质参数上写颜色**三十辆全部失败**
# （上一次报告里是「没上成 × 30」），而且它建的是 MID、编辑器里存不住，已删掉。

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "test_cars.txt"
lines = []


def w(s=""):
    lines.append(str(s))


def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
    except Exception:
        pass


def find_variants():
    """列出 VEHICLE_DIR 下的车型蓝图。"""
    out = []
    try:
        paths = unreal.EditorAssetLibrary.list_assets(
            VEHICLE_DIR, recursive=False, include_folder=False)
    except Exception as exc:
        w("!! 列不出 %s：%s" % (VEHICLE_DIR, str(exc)[:60]))
        return out
    for pth in paths:
        name = str(pth).split("/")[-1].split(".")[0]
        if not name.startswith(NAME_PREFIX):
            continue
        asset = unreal.EditorAssetLibrary.load_asset(str(pth).split(".")[0])
        if asset is not None and isinstance(asset, unreal.Blueprint):
            out.append((name, asset))
    out.sort(key=lambda t: t[0])
    return out


def road_surface_z(world, x, y, z_hint):
    """这一竖线上离 z_hint 最近的**路面**（SplineMesh）高度，没有就 None。

    和 gen_traffic_lanes.surface_layers 同一套做法：逐层往下扫，
    只认 SplineMesh（地形/裙边/建筑都跳过），命中一块板就整块跨过去
    （射线每次只降 1cm 的话，26cm 厚的板会被连着命中二十几次，预算全耗在
    同一块板上）。路口处两条路的路面是叠着的，所以要「离 z_hint 最近」
    而不是「最上面」——取最上面会在路口把车放到交叉路的面上。
    """
    top = z_hint + 3000.0
    bottom = z_hint - 3000.0
    best = None
    for _ in range(14):
        try:
            hit = unreal.SystemLibrary.line_trace_single(
                world, unreal.Vector(x, y, top), unreal.Vector(x, y, bottom),
                unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
                False, [], unreal.DrawDebugTrace.NONE, True)
        except Exception:
            break
        if hit is None:
            break
        d = hit.to_dict()
        if not d.get("blocking_hit"):
            break
        comp = d.get("hit_component")
        z = d["impact_point"].z
        cn = comp.get_class().get_name() if comp is not None else "?"
        if "SplineMesh" in cn:
            if best is None or abs(z - z_hint) < abs(best - z_hint):
                best = z
            top = z - 30.0            # 一步跨过整块板
        else:
            top = z - 1.0             # 地形/裙边/建筑，逐个跳过
        if top <= bottom:
            break
    return best


def seg_dir(sp, at_start=True):
    """段首/段尾的行进方向（XY 单位向量），取不到返回 None。

    不用 get_direction_at_distance_along_spline——端点切线被 CURVE_CLAMPED
    归零过（见 CLAUDE.md），那个函数在端点会返回零向量。
    用样条上相距 100cm 的两个采样点作差。
    取不到时返回 None 而不是兜底成 (1,0)：兜底会把「取不到方向」伪装成
    「方向朝 +X」，车被摆成横着的，而报告里一切正常。
    """
    L = sp.get_spline_length()
    step = min(100.0, L)
    if at_start:
        a = sp.get_location_at_distance_along_spline(0.0, WS)
        b = sp.get_location_at_distance_along_spline(step, WS)
    else:
        a = sp.get_location_at_distance_along_spline(max(0.0, L - step), WS)
        b = sp.get_location_at_distance_along_spline(L, WS)
    dx, dy = b.x - a.x, b.y - a.y
    h = (dx * dx + dy * dy) ** 0.5
    if h < 1e-3:
        return None
    return dx / h, dy / h


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(
        unreal.UnrealEditorSubsystem).get_editor_world()
    actors = eas.get_all_level_actors()

    # --- 0. 先确认车型加载得了，再删旧车 ---
    # 上一版是先删后载，而车蓝图被移到 /Game/Vehicle 之后旧路径失效，
    # 结果 20 辆全删了、一辆没放出来。**删除是不可逆的，必须排在校验之后。**
    variants = find_variants()
    w("车型 %d 种（%s）：%s"
      % (len(variants), VEHICLE_DIR, "、".join(n for n, _ in variants)))
    if not variants:
        w("!! 一种车型都没加载出来，什么都不做（旧车原样保留）。")
        w("   检查 VEHICLE_DIR 和 NAME_PREFIX 是不是对的。")
        flush()
        return
    if world is None:
        w("!! 取不到编辑器世界，没法打地面射线，什么都不做（旧车原样保留）。")
        flush()
        return
    w("")

    # --- 1. 收集全部交通段（车道 + 路口）---
    # 路口段也要收：它们的第一个控制点同样挂着 Box，是「出生点附近有别的
    # 段的箱子」和「末端有没有下家」两道判据的素材。
    segs = []          # (label, spline, 第一个控制点, 段首方向)
    lanes = []         # 候选车道段
    for a in actors:
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
        sp = a.get_component_by_class(unreal.SplineComponent)
        if sp is None or sp.get_number_of_spline_points() < 2:
            continue
        p0 = sp.get_location_at_distance_along_spline(0.0, WS)
        d0 = seg_dir(sp, True)
        segs.append((lbl, sp, p0, d0))
        if (lbl.startswith("Lane_")
                and sp.get_spline_length() >= MIN_LANE_LEN
                and d0 is not None):
            lanes.append((lbl, sp, p0, d0))

    w("交通段 %d 个（Lane_ + Inter_），其中可用车道段 %d 条（长度 >= %.0f）"
      % (len(segs), len(lanes), MIN_LANE_LEN))
    if not lanes:
        w("!! 一条都没有，先跑 gen_traffic_lanes.py")
        flush()
        return
    w("")

    # --- 2. 删掉所有车 ---
    # 按**类**（BP_car_base_C）删，不按名字——关卡里的车实例名字五花八门
    # （carbase4 / BP_car_base2 / ...），按标签匹配漏得到处都是。
    cars = [a for a in actors
            if str(a.get_class().get_name()).startswith(NAME_PREFIX)]
    killed = 0
    for a in cars:
        try:
            eas.destroy_actor(a)
            killed += 1
        except Exception as exc:
            w("!! 删不掉 %s: %s" % (a.get_actor_label(), str(exc)[:50]))
    w("找到车 %d 辆（类名以 %s 开头），已删除 %d 个"
      % (len(cars), NAME_PREFIX, killed))
    w("")
    flush()

    # --- 3. 按离锚定道路的距离排序 ---
    if ANCHOR_ROAD:
        anchor_pts = [sp.get_location_at_distance_along_spline(
                          sp.get_spline_length() * 0.5, WS)
                      for lbl, sp, _p, _d in lanes if ANCHOR_ROAD in lbl]
        if anchor_pts:
            cx = sum(p.x for p in anchor_pts) / len(anchor_pts)
            cy = sum(p.y for p in anchor_pts) / len(anchor_pts)
            w("锚点 %s 的中心 (%.0f, %.0f)，按离它的距离排序"
              % (ANCHOR_ROAD, cx, cy))
            lanes.sort(key=lambda t: (t[2].x - cx) ** 2 + (t[2].y - cy) ** 2)
        else:
            w("!! 没有名字含 %s 的车道段，改为全图顺序" % ANCHOR_ROAD)
            lanes.sort(key=lambda t: t[0])
    else:
        lanes.sort(key=lambda t: t[0])
    w("")

    # --- 4. 逐条过筛子，够数就停 ---
    speeds = list(range(SPEED_MIN, SPEED_MAX + 1, SPEED_STEP))
    rng = random.Random(20261029)      # 固定种子：重跑一次车速分布一样，
                                       # 便于「改了别的东西前后对比」
    spd_hist, var_hist = {}, {}
    n_sp = len(speeds)
    speed_weights = [float(n_sp - i) ** SPEED_LOW_BIAS for i in range(n_sp)]
    variant_weights = [VARIANT_WEIGHTS.get(nm, 1.0) for nm, _bp in variants]
    _tw = sum(speed_weights)
    w("车速从 %s 里带权随机（固定种子），权重偏低速 bias=%.1f" % (speeds, SPEED_LOW_BIAS))
    w("   各档概率：%s"
      % "  ".join("%d:%.0f%%" % (sp, 100.0 * wt / _tw)
                  for sp, wt in zip(speeds, speed_weights)))
    w("   期望均值 %.0f（均匀分布会是 %.0f）"
      % (sum(sp * wt for sp, wt in zip(speeds, speed_weights)) / _tw,
         sum(speeds) / float(n_sp)))
    w("车型权重：%s"
      % "  ".join("%s=%.2f" % (nm, wt) for (nm, _b), wt
                  in zip(variants, variant_weights)))
    w("")
    w("%-4s %-30s %-14s %-26s %7s %6s %5s %7s %7s"
      % ("#", "车道段", "车型", "出生位置", "朝向", "车速", "缩放",
         "到箱心", "地面差"))
    w("-" * 132)

    made = 0
    placed = []        # (出生点 x, y, 前方单位向量 dx, dy)
    reject = {}
    detail = []

    def drop(lbl, why, extra=""):
        reject[why] = reject.get(why, 0) + 1
        detail.append("   %-32s %s %s" % (lbl[:32], why, extra))

    for lbl, sp, p0, d0 in lanes:
        if made >= MAX_CARS:
            break
        dx, dy = d0

        # 4a. 末端要有下家（同向），否则车开到头就裸奔
        dend = seg_dir(sp, False)
        pend = sp.get_location_at_distance_along_spline(
            sp.get_spline_length(), WS)
        has_next = False
        if dend is not None:
            for lbl2, _sp2, q0, dq in segs:
                if lbl2 == lbl or dq is None:
                    continue
                if math.hypot(q0.x - pend.x, q0.y - pend.y) > NEXT_MAX:
                    continue
                if dend[0] * dq[0] + dend[1] * dq[1] > NEXT_DOT:
                    has_next = True
                    break
        if not has_next:
            drop(lbl, "末端没有同向下家", "（开到头就裸奔，不是接不上路）")
            continue

        # 4b. 出生点：沿行进方向退到第一个控制点之前 BEHIND_BOX
        sx = p0.x - dx * BEHIND_BOX
        sy = p0.y - dy * BEHIND_BOX

        # 4c. 底下要有路面
        gz = road_surface_z(world, sx, sy, p0.z)
        if gz is None:
            drop(lbl, "出生点底下没有路面",
                 "（退 %.0f 已经出了这条路）" % BEHIND_BOX)
            continue
        dz = p0.z - gz
        if abs(dz) > SURFACE_TOL:
            drop(lbl, "出生点路面和车道点差太多", "（%.0f cm）" % dz)
            continue

        # 4d. 附近不能有别的段的第一个控制点
        rival = None
        for lbl2, _sp2, q0, _dq in segs:
            if lbl2 == lbl:
                continue
            if math.hypot(q0.x - sx, q0.y - sy) <= COMPETE_R:
                rival = lbl2
                break
        if rival is not None:
            drop(lbl, "出生点附近有别的段的箱子", "（%s）" % rival[:26])
            continue

        # 4e. 前方锥体里不能已经有车（有向判据，不误杀平行车道）
        blocked = None
        for (ox, oy, odx, ody) in placed:
            vx, vy = ox - sx, oy - sy
            ahead = vx * dx + vy * dy
            side = abs(-vx * dy + vy * dx)
            if 0.0 <= ahead <= FOLLOW_CLEAR and side <= FOLLOW_HALF_WIDTH:
                blocked = "前方 %.0f cm 已有车" % ahead
                break
            vx, vy = sx - ox, sy - oy          # 反过来：我在它的锥里
            ahead = vx * odx + vy * ody
            side = abs(-vx * ody + vy * odx)
            if 0.0 <= ahead <= FOLLOW_CLEAR and side <= FOLLOW_HALF_WIDTH:
                blocked = "落在已有车前方 %.0f cm" % ahead
                break
        if blocked is not None:
            drop(lbl, "和已摆的车互相挡", "（%s）" % blocked)
            continue

        # 4f. 放
        loc = unreal.Vector(sx, sy, gz + CAR_Z)
        yaw = math.degrees(math.atan2(dy, dx))
        # unreal.Rotator 的构造参数是 (roll, pitch, yaw)，和 C++ 的
        # FRotator(Pitch, Yaw, Roll) 顺序不一样。把 yaw 填进第二个位置
        # 就变成 pitch，车会被竖起来，而且「歪的角度」跟着那个值变，
        # 看着像随机其实是确定的。
        rot = unreal.Rotator(0.0, 0.0, yaw)
        vname, bp = rng.choices(variants, weights=variant_weights, k=1)[0]
        try:
            car = eas.spawn_actor_from_object(bp, loc, rot)
        except Exception as exc:
            drop(lbl, "spawn 失败", str(exc)[:46])
            continue
        if car is None:
            drop(lbl, "spawn 返回 None")
            continue
        try:
            car.set_actor_label("TestCar_%02d" % made)
            car.set_editor_property("tags", list(car.tags) + [TAG])
        except Exception:
            pass

        sc = SCALE_BY_VARIANT.get(vname, SCALE)
        try:
            car.set_actor_scale3d(unreal.Vector(sc, sc, sc))
        except Exception:
            pass

        spd = rng.choices(speeds, weights=speed_weights, k=1)[0]
        try:
            car.modify(True)
            car.set_editor_property("MaxSpeed", float(spd))
        except Exception as exc:
            w("   !! MaxSpeed 写不进去：%s" % str(exc)[:50])
            spd = -1

        spd_hist[spd] = spd_hist.get(spd, 0) + 1
        var_hist[vname] = var_hist.get(vname, 0) + 1
        placed.append((sx, sy, dx, dy))
        w("%-4d %-30s %-14s (%7.0f,%8.0f,%7.0f) %7.1f %6s %5.2f %7.0f %7.0f"
          % (made, lbl[:30], vname[:14], loc.x, loc.y, loc.z, yaw,
             ("%d" % spd) if spd > 0 else "失败", sc, BEHIND_BOX, dz))
        made += 1

    # --- 5. 报告 ---
    w("")
    w("=" * 78)
    w("删掉 %d 辆旧车，放置 %d 辆新车（标签 TestCar_xx，tag %s）"
      % (killed, made, TAG))
    if made < MAX_CARS:
        w("!! 只凑到 %d 辆（想要 %d）。候选车道 %d 条已全部过完。"
          % (made, MAX_CARS, len(lanes)))
        w("   下面的淘汰统计就是差额的来源。想要更多车：把 ANCHOR_ROAD 设成")
        w("   空串（全图挑）、或者调小 FOLLOW_CLEAR。")
    w("")
    w("淘汰统计（共 %d 条）：" % sum(reject.values()))
    for why, n in sorted(reject.items(), key=lambda kv: -kv[1]):
        w("   %-26s %d 条" % (why, n))
    if detail:
        w("")
        w("淘汰明细（前 60 条）：")
        for ln in detail[:60]:
            w(ln)
        if len(detail) > 60:
            w("   ... 还有 %d 条" % (len(detail) - 60))

    w("")
    w("摆位依据（从 Saved/bp_graph.txt 的图表 dump 反推，不是手感）：")
    w("  TraceForNewPath 球半径 150，End = Start + Forward*FrameRate*CurrentSpeed*5。")
    w("  出生时 CurrentSpeed=0，End==Start，零长度扫描不返回命中——")
    w("  所以车一定要先动起来才可能接上路。")
    w("  Box 半尺寸 (83,83,390) 挂在车道段第一个控制点上，于是能接上路的")
    w("  最远出生距离是 150+83 = %.0f。" % MAX_SPAWN_DIST)
    w("  BEHIND_BOX=%.0f 刻意略大于它：出生姿势下球还没压着 Box"
      % BEHIND_BOX)
    w("  （%.0f-83=%.0f > 150），所以第一次有速度的那一跳是一次真正的 sweep"
      % (BEHIND_BOX, BEHIND_BOX - 83.0))
    w("  命中，不赌「初始重叠算不算命中」；而要走的距离只有 %.0f cm，"
      % (BEHIND_BOX - MAX_SPAWN_DIST))
    w("  裸奔几乎不发生。原来是 300，正好在 %.0f 外侧。" % MAX_SPAWN_DIST)
    w("")
    w("验证方法：跑 Simulate，正常的车应该**几乎立刻**沿车道走。不该出现")
    w("  「直线开出去 → 缩小消失 → 回到出生点」的循环——那是")
    w("  RouteLostTimeout(3 秒) 在兜底，说明那辆车没接上路。")
    w("  逐辆跟踪用 monitor_car.py / chase_car.py（每帧回调，不要 sleep 轮询）。")
    w("")
    w("车开起来的高度和 CAR_Z 无关：焊上样条之后高度完全由车道点自己的 Z")
    w("  决定。所以「车浮着」要去跑 diag_lane_fit.py 查车道高度剖面，")
    w("  调 CAR_Z 只会掩盖它。上表「地面差」列 = 车道点 Z - 实测路面 Z，")
    w("  正常是几厘米（车道自带 Z_OFFSET=2）。")
    w("")
    w("关卡尚未保存。")
    flush()
    unreal.log("[cars] 写入 %s" % OUT)


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[cars] fail:" + chr(10) + err)
    lines.append("")
    lines.append("!! 中途异常（以上结果仍然有效）：")
    lines.append(err)
    flush()
