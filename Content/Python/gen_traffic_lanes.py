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

# ！！**不要改成 True。关卡里有大量手改的交通线（用户 2026-10-29 明确说明）。**
# `ALL_ROADS = True` 会把全图 38 条路的车道/路口段全部删掉重建，
# **所有手动调整都会消失**，而且没有任何脚本能复现它们。
#
# 曾经一度改成 True，理由是"重建会丢的东西都能被脚本复现"——那张核对表
# 只核对了脚本产物（LightNumber、转弯样条、Z 偏移、点型），
# **漏掉了人手改过的部分**，是错的。
#
# 现在是**单路模式**：只重建 ROAD_LABELS 里列的路。
# 即使这样，**那几条路上的手改也会丢**——跑之前先确认那几条没有手改内容。
# 不想重建又要修某些问题时，用就地修的脚本：
#   · 折线样     → smooth_lane_tangents.py（只改点型，控制点一个不动）
#   · 废路口     → drop_intersection.py（解除红灯 + 删盒子，但保留 150cm 接缝）
#   · 端点切线零 → fix_endpoint_tangents.py（就地改点型，不重建 actor）
ALL_ROADS = False         # True = 关卡里所有道路样条全铺，忽略 CROSSING_WITH。
                          # 只按 EXCLUDE_ROADS 剔除。河（River/BranchRiver）不是路，
                          # 必须排除，否则会沿着河铺出一条车道来。
CROSSING_WITH = ""        # 非空时自动算出要处理的路：这条路 + 所有与它相交的路。
                          # 用的是和切路口同一套 find_intersections，
                          # 以后改样条也不会过期，不用手动维护名单。
                          # 置成 "" 就退回下面的手写名单。
                          # **这次刻意置空**：自动模式会把"所有与 R003 相交的路"
                          # 一起重建，那些路的 LightNumber 会全部回到默认值，
                          # 改动面比需要的大得多。
ROAD_LABELS = ["MainRoad_R003",         # CROSSING_WITH 为空时才用这个
               "MainRoad_R010",
               "MainRoad_R008"]        # R003: 不再和 R020 交叉；
                                       # R010: 不再和 R004/R005 交叉
                                       #       （两条都要重建才能在那些位置连成直行线）；
                                       # R008: 修 Inter_MainRoad_R008_+0540_I00
                                       #       首尾控制点重合（轴角读不到、
                                       #       自动切线为零）。相邻重合点去重
                                       #       （阈值 2cm）的代码早就在了，
                                       #       但只在生成时生效，必须重建这条路。
EXTRA_ROADS = []                       # 无论自动模式算没算到，都追加处理这几条。
                                      # 自动模式只取"与 形状13 相交的路"，
                                      # 离得远的新路（形状40）永远进不来。
                                      # 加在这里比把 CROSSING_WITH 置空、
                                      # 退回全手写名单好：自动那部分仍然自动。
EXCLUDE_ROADS = ["形状 43",            # 源样条是坏的：2 个点、首尾同坐标、长度却有 1597
                 "River", "BranchRiver",   # 河，不是路
                 "BranchRoad_R020",       # 2026-10-29：用户手删了它的交通线，
                 "MainRoad_R004",          # 这里也排掉，否则重新生成会再铺一遍
                 "MainRoad_R005",          # R004/R005 对应已废弃的 Intersection_39
                 "BranchRoad_R031"]        # 2026-10-29 export_manual_edits.py 查出来的
                                           # 第四条装饰路（一条交通线都没有）
NO_JUNCTION_WITH = ["River", "BranchRiver",
                    "BranchRoad_R020",     # 它和 MainRoad_R003 的交叉点
                    "MainRoad_R004",       # R004/R005 和 MainRoad_R010 的交叉点
                    "MainRoad_R005",       # （Intersection_39）也不再当路口，
                    "BranchRoad_R031"]     # R031 同理
                                           # （约 3826, 18806）不再当路口，
                                           # 对应已删掉的 Intersection_15。
                                           # **光加进 EXCLUDE_ROADS 不够**：
                                           # 被排除的样条仍然参与相交检测，
                                           # R003 照样会在那里被切断、长出
                                           # Inter_ 段，gen_intersections 再给它
                                           # 配一个盒子，等于白删。
