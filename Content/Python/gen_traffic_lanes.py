# -*- coding: utf-8 -*-
"""沿道路样条生成交通车道，路口处断开并换成 IntersectionChild。

结构：
    路段(BP_TrafficLine1) --空隙-- 路口(IntersectionChild) --空隙-- 路段 ...

实测依据：
  * 车道数按实测路宽自动分：主路 1800 宽 -> 4 车道（偏移 +-225/+-675），
    次路 1080 宽 -> 2 车道（偏移 +-270）。靠左行驶。
    宽度实测用低分位数 + 上限，避开路口处横向探测打到交叉路造成的虚高。
  * BP_TrafficLine1 与 IntersectionChild 结构完全相同（Spline + 固定 Box），
    Box 只是"起点探测把手"，所以一段路用一条样条即可
  * 不能信任源样条的 Z：形状13 与真实路面偏差 -546 ~ +411 cm，
    真正的路面是 Landscape 上的地形样条网格，必须逐点打射线重测
  * UE5.8 的 HitResult 属性是 protected，取值走 to_dict()

可重复运行：tag 按路区分（"ClaudeGenLane:<路名>"），每次只清掉 ROAD_LABELS
里那几条路的旧 actor，不会误删其它路已经生成好的车道。
"""

import traceback

import unreal

CROSSING_WITH = "形状 13"  # 非空时自动算出要处理的路：这条路 + 所有与它相交的路。
                          # 用的是和切路口同一套 find_intersections，
                          # 以后改样条也不会过期，不用手动维护名单。
                          # 置成 "" 就退回下面的手写名单。
ROAD_LABELS = ["形状 13", "形状 39"]   # CROSSING_WITH 为空时才用这个
EXCLUDE_ROADS = ["形状 43"]            # 这几条路不铺车道，自动名单里也剔掉。
                                      # 光在关卡里删 actor 不够——自动模式下次
                                      # 会把它们重新算进来，必须在这里也排除。
LANE_BP = "/Game/PS2DEM/BP_TrafficLine1"
CHILD_BP = "/Game/PS2DEM/BP_TrafficLine1_IntersectionChild"
TAG_PREFIX = "ClaudeGenLane"   # 实际 tag = "ClaudeGenLane:<路名>"，按路独立，互不误删

# 主次路按**大纲文件夹**判定，不靠实测宽度猜——
# 实测会被路口、裙边、压平的地形干扰（形状13/39 都被测成上限 1000），
# 而文件夹是人工整理的，确定且可控。
MAIN_ROAD_FOLDER = "MainRoad"
MAIN_ROAD_WIDTH, MAIN_ROAD_LANES = 1800.0, 4     # 偏移 +-225 / +-675
SIDE_ROAD_WIDTH, SIDE_ROAD_LANES = 1080.0, 2

# 车道横向位置。None = 按 宽度/车道数 均分（2 车道 1080 宽 -> +-270，
# 等于把路四等分、车道压在 1/4 和 3/4 上，看着偏外）。
# 填了数组就直接用这组偏移。
# 次路要的是"两条车道三分道路"：车道落在 1/3 和 2/3 上 -> +-宽度/6 = +-180。
# 主路 1800 宽 4 车道。几轮调整下来的落点：
#   +-225/+-675  均分公式的原始值，最外侧离路缘只有 225，整体偏外
#   +-180/+-540  改成落在五等分点上，整体往里收
#   +-280/+-560  试过把中间两条往外推、中央间距拉到 560，看下来不如上一版，已回退
# 当前用 +-180/+-540：中央间距 360，同向两条间距 360，最外侧离路缘 360，
# 三个间距一样大——就是"车道落在五等分点上"这条规则的直接结果。
MAIN_ROAD_OFFSETS = [-540.0, -180.0, 180.0, 540.0]
SIDE_ROAD_OFFSETS = [-180.0, 180.0]   # 次路 1080 宽 2 车道，三等分 -> +-宽度/6
SAMPLE_STEP = 500.0
Z_OFFSET = 15.0
LEFT_HAND_TRAFFIC = True

# --- 路口切断参数 ---
MIN_SEG_LEN = 300.0      # 两个路口之间短于此值才放弃生成路段。
                         # 原来用 SAMPLE_STEP*2 = 1000，太保守：形状39 上
                         # 7000 和 10500 两个路口之间可用 960cm，被判成"太短"
                         # 直接不生成，那里就空了一整段。
TARGET_GAP = 150.0       # 路段末端到路口段起点的目标空隙。
                         # 250 时实测车接不上下一段（间隙里车是脱离样条直行的，
                         # 越长横向漂得越多，弯道上容易错过下一段的 Box）。
                         # 路口盒那边现在不依赖大间隙了——gen_intersections.py
                         # 改成四边独立伸缩 + 覆盖是硬约束之后，窄间隙也能摆。
BOX_MARGIN = 50.0        # Box 距重叠区边界留多少余量
# BREAK_HALF / INTER_HALF 不再是常数，按每个路口交叉道路的宽度逐个算：
#   INTER_HALF = 交叉路半宽 - BOX_MARGIN      Box 必须落在重叠区内
#   BREAK_HALF = INTER_HALF + SEG_EXTEND + TARGET_GAP
# 这样宽路口的路口样条可以很长，窄路口自动收窄，空隙恒为 TARGET_GAP。
INTER_HALF_EXTRA = 300.0 # 路口段在算出来的半长上再往两头各加这么多。
                         # 半长本来 = 交叉路半宽 - BOX_MARGIN（次路 490、主路 850），
                         # 加 300 之后是 790 / 1150，整段长度约 1.6 倍。
                         #
                         # 已知代价（用户权衡后决定接受）：36 个路口里有 23 个的
                         # 路口盒为了罩住加长的路口段，长大到压上了车道 Box，
                         # "只包裹路口样条线、不覆盖普通交通线"这条守不住。
                         # 盒子两头一起扩、又要同时罩住来自两条路的多个 Box，
                         # 扩张量不是线性传导的，300 把余量吃光了。
                         # 想回到不重叠就降到 150 左右，路口段仍有 1.3 倍。
                         # 这个值会自动传导：BREAK_HALF = INTER_HALF + SEG_EXTEND
                         # + TARGET_GAP，路段会跟着往后退，空隙仍然是 TARGET_GAP。
                         # 代价：Box 在 c - INTER_HALF 处，加长后它离路口中心更远、
                         # 可能跑出两路的重叠区，路口盒也得跟着长大，
                         # 有可能压到车道 Box（gen_intersections 的报告里会写明）。
BREAK_HALF_FALLBACK = 900.0   # 认不出交叉路时的兜底
DETECT_STEP = 250.0
XY_CROSS = 200.0         # 最近 XY 小于此值才算真的相交（实测真路口都 <120）
Z_CROSS = 600.0          # Z 差大于此值是立交，不断开
MERGE_DIST = 2500.0      # 中心相距小于此值的路口合并成一个
SEG_EXTEND = 250.0       # 路段(Lane_*)向路口方向延伸多少（两端都延）。
                         # 每段路的首尾各对着一个路口，都该伸到路口边上。
                         # 它被 BREAK_HALF 的算式抵消掉了，不影响最终空隙：
                         # 空隙 = BREAK_HALF - INTER_HALF - SEG_EXTEND = TARGET_GAP。
INTER_END_EXTEND = 0.0   # 路口段只延**行驶方向的末端**，起点不动
                         #（起点带着 Box、必须留在重叠区内，不能两端一起延）。
                         # 出口侧间隙 = TARGET_GAP - INTER_END_EXTEND
                         # 入口侧间隙 = TARGET_GAP（起点没动）
                         # TARGET_GAP 从 350 降下来之后这里必须归零：
                         # 350 时留 250 给它吃，出口侧剩 100；现在总共 250，
                         # 再吃 250 就是 0，路口段会直接怼上下一段路。
                         # 归零后进出两侧一致，都是 TARGET_GAP。
                         # 逆行段点序是倒的，末端在区间起始侧，要按 forward 判断。

BRIDGE_INTERSECTIONS = True
BRIDGE_DEV_WARN = 80.0   # 桥接高度偏离路口内实测路面超过这么多就在报告里点名
# 路口段的高度不去路口里实测，直接由两侧车道段的端点桥接出来。
#
# 理由：路口内部的路面本来就是一团乱——两条路的面叠着（实测差约 200cm）、
# 高度不一致、还有被压平出来的平台。在里面"测对高度"是个没有正确答案的问题，
# 因为根本不存在唯一的"路面"。而车需要的只有一件事：能从上一段顺畅地开到下一段。
# 所以干脆不测：XY 仍然沿源样条走（弯道路口不会被拉成直线切角），
# Z 用"上一段末端 -> 下一段起点"的线性过渡。
#
# 过渡区间取完整跨度（含两侧空隙）而不只是路口段本身，
# 这样进出两个接缝**同时**连续，不是只修一头。
# 两侧缺一段时（路网端点、或相邻段因为太短没生成）退回实测。

LIGHT_NUMBER_BY_AXIS = True
LIGHT_NUMBER_Y = 1
# 左右走向（沿 Y 轴）的路口样条线，LightNumber 设成 LIGHT_NUMBER_Y；沿 X 的不动。
# 红绿灯要交替放行，横竖两个方向必须分在不同的组里。
# 放在生成器里而不是只有事后脚本：每次重新生成都是全新的 actor，
# 事后设的值会被冲掉，和 Box 尺寸是同一类问题。

# 车道/路口样条线自带的那个 Box 的半尺寸，逐轴指定（蓝图默认是 32/32/150）。
# 原来是统一倍率 BOX_SCALE=2.0 三轴一起放大到 64/64/300，
# 但 X 是**沿行驶方向**的（Box 跟着 actor 转），X 太长等于探测区在路上拖得很长。
# 现在路段和路口段分开、逐轴给值，想单独收窄哪一轴就改哪一个。
LANE_BOX_HALF = (83.0, 83.0, 390.0)    # Lane_*   实际 166 x 166 x 780
                                       # 在 64/64/300 基础上整体放大 1.3 倍。
                                       # 曾为了消除"路口盒压到车道 Box"把 X 收到 32，
                                       # 后来确认那个重叠无所谓就撤回了——
                                       # X 是沿行驶方向的，改它会影响车看到这个 Box
                                       # 的时长，属于功能性参数，别为无害的重叠去动。