# **不参与路口判定**的样条。和 EXCLUDE_ROADS 是两件事：
# EXCLUDE_ROADS 只管"不给谁铺车道"，被排除的样条**仍然参与相交检测**，
# 于是路过河的地方被当成路口切断了（实测 形状16、形状12 都中招），
# 车开到桥上反而断线。河上是桥，不是路口，车该直接开过去。
                                      # 这几条路不铺车道，自动名单里也剔掉。
                                      # 光在关卡里删 actor 不够——自动模式下次
                                      # 会把它们重新算进来，必须在这里也排除。
LANE_BP = "/Game/PS2DEM/BP_TrafficLine1"
CHILD_BP = "/Game/PS2DEM/BP_TrafficLine1_IntersectionChild"
TAG_PREFIX = "ClaudeGenLane"   # 实际 tag = "ClaudeGenLane:<路名>"，按路独立，互不误删

# **道路样条按大纲文件夹找，不按类名找**（2026-10-21 改）。
# 原来写的是 `get_class().get_name() == "BP_PS2DEMSplineActor_v3_C"`，
# 而队友把 landscape spline 逆向同步成蓝图 spline actor 之后
# （348e00b），道路变成了 45 个**裸 AActor**（类名就是 "Actor"）
# 各带一个 USplineComponent，那条类名判断只剩两条河能匹配。
# 实测后果：报告写「关卡道路样条共 2 条、合计 0 个 actor」——
# **一条车道都没生成，而且不报错**，只有去读报告才看得出来。
# 按类名找在这里本来就不可靠（裸 Actor 关卡里一大堆），
# 文件夹才是队友维护的那个事实。
ROAD_FOLDERS = ["PS2DEM/Splines/MainRoad", "PS2DEM/Splines/BranchRoad"]
SKIP_PART_SEGMENTS = True   # 跳过 _PartNN 分段，只认 merge_part_roads.py 合出来的
                            # 「<基路名>_Merged」。两边都收会把同一条路铺两遍。
_skipped_parts = []         # 被跳过的分段，报告里点名
# 顺带一个好处：河不在这两个文件夹里，于是**压根不进 all_splines**，
# 既不会被铺车道、也不会参与路口判定，所以下面 EXCLUDE_ROADS /
# NO_JUNCTION_WITH 里那两条河现在是空操作（留着无害，换图可能还用得上）。

# 主次路按**大纲文件夹**判定，不靠实测宽度猜——
# 实测会被路口、裙边、压平的地形干扰（形状13/39 都被测成上限 1000），
# 而文件夹是人工整理的，确定且可控。
# 新路径 PS2DEM/Splines/MainRoad 天然命中这个子串，这一条不用改。
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
# 车道点比路面高出多少。**2026-09-24 从 15 降到 2**，同时用 lower_traffic_z.py
# 把已经生成好的车道/路口 actor（连同 Box 和样条）整体下移了 13。
# 为什么原来是 15：没有记录——这是本文件里唯一一个没写理由的常量，
# 从用法反推最可能只是为了让样条在编辑器里不被路面遮住。
# 为什么要降：车的运行时高度**直接取自样条**（BP_car_base 沿样条 SetActorLocation），
# 所以这个偏移会一比一变成车的浮空。留 2 而不是 0，是给路面起伏和 z-fighting 留余量。
# 注意：这个值和已经生成的车道必须一致，改它就得同步处理存量车道，否则两者会差一截。
Z_OFFSET = 2.0
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
XY_CROSS = 400.0         # 最近 XY 小于此值才算真的相交。
                         # **200 → 400（2026-10-21）**，依据是实测、不是手感：
                         # 队友把道路从 形状xx 换成 landscape spline 同步出来的
                         # MainRoad_*/BranchRoad_* 之后，新样条**在路口处彼此差
                         # 244~360cm 没碰上**（旧样条真路口都 <120，所以旧值 200 够用）。
                         # 拿改路前的 31 个路口当标答对标，200 时判出 27/31，
                         # 漏掉的 4 个缺口实测 244 / 272 / 351 / 360。
                         # 选 400 两头都有余量：往下盖住最大的 360（余 40），
                         # 往上低于**最小的兄弟段缺口 629**（余 229）——
                         # 兄弟段是同一条路被切开的两段（_PartNN），
                         # 阈值一旦超过它们，每个切点都会多出一个假路口。
                         # 距离分布是连续的（…185、199、200、210、244…342、351…），
                         # 没有干净的数值空隙可卡，所以**不能再往上随手调**；
                         # 真要调先跑 diag_junction_miss.py 重新量这两头。