INTER_BOX_HALF = (32.0, 64.0, 300.0)   # Inter_*  X 收回默认的 32（实际 64 x 128 x 600）

SURFACE_MAX_LAYERS = 40  # 往下最多迭代几次再放弃。8 -> 16 -> 40：
                         # 层多的地方（地形+裙边+建筑+两条路的路面）会提前放弃，
                         # 白白算成打空；裙边实测能叠十几层。
SLAB_SKIP = 30.0         # 命中路面后往下跳这么多，一步跨过整块板（板厚 26cm）。
                         # 不这么做的话，每次只降 1cm、一块板要吃掉二十几次迭代，
                         # 预算全耗在同一块板上，根本够不到下面那层。
SLAB_MERGE = 60.0        # 两次命中相差小于此值算同一块板（板厚 26cm，留足余量）。
PROFILE_STEP = 250.0     # 全程高度剖面的采样间距，比 SAMPLE_STEP 密一些，
                         # 让补洞时的锚点更贴合真实路面起伏。

WS = unreal.SplineCoordinateSpace.WORLD
OUT = unreal.Paths.project_saved_dir() + "gen_lanes_report.txt"
_profiles = {}           # (路名, 偏移) -> 全程高度剖面，见 build_profile()

lines = []


def w(s=""):
    lines.append(str(s))
    unreal.log("[gen_lanes] %s" % s)


def surface_layers(world, x, y, z_hint):
    """往下逐层扫，返回这一竖线上**所有**互不相同的路面高度（由高到低）。

    为什么要全部而不是最上面那一层：路口处两条路的路面是叠着的，
    实测 形状13 × 形状1 的结合部上下两层相差约 200cm。
    只取最上面那层的话，路口内的点会爬到交叉路的面上、路口外的点在本路面上，
    接缝就出现 250cm 的台阶（实测 Inter_形状13_+0225_I00 -> Lane_..._S00）。
    取到所有层之后，由 pick_layer() 按"和相邻站点连续"来挑，而不是按高低挑。

    命中同一块板会被聚成一层：射线每次只把起点下移 1cm，
    26cm 厚的板会被连着命中二十几次，那不是二十几层。
    """
    top = z_hint + 60000.0
    bottom = z_hint - 60000.0
    layers = []
    for _ in range(SURFACE_MAX_LAYERS):
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
            if not layers or (layers[-1] - z) > SLAB_MERGE:
                layers.append(z)
            top = z - SLAB_SKIP       # 一步跨过整块板
        else:
            top = z - 1.0             # 地形/裙边/建筑，逐个跳过
        if top <= bottom:
            break
    return layers


def pick_layer(layers, expect):
    """从多层里挑一层：有参考高度就挑最接近的，没有就挑最上面的。

    "最接近"而不是"最上面"是这套修复的核心——路口处上下两层路面
    分别属于两条路，挑最上面的等于在路口把车道拽到交叉路的面上去。
    """
    if not layers:
        return None
    if expect is None:
        return layers[0]
    return min(layers, key=lambda z: abs(z - expect))


def horiz_right(sp, d):
    r = sp.get_right_vector_at_distance_along_spline(d, WS)
    h = (r.x * r.x + r.y * r.y) ** 0.5
    if h < 1e-4:
        return 1.0, 0.0
    return r.x / h, r.y / h


def inter_half_for(cross_name, actor_by_label):
    """按交叉道路的宽度决定这个路口的样条半长（同时也是 Box 的落点）。

    Box 在 actor 原点 = 样条第一个点，必须落在两路重叠区内，
    而重叠区沿本路方向的半宽 = 交叉路的半宽。
    合并路口（名字里有 '+'）取其中最窄的那条。
    """
    best = None
    for nm in str(cross_name).split("+"):
        a = actor_by_label.get(nm.strip())
        if a is None:
            continue
        _m, width, _l = classify(a)
        hw = width / 2.0 - BOX_MARGIN
        best = hw if best is None else min(best, hw)
    if best is None:
        return BREAK_HALF_FALLBACK - SEG_EXTEND - TARGET_GAP + INTER_HALF_EXTRA
    return max(150.0, best) + INTER_HALF_EXTRA


def classify(actor):
    """按大纲文件夹判定主次路，返回 (是否主路, 标称路宽, 车道数)。"""
    try:
        folder = str(actor.get_folder_path())
    except Exception:
        folder = ""
    is_main = MAIN_ROAD_FOLDER.lower() in folder.lower()
    if is_main:
        return True, MAIN_ROAD_WIDTH, MAIN_ROAD_LANES
    return False, SIDE_ROAD_WIDTH, SIDE_ROAD_LANES


def find_intersections(tsp, roads, total):
    """返回 [(路口中心沿线距离, 对向道路名)]，已排除立交、已合并相近的。"""
    raw = []
    d = 0.0
    while d <= total:
        p = tsp.get_location_at_distance_along_spline(d, WS)
        for name, osp in roads:
            q = osp.find_location_closest_to_world_location(p, WS)
            dxy = ((q.x - p.x) ** 2 + (q.y - p.y) ** 2) ** 0.5
            if dxy < XY_CROSS and abs(q.z - p.z) < Z_CROSS:
                raw.append((d, name, dxy))
        d += DETECT_STEP

    groups = []
    for dist, name, dxy in raw:
        hit = None
        for g in groups:
            if g["road"] == name and dist - g["last"] <= DETECT_STEP * 3:
                hit = g
                break
        if hit is None:
            groups.append({"road": name, "last": dist, "best": (dist, dxy)})
        else:
            hit["last"] = dist
            if dxy < hit["best"][1]:
                hit["best"] = (dist, dxy)

    centers = sorted((g["best"][0], g["road"]) for g in groups)

    merged = []
    for c, name in centers:
        if merged and c - merged[-1][0] < MERGE_DIST:
            pc, pn = merged[-1]
            merged[-1] = ((pc + c) / 2.0, pn + "+" + name)
        else:
            merged.append((c, name))
    return merged


def build_profile(world, tsp, offset, total, road_key):
    """整条车道的高度剖面，**全程算一次**，打空的位置在全局范围内插值补上。

    为什么必须是全程而不是按段：build_points 对每个 Lane_ 段和每个 Inter_ 段
    分别调用，各自在段内插值。打空区域一旦跨在两段交界上，
    Inter_ 的尾部从它段内最后一个有效点平推过去、Lane_ 的头部从它段内第一个
    有效点平推回来——两个锚点不同，两段各自平滑，接缝处却对不上，
    表现就是相邻两段之间一个巨大的高低落差
    （实测 Inter_形状13_-0675_I03 和 Lane_形状13_-0675_S04 之间）。
    全程剖面让所有段共用同一批锚点，接缝连续是构造保证的。

    返回 (距离数组, 高度数组, 有没有任何有效命中)。
    """
    key = (road_key, round(offset, 1))
    if key in _profiles:
        return _profiles[key]

    # 先把每个站点的**所有**路面层都采下来，暂不决定用哪层
    ds, cand, fallback = [], [], []
    d = 0.0
    while True:
        dd = min(d, total)
        loc = tsp.get_location_at_distance_along_spline(dd, WS)
        hx, hy = horiz_right(tsp, dd)
        layers = surface_layers(world, loc.x + hx * offset,
                                loc.y + hy * offset, loc.z)
        ds.append(dd)
        cand.append(layers)
        fallback.append(loc.z)
        if dd >= total:
            break
        d += PROFILE_STEP

    n = len(ds)
    multi = sum(1 for c in cand if len(c) > 1)

    # 链式选层：从候选最少的站点起锚，向两头传播，每步挑"离上一站最近"的那层。
    # 不能各站独立挑最上面的——路口处上面那层属于交叉路，独立挑就会在路口
    # 把车道拽上去、出路口再掉下来，接缝出现两百多厘米的台阶。
    chosen = [None] * n
    anchor = None
    for i, c in enumerate(cand):
        if len(c) == 1:
            anchor = i
            break
    if anchor is None:
        anchor = next((i for i, c in enumerate(cand) if c), None)
    if anchor is not None:
        chosen[anchor] = cand[anchor][0]
        prev = chosen[anchor]
        for i in range(anchor + 1, n):
            chosen[i] = pick_layer(cand[i], prev)
            if chosen[i] is not None:
                prev = chosen[i]
        prev = chosen[anchor]
        for i in range(anchor - 1, -1, -1):
            chosen[i] = pick_layer(cand[i], prev)
            if chosen[i] is not None:
                prev = chosen[i]

    zs, oks = [], []
    for i in range(n):
        if chosen[i] is None:
            zs.append(fallback[i])
            oks.append(False)
        else:
            zs.append(chosen[i] + Z_OFFSET)
            oks.append(True)

    n = len(zs)
    for i in range(n):
        if oks[i]:
            continue
        lo = next((j for j in range(i - 1, -1, -1) if oks[j]), None)
        hi = next((j for j in range(i + 1, n) if oks[j]), None)
        if lo is None and hi is None:
            continue
        if lo is None:
            zs[i] = zs[hi]
        elif hi is None:
            zs[i] = zs[lo]
        else:
            f = (ds[i] - ds[lo]) / max(1e-6, ds[hi] - ds[lo])
            zs[i] = zs[lo] + (zs[hi] - zs[lo]) * f

    prof = (ds, zs, any(oks), sum(1 for o in oks if not o), n, multi)
    _profiles[key] = prof
    return prof


def profile_z(prof, d):
    """在剖面上取某个沿线距离的高度，站点之间线性插值。"""
    ds, zs, valid = prof[0], prof[1], prof[2]
    if not valid or not ds:
        return None
    if d <= ds[0]:
        return zs[0]
    if d >= ds[-1]:
        return zs[-1]
    lo, hi = 0, len(ds) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if ds[mid] <= d:
            lo = mid
        else:
            hi = mid
    span = ds[hi] - ds[lo]
    if span <= 1e-6:
        return zs[lo]
    return zs[lo] + (zs[hi] - zs[lo]) * (d - ds[lo]) / span