# ---- 端点规则（2026-10-26）----
# 现象：很多 T 形路口判不出来。原因是上面那条判据问的是「两条路的中线靠多近」，
# 而 T 形接头的几何是**支路停在主路外面**——队友把长样条在路口处切断、手工补路
# 之后，支路端点到主路中线常常差 600~1200cm，远超 XY_CROSS(400)。
#
# 为什么不直接调大 XY_CROSS：那是全局阈值，一调大，所有**并行经过**的路
# 都会被判成路口，凭空多出一堆假路口（表现是"车在直路中间莫名停下等红灯"，
# 极难往这个方向想）。端点规则是**定向放宽**：只有当"近点落在某条路的端点上"
# 时才用这个更大的阈值。并行经过的两条路在那个位置上没有端点，不受影响。
ENDPOINT_CROSS = 900.0   # 端点到对方中线的最大缺口。比 XY_CROSS 宽得多，
                         # 但只在端点处生效，所以不会把并行的路判成路口。
                         # **900 是从实测分布里挑的，不是手感**
                         # （preview_endpoint_junctions.py，2026-10-26）：
                         #   400~500: 38 处   500~600: 26   600~700: 20
                         #   700~800:  9      800~900:  6
                         #   900~1000: **0 处**  ← 空隙在这儿
                         #   1000~1100: 2     1100~1200: 1   最大 1188
                         # 900 以下 99 处、1000 以上只有 3 处，中间整档为空，
                         # 所以卡 900 两边都有余量。被切掉的那 3 处缺口 10~12 米，
                         # 基本可以断定是"路从旁边经过"而不是接头。
                         # 要再调先重跑那个预览看分布，别直接改数字。
ENDPOINT_TOL = 300.0     # 离样条首/末控制点多近才算"在端点上"
#
# 四种几何下的行为（第三条是必须排除的那个）：
#   1. 十字交叉（两条中线真的穿过）—— 老规则 XY_CROSS 就命中，端点规则不参与
#   2. T 形（支路端点 ↔ 主路中段）—— **要的就是这个**。
#      走主路时："对方的端点离我很近" → 命中；
#      走支路时："我正站在自己的端点上，而对方从旁边过" → 命中。
#      两边都命中才能两条路各自切出 Inter_ 段、各自有灯。
#   3. 首尾相接的续接（A 的端点 ↔ B 的端点）—— **不是路口**，是同一条路线的延续。
#      所以两条子规则都要求"另一头不是端点"：走主路时我自己不能站在端点上，
#      走支路时对方的近点不能也是端点。不排除的话，每一处接驳都会多出一个
#      假路口，而那恰恰是路最连贯的地方。
#   4. 并行经过 —— 两个位置上都没有端点，两条子规则都不触发，行为完全不变。

# 强制路口：某个**具体位置**上，就算两条路没真正相交也当成路口（T 形接头）。
# 队友在路口处切断长样条、再手工补路之后，两条路可能只是"凑得很近"，
# XY_CROSS=200 够不着。
#
# 必须连位置一起指定，不能只写路名：形状1 长 2.19 公里，只按路名放宽阈值的话，
# 这两条路在**任何**地方靠近 15 米都会被判成路口，凭空多出一堆假路口。
# 格式：(路A, 路B, 世界X, 世界Y, 生效半径, 该点使用的 XY 阈值)
FORCE_JUNCTIONS = [
    ("形状 13", "形状 1", 3094.0, -17726.0, 3000.0, 1500.0),
]
# 运行时自动追加的强制路口（见 build_part_junctions）。手写那条保持不变。
AUTO_FORCE_JUNCTIONS = []

# ---- 被切成 _PartNN 的路，在切点处自动补强制路口 ----
# 队友把 5 条支路在路口处切成了多段（R024/R035/R036/R038/R039）。
# **名字带 _PartNN 就意味着这条路在那儿被切开了，而被切开就意味着那儿有路口**——
# 这是队友的命名约定给出的结构事实，比靠距离阈值猜可靠得多。
#
# 为什么不直接把 XY_CROSS 调大：实测横穿路到被切两段端点的距离最大 1107cm
# （R024_Part01 × BranchRoad_R023），要盖住就得把全局阈值放到 1200；
# 而不同逻辑路之间的最近接近距离分布是**连续的**
# （…185、199、200、210、244…342、351…），没有空隙可卡，
# 一刀放到 1200 必然把并行的两条路判成路口，表现是
# 「车在直路中间莫名停下等红灯」，而且极难往这个方向查。
# 所以只在**切点这个具体位置**放宽，别的地方仍按 XY_CROSS。
PART_JUNCTION_NEAR = 1200.0    # 横穿路离切点中心多近才认为「这条路穿过这个切点」
                               # 实测 7 个真切点里横穿路离中点 4~630cm，
                               # 1200 足够宽；再大会把路口旁边平行经过的路也算进来。