def lane_point(world, tsp, offset, dist, prof=None):
    """某个沿线距离处、该车道的点。横向用右向量水平分量，高度打射线重测。

    多层时挑哪一层，以全程剖面在该处的高度为准（挑最接近的），
    不是挑最上面的——路口处上面那层是交叉路的面。

    射线打空时的兜底顺序：
      1. 全程剖面插值（和相邻段共用锚点，接缝连续）
      2. 源样条 Z —— 只在整条车道一个点都没命中时才会走到，
         实测源样条与真实路面偏差可达 +-500cm，属于最后手段。
    """
    loc = tsp.get_location_at_distance_along_spline(dist, WS)
    right = tsp.get_right_vector_at_distance_along_spline(dist, WS)
    hx, hy = right.x, right.y
    h = (hx * hx + hy * hy) ** 0.5
    if h > 1e-4:
        hx, hy = hx / h, hy / h
    px, py = loc.x + hx * offset, loc.y + hy * offset

    zp = profile_z(prof, dist) if prof is not None else None
    layers = surface_layers(world, px, py, loc.z)
    # 剖面给的是"加过 Z_OFFSET 的点高度"，层是裸路面高度，比之前先减回去
    expect = (zp - Z_OFFSET) if zp is not None else None
    pz = pick_layer(layers, expect)
    if pz is not None:
        who = "road" if len(layers) == 1 else "road/多层%d" % len(layers)
        return unreal.Vector(px, py, pz + Z_OFFSET), True, who
    if zp is not None:
        return unreal.Vector(px, py, zp), False, "profile"
    return unreal.Vector(px, py, loc.z + right.z * offset), False, "spline:no-hit"


def bridge_fn(bounds, e0, e1):
    """路口段 [e0,e1] 的高度函数：在两侧车道段端点之间线性过渡。

    过渡区间是 上一段末端 -> 下一段起点 的完整跨度（含两侧空隙），
    不是路口段本身——这样进出两个接缝同时连续。
    两侧任一缺失就返回 None，调用方退回实测。
    """
    before = [(d_end, z_end) for _d0, _z0, d_end, z_end in bounds
              if d_end <= e0 + 1.0]
    after = [(d0, z0) for d0, z0, _d1, _z1 in bounds if d0 >= e1 - 1.0]
    if not before or not after:
        return None
    da, za = max(before, key=lambda t: t[0])     # 最靠近 e0 的那段末端
    db, zb = min(after, key=lambda t: t[0])      # 最靠近 e1 的那段起点
    span = db - da
    if span <= 1e-3:
        return None

    def f(d):
        t = (d - da) / span
        t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
        return za + (zb - za) * t
    return f


def grow(d_from, d_to, amount, total):
    """把区间两端各往外延 amount（夹在 [0, total] 内）。"""
    if amount <= 0:
        return d_from, d_to
    return max(0.0, d_from - amount), min(total, d_to + amount)


def grow_end(d_from, d_to, amount, forward, total):
    """只延行驶方向的末端，起点不动。

    顺行时末端是 d_to；逆行段生成后会 reverse，它的末端在 d_from 那一侧。
    """
    if amount <= 0:
        return d_from, d_to
    if forward:
        return d_from, min(total, d_to + amount)
    return max(0.0, d_from - amount), d_to


def build_points(world, tsp, offset, d_from, d_to, prof=None, z_fn=None):
    """采一段车道的点。z_fn 非空时高度由它决定，不打射线（见 BRIDGE_INTERSECTIONS）。"""
    pts, oks, whos, missed, kinds = [], [], [], 0, {}
    dev = 0.0      # 桥接高度与实测路面的最大偏差，只在 z_fn 非空时有意义
    # 用 while True + 末尾 break，保证区间终点一定被采到。
    # 写成 while d <= d_to 时，d 一旦跨过 d_to 就直接退出，
    # min(d, d_to) 的夹取永远轮不到执行，每段末尾会丢掉最多一个 SAMPLE_STEP
    # （实测段间距 501cm 而不是设定的 50cm，就是这么来的）。
    d = d_from
    while True:
        dd = min(d, d_to)
        p, ok, who = lane_point(world, tsp, offset, dd, prof)
        if z_fn is not None:
            zb = z_fn(dd)
            if ok:      # 实测到了路面，记下桥接线偏离它多远
                dev = max(dev, abs(p.z - zb))
            p = unreal.Vector(p.x, p.y, zb)
            ok, who = True, "bridge"
        pts.append(p)
        oks.append(ok)
        whos.append(who)
        kinds[who] = kinds.get(who, 0) + 1
        if not ok:
            missed += 1
        if dd >= d_to:
            break
        d += SAMPLE_STEP

    # 段内插值只兜底"连全程剖面都没救回来"的点（who 以 spline: 开头）。
    # who == "profile" 的点已经由全程剖面给过高度了，不能再在段内重算——
    # 段内锚点和相邻段不是同一批，那正是接缝落差的来源。
    n = len(pts)
    for i in range(n):
        if oks[i] or not whos[i].startswith("spline"):
            continue
        lo = next((j for j in range(i - 1, -1, -1) if oks[j]), None)
        hi = next((j for j in range(i + 1, n) if oks[j]), None)
        if lo is None and hi is None:
            continue                      # 整段都没命中，只能保留原值

        if lo is None:
            z = pts[hi].z
        elif hi is None:
            z = pts[lo].z
        else:
            f = float(i - lo) / (hi - lo)
            z = pts[lo].z + (pts[hi].z - pts[lo].z) * f
        pts[i] = unreal.Vector(pts[i].x, pts[i].y, z)
    return pts, missed, kinds, dev