PART_JUNCTION_RADIUS = 2500.0  # 规则生效半径（采样点离切点中心多近才放宽）
PART_JUNCTION_LIMIT = 1300.0   # 放宽后的阈值。实测最大需要 1107，留 ~200 余量。
PART_INTER_HALF = 200.0        # **路口落在被切段的端点上时，路口段用这个短半长。**
                               # 默认半长按横穿路的半宽算（inter_half_for），
                               # 那个算式假设「路从路口中间穿过去」。
                               # 而被切开的段是**停在路口外面**的，端点到路口中心
                               # 实测还有 599~1107cm——这段距离本身已经在路口里了，
                               # 再按整条横穿路的半宽给一次等于重复计一遍：
                               # 每段 1150 + 中间物理缺口 1283~1677 + 另一段 1150
                               # ≈ 3600cm 的路口，而横穿的主路只有 1800 宽。
                               # 短半长让路口段贴着段的端点收尾，车道也能跑到端点附近。
PART_END_EPS = 400.0           # 路口中心离段的首/尾多近算「落在端点上」。
                               # 实测被切段的路口都正好判在端点处（最近点就是端点）。
# 生效半径 = 以那个世界坐标为圆心多大范围内放宽；半径外一律回到 XY_CROSS。
# 坐标取自改路之前 Intersection_28 的中心，队友改完路可能挪了位——
# 所以下面无论成不成都会把该范围内的实测最近距离打进报告，一轮就能校准。
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
BRIDGE_SURFACE_BLEND = 0.5
# **路口段高度：桥接线往实测路面靠多少**（0 = 纯桥接，1 = 完全贴地）。
# 原来是探到路面也直接丢掉、无条件用桥接高度，代价写在 BRIDGE_INTERSECTIONS
# 那条注释里：「路口里车可能浮起/陷入」。实测确实浮了——合并路的路口段
# 平均高差 +40 / −62、最大 76cm（diag_lane_fit 报的），而且全都在
# BRIDGE_DEV_WARN=80 的报警线以下，所以之前一声不响。
# 0.5 把浮/陷砍掉一半，同时保留一半的桥接平滑度。
# **为什么不设 1.0**：路口处两条路的路面是叠着的（实测差约 200cm），
# 完全贴地会让车道在层与层之间跳，那正是桥接要解决的问题——
# 文档记过「只取最上面那层会在接缝出现 250cm 台阶」。
BRIDGE_SURFACE_MAX_DIFF = 120.0
# 只有桥接线和实测路面差在这以内才往回靠。差得更多说明**那个路口两条路的
# 路面本来就对不上**（关卡几何问题，文档明说桥接治不了），
# 这时硬贴会把车道拽到另一层路面上去，比浮着更糟。
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


def inter_half_for(cross_name, actor_by_label, road_label=None,
                   at_dist=None, total=None):
    """按交叉道路的宽度决定这个路口的样条半长（同时也是 Box 的落点）。

    Box 在 actor 原点 = 样条第一个点，必须落在两路重叠区内，
    而重叠区沿本路方向的半宽 = 交叉路的半宽。
    合并路口（名字里有 '+'）取其中最窄的那条。

    **例外：被切成 _PartNN 的段，路口落在它端点上时用 PART_INTER_HALF。**
    那个「按横穿路半宽」的算式假设路从路口中间穿过去，而被切开的段停在
    路口外面，端点到路口中心那段距离本身已经在路口里了（见 PART_INTER_HALF）。
    判据是「路口中心落在本段首或尾 PART_END_EPS 以内」——
    被切段的路口实测都正好判在端点处，而正常穿过的路口落在样条中段。
    """
    if (road_label is not None and at_dist is not None and total is not None
            and part_index(road_label) is not None):
        if at_dist <= PART_END_EPS or (total - at_dist) <= PART_END_EPS:
            return PART_INTER_HALF

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


def base_road_name(label):
    """剥掉 _PartNN 后缀，得到逻辑路名。

    队友把几条路在路口处切成了多段（实测 5 条：R024/R035/R036/R038/R039，
    形如 BranchRoad_R035_Part01 / _Part02）。这些段**是同一条路**，
    彼此之间不该算路口——否则每个切点都会多出一个假路口，
    车开到那儿会以为要选路。

    现在这条排除其实是**安全网而不是必需品**：实测兄弟段之间的缺口
    最小 629cm，而 XY_CROSS 是 400，本来就判不成路口。
    但它让「以后谁想再调大 XY_CROSS」这件事变安全——
    一旦超过 629 就会在每个切点造出假路口，而那种错误的表现是
    「车在直路中间莫名停下等红灯」，极难往这个方向想。
    """
    return re.sub(r"_[Pp]art\d+$", "", str(label))


def road_spline_of(actor):
    """是道路样条就返回它的 SplineComponent，不是就返回 None。

    判据是**大纲文件夹**（ROAD_FOLDERS）+ 身上有 USplineComponent，
    不看类名——理由见文件顶部 ROAD_FOLDERS 的注释。
    """
    try:
        folder = str(actor.get_folder_path()).replace("\\", "/").lower()
    except Exception:
        return None
    if not any(f.lower() in folder for f in ROAD_FOLDERS):
        return None

    # **跳过带 _PartNN 的分段**：它们已经由 merge_part_roads.py 合并成
    # 「<基路名>_Merged」了，两边都收就会把同一条路铺两遍车道
    # （两套车道几乎重叠，车会随机选到其中一套，而且路口会被重复判定）。
    # 判据是名字——队友把一条路切开时就是用这个后缀标记的。
    # 没跑过合并脚本的话这里会把那 5 条路整条跳掉、报告里车道数明显少，
    # 所以下面会专门点名提醒。
    try:
        label = actor.get_actor_label()
    except Exception:
        label = ""
    if SKIP_PART_SEGMENTS and part_index(label) is not None:
        _skipped_parts.append(label)
        return None

    try:
        return actor.get_component_by_class(unreal.SplineComponent)
    except Exception:
        return None


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


_no_junction = set(n.strip() for n in NO_JUNCTION_WITH)


def forced_entry(a_name, b_name, p):
    """采样点 p 处，这一对路有没有命中某条强制路口规则。命中返回该条，否则 None。

    路名和位置都要对上——只对路名的话，长路上任何一处靠近都会被放行。
    """
    key = frozenset((str(a_name).strip(), str(b_name).strip()))
    # 手写的 + 切点处自动算出来的，查同一套逻辑
    for ra, rb, fx, fy, radius, lim in list(FORCE_JUNCTIONS) + list(AUTO_FORCE_JUNCTIONS):
        if frozenset((ra.strip(), rb.strip())) != key:
            continue
        if ((p.x - fx) ** 2 + (p.y - fy) ** 2) ** 0.5 <= radius:
            return (ra, rb, fx, fy, radius, lim)
    return None


def part_index(label):
    """取 _PartNN 里的那个编号；不是分段的返回 None。"""
    m = re.search(r"_[Pp]art(\d+)$", str(label))
    return int(m.group(1)) if m else None