def mark_edited(sp):
    """给样条打"已被编辑"标记，否则构造脚本一重跑就把点冲回蓝图默认的 2 个点。

    蓝图定义的组件，实例上的改动默认不保留；UE 靠 bSplineHasBeenEdited
    （编辑器里显示为 "Override Construction Script"）决定要不要在
    构造脚本之后把实例的样条数据重新应用回去。手动拖点时 UE 自动打这个标记，
    从脚本写点必须自己设。
    """
    for name in ("spline_has_been_edited", "b_spline_has_been_edited"):
        try:
            sp.set_editor_property(name, True)
            return True
        except Exception:
            continue
    return False


def spawn_lane(eas, cls, pts, label, forward, tag, box_half=None):
    if len(pts) < 2:
        return None
    if not forward:
        pts = list(reversed(pts))
    a = eas.spawn_actor_from_class(cls, pts[0])
    a.set_actor_label(label)
    a.tags = [tag]
    sp = a.get_component_by_class(unreal.SplineComponent)
    sp.clear_spline_points(False)
    for p in pts:
        sp.add_spline_point(p, WS, False)
    for i in range(sp.get_number_of_spline_points()):
        sp.set_spline_point_type(i, unreal.SplinePointType.CURVE_CLAMPED, False)
    sp.update_spline()
    if not mark_edited(sp):
        w("!! %s 的 spline_has_been_edited 设置失败，双击后点会被构造脚本冲掉" % label)
    scale_box(a, label, box_half)
    return a


_light_prop = None       # LightNumber 在 Python 侧的确切属性名，首次用时定下来


def light_prop_name(a):
    """找出 LightNumber 的确切属性名。写法可能是 LightNumber/lightnumber/…"""
    global _light_prop
    if _light_prop is not None:
        return _light_prop or None
    cands = []
    try:
        bp = unreal.EditorAssetLibrary.load_asset(CHILD_BP)
        names = [str(n) for n in
                 unreal.BlueprintEditorLibrary.list_member_variable_names(bp)]
        cands = [n for n in names if "light" in n.lower() and "num" in n.lower()]
    except Exception:
        pass
    cands += ["LightNumber", "lightnumber", "light_number", "LightNum"]
    for n in cands:
        try:
            a.get_editor_property(n)
            _light_prop = n
            w("  LightNumber 属性名 = '%s'" % n)
            return n
        except Exception:
            continue
    _light_prop = ""
    w("  !! 找不到 LightNumber 属性（试过 %s），跳过分组" % ", ".join(cands[:4]))
    return None


def set_light_number(a, pts, label):
    """路口段按走向分灯组：沿 Y（左右走向）设 LIGHT_NUMBER_Y，沿 X 不动。"""
    if not LIGHT_NUMBER_BY_AXIS or len(pts) < 2:
        return
    if abs(pts[-1].y - pts[0].y) <= abs(pts[-1].x - pts[0].x):
        return                      # 沿 X，保持原值
    prop = light_prop_name(a)
    if prop is None:
        return
    try:
        a.modify(True)
        a.set_editor_property(prop, LIGHT_NUMBER_Y)
    except Exception as exc:
        w("  !! %s 设 LightNumber 失败: %s" % (label, str(exc)[:60]))


def scale_box(a, label, half):
    """把这个 actor 上的 Box 设成指定的半尺寸（绝对值，不是倍率，重复跑幂等）。"""
    if half is None:
        return
    for c in a.get_components_by_class(unreal.BoxComponent):
        try:
            c.modify(True)
            c.set_editor_property("box_extent",
                                  unreal.Vector(half[0], half[1], half[2]))
        except Exception as exc:
            w("!! %s 的 Box 设尺寸失败: %s" % (label, str(exc)[:60]))
        break