def build_part_junctions(all_splines):
    """为被切成 _PartNN 的路，在切点处自动生成强制路口规则。

    只配**相邻编号**的两段（Part01-Part02、Part02-Part03）。
    不按距离判相邻：编号就在名字里，是确定的。
    按距离判会出错——实测 R036_Part01 和 _Part03 的两端相距 24915cm，
    它们的「中点」附近恰好有别的路经过，当成切点就会在空地上造一个假路口。

    切点位置取两段**最近的那一对端点**的中点。横穿路取离这个中点
    PART_JUNCTION_NEAR 以内的别的逻辑路——实测 7 个真切点里
    横穿路离中点只有 4~630cm，所以这个判据很稳。
    """
    del AUTO_FORCE_JUNCTIONS[:]
    by_base = {}
    for label, sp in all_splines:
        by_base.setdefault(base_road_name(label), []).append((label, sp))

    made = 0
    for base in sorted(by_base):
        parts = [(part_index(l), l, sp) for l, sp in by_base[base]]
        parts = [x for x in parts if x[0] is not None]
        if len(parts) < 2:
            continue
        parts.sort()
        for k in range(len(parts) - 1):
            ia, la, spa = parts[k]
            ib, lb, spb = parts[k + 1]
            # 最近的那一对端点 = 切点
            la_len = spa.get_spline_length()
            lb_len = spb.get_spline_length()
            ea = [spa.get_location_at_distance_along_spline(0.0, WS),
                  spa.get_location_at_distance_along_spline(la_len, WS)]
            eb = [spb.get_location_at_distance_along_spline(0.0, WS),
                  spb.get_location_at_distance_along_spline(lb_len, WS)]
            best = None
            for pa in ea:
                for pb in eb:
                    d = ((pa.x - pb.x) ** 2 + (pa.y - pb.y) ** 2) ** 0.5
                    if best is None or d < best[0]:
                        best = (d, pa, pb)
            gap, pa, pb = best
            mx, my = (pa.x + pb.x) / 2.0, (pa.y + pb.y) / 2.0

            # 谁穿过这个切点
            crossing = []
            for name, osp in all_splines:
                if base_road_name(name) == base:
                    continue
                q = osp.find_location_closest_to_world_location(
                    unreal.Vector(mx, my, (pa.z + pb.z) / 2.0), WS)
                dmid = ((q.x - mx) ** 2 + (q.y - my) ** 2) ** 0.5
                if dmid <= PART_JUNCTION_NEAR:
                    crossing.append((dmid, name))
            crossing.sort()

            if not crossing:
                # 切点附近没有别的路 = 这不是路口，是一条路被切成两段。
                # 加路口是错的（车会在直路中间停下等一个不存在的灯），
                # 但车道也会在这儿断开（缺口远大于 TARGET_GAP），所以要报出来。
                w("   !! %s ←→ %s 切点 (%.0f, %.0f) 缺口 %.0f 附近没有横穿路——"
                  "不是路口，车道会在这儿断开"
                  % (la, lb, mx, my, gap))
                continue

            for dmid, other in crossing:
                for me in (la, lb):
                    AUTO_FORCE_JUNCTIONS.append(
                        (me, other, mx, my, PART_JUNCTION_RADIUS, PART_JUNCTION_LIMIT))
                    made += 1
                w("   切点强制路口：%s ←→ %s 处的 %s（离切点 %.0f cm，缺口 %.0f）"
                  % (la, lb, other, dmid, gap))
    w("按 _PartNN 切点自动生成强制路口规则 %d 条" % made)
    return made


def endpoint_near(sp, point, tol=None):
    """point 是不是落在样条 sp 的首/末控制点附近。

    **取控制点、不按弧长采样**：`get_location_at_distance_along_spline(L)` 用的是
    近似弧长，曲线样条上采到 L 处未必正好是最后一个控制点（这条在生成转弯曲线时
    已经栽过一次）。
    """
    if tol is None:
        tol = ENDPOINT_TOL
    n = sp.get_number_of_spline_points()
    if n < 2:
        return False
    for i in (0, n - 1):
        e = sp.get_location_at_spline_point(i, WS)
        if ((e.x - point.x) ** 2 + (e.y - point.y) ** 2) ** 0.5 < tol:
            return True
    return False


def find_intersections(tsp, roads, total, self_name=""):
    """返回 [(路口中心沿线距离, 对向道路名)]，已排除立交、已合并相近的。

    self_name 用来查 FORCE_JUNCTIONS——不传就等于所有路对都用默认阈值。
    """
    forced_hits = []
    forced_seen = {}     # 强制规则覆盖范围内实测到的最近距离，成不成都要报
    endpoint_hits = []   # 靠端点规则捡回来的，必须单独报——它放宽了阈值，
                         # 一旦误判就是假路口，得能一眼看出是哪条规则加的
    raw = []
    d = 0.0
    while d <= total:
        p = tsp.get_location_at_distance_along_spline(d, WS)
        for name, osp in roads:
            # 同一条路被切开的两段之间不算路口（见 base_road_name）
            if self_name and base_road_name(name) == base_road_name(self_name):
                continue
            q = osp.find_location_closest_to_world_location(p, WS)
            dxy = ((q.x - p.x) ** 2 + (q.y - p.y) ** 2) ** 0.5
            ent = forced_entry(self_name, name, p)
            lim = ent[5] if ent else XY_CROSS
            if ent is not None:
                k = (ent[0], ent[1], ent[2], ent[3])
                if k not in forced_seen or dxy < forced_seen[k][0]:
                    forced_seen[k] = (dxy, d, abs(q.z - p.z), ent[5])
            if dxy < lim and abs(q.z - p.z) < Z_CROSS:
                raw.append((d, name, dxy))
                if ent is not None and dxy >= XY_CROSS:
                    forced_hits.append((d, name, dxy))
            elif dxy < ENDPOINT_CROSS and abs(q.z - p.z) < Z_CROSS:
                # 中段阈值没够，但可能是 T 形接头。看端点（见上面 ENDPOINT_CROSS
                # 那段的四种几何）。`near_own_end` 是"我正站在自己的端点上"。
                near_own_end = (d < ENDPOINT_TOL) or (total - d < ENDPOINT_TOL)
                other_end = endpoint_near(osp, q)
                hit_kind = None
                if other_end and not near_own_end:
                    hit_kind = u"对方端点"        # 我是主路，支路端点顶上来
                elif near_own_end and not other_end:
                    hit_kind = u"我方端点"        # 我是支路，端点顶在对方中段
                # 两头都是端点 = 首尾续接，不是路口，故意不处理
                if hit_kind:
                    raw.append((d, name, dxy))
                    endpoint_hits.append((d, name, dxy, hit_kind))
        d += DETECT_STEP
    # 强制规则的执行情况：成了要说，没成更要说（否则只是报告里少一行，很难发现）
    for (ra, rb, fx, fy), (best, at_d, dz, lim) in forced_seen.items():
        other = rb if str(ra).strip() == str(self_name).strip() else ra
        if best < lim and dz < Z_CROSS:
            w("   强制路口生效：与 %s 在 (%.0f, %.0f) 附近最近 %.0f cm"
              "（默认阈值 %.0f 够不着，按 %.0f 放行）"
              % (other, fx, fy, best, XY_CROSS, lim))
        else:
            why = ("阈值 %.0f 不够" % lim) if best >= lim else ("Z 差 %.0f 太大" % dz)
            w("   !! 强制路口没生效：与 %s 在 (%.0f, %.0f) 半径内实测最近 %.0f cm，%s"
              % (other, fx, fy, best, why))

    for d_at, nm, dxy, kind in endpoint_hits:
        w("   端点规则命中：与 %s 在沿线 %.0f 处缺口 %.0f cm（%s，"
          "中段阈值 %.0f 不够，端点阈值 %.0f 放行）"
          % (nm, d_at, dxy, kind, XY_CROSS, ENDPOINT_CROSS))

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
            if ok:      # 实测到了路面：按 BRIDGE_SURFACE_BLEND 往它靠一部分
                diff = p.z - zb
                if (BRIDGE_SURFACE_BLEND > 0.0
                        and abs(diff) <= BRIDGE_SURFACE_MAX_DIFF):
                    zb = zb + diff * BRIDGE_SURFACE_BLEND
                    who_tag = "bridge+surf"
                else:
                    who_tag = "bridge"
                # 记**混合之后**还差多少——那才是车实际会浮起/陷入的量，
                # 记原始偏差会让报告比实际严重一倍。
                dev = max(dev, abs(p.z - zb))
            else:
                who_tag = "bridge"
            p = unreal.Vector(p.x, p.y, zb)
            ok, who = True, who_tag
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
        elif name.strip() not in _no_junction:
            others.append((name, sp))
    if target is None:
        w("!! 找不到 %s，跳过" % road_label)
        return None

    tsp = target
    total = tsp.get_spline_length()
    w("")
    w("=" * 60)
    w("主路 %s   总长 %.0f cm   对照道路 %d 条（已排除不判路口的 %s）"
      % (road_label, total, len(others), "、".join(NO_JUNCTION_WITH) or "无"))
    w("=" * 60)
    roads = others

    inters = find_intersections(tsp, roads, total, road_label)
    w("检测到路口 %d 处（XY<%.0f 且 Z差<%.0f，相距<%.0f 的已合并）"
      % (len(inters), XY_CROSS, Z_CROSS, MERGE_DIST))
    ihalf_preview = [inter_half_for(nm, actor_by_label, road_label, c, total)
                     for c, nm in inters]
    for k, (c, name) in enumerate(inters):
        w("   沿线 %8.0f   %-18s 路口段半长 %.0f  截断半径 %.0f"
          % (c, name, ihalf_preview[k],
             ihalf_preview[k] + SEG_EXTEND + TARGET_GAP))

    # 每个路口各自的 INTER_HALF / BREAK_HALF
    # 路名和位置都要传：被切段在端点处的路口要用短半长（见 inter_half_for）
    ihalf = [inter_half_for(nm, actor_by_label, road_label, c, total)
             for c, nm in inters]
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
    """要处理哪些路。CROSSING_WITH 非空就自动算，否则用手写的 ROAD_LABELS。

    两种模式最后都会并上 EXTRA_ROADS。
    """
    if ALL_ROADS:
        names = [n for n, _s in all_splines]
        w("全铺模式：关卡里的道路样条 %d 条全部处理（只按 EXCLUDE_ROADS 剔除）"
          % len(names))
        return names

    if not CROSSING_WITH:
        return add_extras(list(ROAD_LABELS), [n for n, _s in all_splines])

    target, others = None, []
    for name, sp in all_splines:
        if name.strip() == CROSSING_WITH.strip():
            target = sp
        elif name.strip() not in _no_junction:
            others.append((name, sp))
    if target is None:
        w("!! 自动模式找不到 %s，退回手写名单 %s" % (CROSSING_WITH, ROAD_LABELS))
        return add_extras(list(ROAD_LABELS), [n for n, _s in all_splines])

    inters = find_intersections(target, others, target.get_spline_length(),
                                CROSSING_WITH)
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
    return add_extras(roads, [n for n, _s in all_splines])