def process_road(eas, world, all_splines, road_label, tag, road_actor,
                 actor_by_label):
    """给一条路生成全部车道（路段 + 路口段）。返回统计字典。"""
    target = None
    others = []
    for name, sp in all_splines:
        if name.strip() == road_label:
            target = sp
        else:
            others.append((name, sp))
    if target is None:
        w("!! 找不到 %s，跳过" % road_label)
        return None

    tsp = target
    total = tsp.get_spline_length()
    w("")
    w("=" * 60)
    w("主路 %s   总长 %.0f cm   对照道路 %d 条" % (road_label, total, len(others)))
    w("=" * 60)
    roads = others

    inters = find_intersections(tsp, roads, total)
    w("检测到路口 %d 处（XY<%.0f 且 Z差<%.0f，相距<%.0f 的已合并）"
      % (len(inters), XY_CROSS, Z_CROSS, MERGE_DIST))
    ihalf_preview = [inter_half_for(nm, actor_by_label) for _c, nm in inters]
    for k, (c, name) in enumerate(inters):
        w("   沿线 %8.0f   %-18s 路口段半长 %.0f  截断半径 %.0f"
          % (c, name, ihalf_preview[k],
             ihalf_preview[k] + SEG_EXTEND + TARGET_GAP))

    # 每个路口各自的 INTER_HALF / BREAK_HALF
    ihalf = [inter_half_for(nm, actor_by_label) for _c, nm in inters]
    bhalf = [h + SEG_EXTEND + TARGET_GAP for h in ihalf]

    segs = []
    cur = 0.0
    for k, (c, _) in enumerate(inters):
        a0, a1 = c - bhalf[k], c + bhalf[k]
        if a0 - cur > MIN_SEG_LEN:
            segs.append((cur, a0))
        cur = max(cur, a1)
    if total - cur > MIN_SEG_LEN:
        segs.append((cur, total))
    w("切出路段 %d 段，路口段 %d 处" % (len(segs), len(inters)))
    w("")

    is_main, road_width, lane_count = classify(road_actor)
    lane_w = road_width / lane_count
    override = MAIN_ROAD_OFFSETS if is_main else SIDE_ROAD_OFFSETS
    if override:
        offsets = list(override)
        how = "显式指定"
    else:
        offsets = [(i - (lane_count - 1) / 2.0) * lane_w
                   for i in range(lane_count)]
        how = "按 %.0f/%d 均分，单车道 %.0f" % (road_width, lane_count, lane_w)
    w("分类: %s（文件夹 %r）-> 标称 %.0f 宽，%d 车道，横向位置%s"
      % ("主路" if is_main else "次路",
         str(road_actor.get_folder_path()), road_width, len(offsets), how))
    lane_cls = unreal.EditorAssetLibrary.load_asset(LANE_BP).generated_class()
    child_cls = unreal.EditorAssetLibrary.load_asset(CHILD_BP).generated_class()
    w("通行 %s   偏移 %s"
      % ("靠左" if LEFT_HAND_TRAFFIC else "靠右", ["%.0f" % o for o in offsets]))
    w("")

    rid = road_label.replace(" ", "")
    total_missed = 0
    all_kinds = {}
    n_seg = n_int = 0

    bridge_dev = []
    for offset in offsets:
        forward = (offset < 0) if LEFT_HAND_TRAFFIC else (offset > 0)
        otag = "%+05d" % int(offset)

        # 全程高度剖面：整条车道算一次，所有段共用，保证接缝处高度连续
        prof = build_profile(world, tsp, offset, total, road_label)
        if prof[3] or prof[5]:
            w("     剖面站点 %d 个：打空 %d（全局插值补上），多层路面 %d（按连续性选层）"
              % (prof[4], prof[3], prof[5]))

        # 记下每段车道在**距离空间**里的两端和端点高度，给路口段桥接用。
        # 用距离而不是"前一段/后一段"的序号：逆行车道点序是倒的，
        # 但 build_points 始终按距离递增采样，距离空间里前后关系是唯一的。
        bounds = []
        for i, (d0, d1) in enumerate(segs):
            e0, e1 = grow(d0, d1, SEG_EXTEND, total)
            pts, miss, kinds, _dev = build_points(world, tsp, offset,
                                                  e0, e1, prof)
            total_missed += miss
            for k, v in kinds.items():
                all_kinds[k] = all_kinds.get(k, 0) + v
            if pts:
                bounds.append((e0, pts[0].z, e1, pts[-1].z))
            if spawn_lane(eas, lane_cls, pts,
                          "Lane_%s_%s_S%02d" % (rid, otag, i), forward, tag,
                          LANE_BOX_HALF):
                n_seg += 1

        n_bridged = 0
        for i, (c, _) in enumerate(inters):
            e0, e1 = grow_end(c - ihalf[i], c + ihalf[i],
                              INTER_END_EXTEND, forward, total)
            z_fn = bridge_fn(bounds, e0, e1) if BRIDGE_INTERSECTIONS else None
            if z_fn is not None:
                n_bridged += 1
            pts, miss, kinds, dev = build_points(world, tsp, offset, e0, e1,
                                                 prof, z_fn)
            if z_fn is not None and dev > BRIDGE_DEV_WARN:
                bridge_dev.append((dev, "I%02d %s" % (i, otag)))
            total_missed += miss
            for k, v in kinds.items():
                all_kinds[k] = all_kinds.get(k, 0) + v
            ilabel = "Inter_%s_%s_I%02d" % (rid, otag, i)
            ia = spawn_lane(eas, child_cls, pts, ilabel, forward, tag,
                            INTER_BOX_HALF)
            if ia:
                set_light_number(ia, pts, ilabel)
                n_int += 1
        if BRIDGE_INTERSECTIONS and n_bridged < len(inters):
            w("     路口段桥接 %d/%d（其余两侧缺段，退回实测）"
              % (n_bridged, len(inters)))

        w("  偏移 %+6.0f  %s  路段 %d + 路口 %d"
          % (offset, "顺行" if forward else "逆行", len(segs), len(inters)))

    w("小计: 路段 %d + 路口段 %d = %d 个 actor，射线打空 %d 点"
      % (n_seg, n_int, n_seg + n_int, total_missed))
    if bridge_dev:
        bridge_dev.sort(reverse=True)
        w("  桥接高度与路口内实测路面偏差超过 %.0fcm 的路口段 %d 个（车在路口里会"
          "浮起/陷入这么多）:" % (BRIDGE_DEV_WARN, len(bridge_dev)))
        for d, nm in bridge_dev[:6]:
            w("     %7.0f cm   %s" % (d, nm))
        w("  偏差大说明那个路口两条路的路面本来就对不上——这是关卡几何的问题，")
        w("  桥接只保证车开得顺，不能把两条路真的接平。")
    return {"road": road_label, "seg": n_seg, "int": n_int,
            "missed": total_missed, "kinds": all_kinds,
            "inters": len(inters), "segs": len(segs)}