def add_extras(roads, known):
    """把 EXTRA_ROADS 并进来，顺带报出名字对不上的（关卡里没有这条路）。

    名字打错会静默少铺一整条路，而报告里只是少一行、很难注意到，
    所以对不上的直接点名。
    """
    known_set = set(n.strip() for n in known)
    added, unknown = [], []
    for nm in EXTRA_ROADS:
        nm = nm.strip()
        if not nm or nm in roads:
            continue
        (added if nm in known_set else unknown).append(nm)
    if added:
        w("追加 EXTRA_ROADS %d 条：%s" % (len(added), "、".join(added)))
    if unknown:
        w("!! EXTRA_ROADS 里这几条在关卡里找不到，已忽略：%s" % "、".join(unknown))
    return roads + added


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
        sp = road_spline_of(a)
        if sp:
            all_splines.append((a.get_actor_label(), sp))
            actor_by_label[a.get_actor_label()] = a
    w("关卡道路样条共 %d 条（按文件夹 %s 找）"
      % (len(all_splines), "、".join(ROAD_FOLDERS)))
    if _skipped_parts:
        bases = sorted(set(base_road_name(x) for x in _skipped_parts))
        merged = sorted(lbl for lbl, _sp in all_splines if lbl.endswith("_Merged"))
        w("跳过 _PartNN 分段 %d 个（属于 %d 条逻辑路）：%s"
          % (len(_skipped_parts), len(bases), "、".join(bases)))
        w("已收到的合并产物 %d 个：%s" % (len(merged), "、".join(merged) or "无"))
        if len(merged) < len(bases):
            w("  !! 合并产物比被切的逻辑路少 —— **先跑 merge_part_roads.py**，")
            w("     否则这几条路整条不会有车道（而且不会报错，只是报告里少几条）。")
    # 一条都没找到时直接说清楚，别让它静默生成 0 个 actor——
    # 上一次就是这么白跑一轮的（报告里只有一行「合计 0 个」）。
    if not all_splines:
        w("")
        w("**一条道路样条都没找到，后面什么都不会生成。**")
        w("去 Outliner 确认道路样条在哪个文件夹下，改 ROAD_FOLDERS；")
        w("或者跑 find_road_splines.py 看所有带样条的 actor 都在什么类/什么名下。")

    # 被切成 _PartNN 的路，在切点处补强制路口（必须在 find_intersections 之前）
    build_part_junctions(all_splines)
    w("")

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

    # **再扫一遍改名前的遗留**。上面那轮是按路名清的（tag = ClaudeGenLane:<路名>），
    # 而路名刚从 形状xx 变成 MainRoad_R0xx / BranchRoad_R0xx，对不上——
    # 旧车道会原地留在关卡里贴着**旧**路几何，和新车道混在一起，
    # 而且数量相当（实测残留 262 段 + 218 路口段），车会乱跑且极难看出原因。
    # 判据：带本脚本的 tag 前缀、但路名不在本次名单里 = 遗留。
    # **只在 ALL_ROADS 下做**：非全铺模式时，"不在本次名单里"完全可能是
    # 本次故意不处理的路，那样扫就是误删别人的成果。
    if ALL_ROADS and roads:
        prefix = TAG_PREFIX + ":"
        keep = set("%s%s" % (prefix, r) for r in roads)
        orphan = 0
        for a in actors:
            try:
                tags = set(str(t) for t in a.tags)
            except Exception:
                continue          # 上一轮已经销毁的，拿 tags 会抛
            mine = set(t for t in tags if t.startswith(prefix))
            if mine and not (mine & keep):
                try:
                    eas.destroy_actor(a)
                    orphan += 1
                except Exception:
                    pass
        w("清除改名前遗留的车道 %d 个（tag 以 %s 开头、路名已不存在）" % (orphan, prefix))
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