def resolve_roads(all_splines):
    """要处理哪些路。CROSSING_WITH 非空就自动算，否则用手写的 ROAD_LABELS。"""
    if not CROSSING_WITH:
        return list(ROAD_LABELS)

    target, others = None, []
    for name, sp in all_splines:
        if name.strip() == CROSSING_WITH.strip():
            target = sp
        else:
            others.append((name, sp))
    if target is None:
        w("!! 自动模式找不到 %s，退回手写名单 %s" % (CROSSING_WITH, ROAD_LABELS))
        return list(ROAD_LABELS)

    inters = find_intersections(target, others, target.get_spline_length())
    names = []
    for _c, nm in inters:
        # 合并过的路口名字形如 "形状 43+形状 44"，拆开
        for part in str(nm).split("+"):
            part = part.strip()
            if part and part not in names:
                names.append(part)
    roads = [CROSSING_WITH.strip()] + names
    w("自动模式：与 %s 相交的路 %d 条 -> 本次共处理 %d 条"
      % (CROSSING_WITH, len(names), len(roads)))
    w("   %s" % "、".join(roads))
    return roads


def drop_excluded(roads):
    ex = set(r.strip() for r in EXCLUDE_ROADS)
    kept = [r for r in roads if r.strip() not in ex]
    gone = [r for r in roads if r.strip() in ex]
    if gone:
        w("排除不铺车道的路 %d 条：%s" % (len(gone), "、".join(gone)))
    return kept


def run():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    actors = eas.get_all_level_actors()

    # 道路样条一次性收集好，各条路互为对照。
    # 必须在删除之前做：自动模式要先靠这些样条算出本次处理哪几条路，
    # 才知道该删谁的旧 actor。
    all_splines = []
    actor_by_label = {}
    for a in actors:
        if a.get_class().get_name() == "BP_PS2DEMSplineActor_v3_C":
            sp = a.get_component_by_class(unreal.SplineComponent)
            if sp:
                all_splines.append((a.get_actor_label(), sp))
                actor_by_label[a.get_actor_label()] = a
    w("关卡道路样条共 %d 条" % len(all_splines))

    roads = drop_excluded(resolve_roads(all_splines))

    # 只清掉本次要处理的那几条路的旧 actor（外加早期版本的裸 tag），别误删别的路
    targets = set("%s:%s" % (TAG_PREFIX, r) for r in roads)
    targets.add(TAG_PREFIX)
    removed = 0
    for a in actors:
        if targets & set(str(t) for t in a.tags):
            eas.destroy_actor(a)
            removed += 1
    w("清除旧 actor %d 个（仅限 %s）" % (removed, "、".join(roads)))
    w("")

    stats = []
    for road_label in roads:
        st = process_road(eas, world, all_splines, road_label,
                          "%s:%s" % (TAG_PREFIX, road_label),
                          actor_by_label.get(road_label), actor_by_label)
        if st:
            stats.append(st)

    w("")
    w("=" * 60)
    w("总计")
    w("=" * 60)
    grand = {}
    tot_seg = tot_int = tot_miss = 0
    for st in stats:
        w("  %-10s 路口 %d 处，路段 %d 段 -> actor %d 个，打空 %d 点"
          % (st["road"], st["inters"], st["segs"], st["seg"] + st["int"], st["missed"]))
        tot_seg += st["seg"]
        tot_int += st["int"]
        tot_miss += st["missed"]
        for k, v in st["kinds"].items():
            grand[k] = grand.get(k, 0) + v
    w("  合计 路段 %d + 路口段 %d = %d 个 actor，打空 %d 点"
      % (tot_seg, tot_int, tot_seg + tot_int, tot_miss))
    w("  命中对象分布（出现建筑名 = 有点落在屋顶）:")
    for k in sorted(grand, key=lambda x: -grand[x]):
        w("     %-32s %d" % (k, grand[k]))
    w("")
    w("入口侧间隙 %.0f cm，出口侧 %.0f cm（路口段末端延 %.0f）；"
      % (TARGET_GAP, TARGET_GAP - INTER_END_EXTEND, INTER_END_EXTEND))
    w("路口段半长按交叉路宽度逐个决定，")
    w("Box 落在距路口中心 INTER_HALF 处，始终在重叠区内（留 %.0f cm 余量）。"
      % BOX_MARGIN)
    w("完成。关卡尚未保存。")

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


try:
    run()
except Exception:
    err = traceback.format_exc()
    unreal.log_error("[gen_lanes] fail:" + chr(10) + err)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("fail:\n" + err)
