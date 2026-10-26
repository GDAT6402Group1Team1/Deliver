# 待手动处理：方向多于两个的路口（2026-10-26）

`fix_light_number_by_axis.py` 按"同轴同组、相交异组"染色时，有 **5 个路口**
聚出了 3 个以上轴向。脚本会给它们 0/1/2（甚至 0~3），能跑——蓝图那套
`Map<LightNumber, 段>` + 游标天然支持 N 组（`Length` 就是组数，轮 N 轮）——
但**每个方向要等 `(N−1) × LightDuration`**，4 组就是等 18 秒，偏长。

所以这 5 个留给人工决定。下面是实测数据和我的判断。

## 先分清两类：真多岔 vs 盒子罩过界

判据是**同一条路的段之间轴角差多少**。同一条路在一个路口里应该只有一个走向
（对向车道是 ±180°，归一化后同轴）。如果同一条路出现两个差很多的角度，
那多半是**路口盒罩过界、把隔壁路口的段也收进来了**——
`BP_Intersection` 靠 `GetOverlappingActors` 按 Box 重叠找子节点，盒子大一点就会越界。
那种情况正解是调盒子，不是开多相位。

### 疑似盒子罩过界（优先查这两个）

**Intersection_40** — `107°×5 / 141°×2 / 9°×1 / 42°×1`
```
Inter_MainRoad_R010_+0540_I16   104°      ← R010 四条车道
Inter_MainRoad_R010_+0180_I16   107°
Inter_MainRoad_R010_-0180_I16   109°
Inter_MainRoad_R010_-0540_I16   110°
Inter_MainRoad_R006_+0540_I00   141°      ← R006
Inter_MainRoad_R006_-0540_I00   141°
Inter_MainRoad_R006_+0180_I00     9°      ← **同样是 R006，和上面差 132°**
Inter_MainRoad_R007_+0540_I00    42°      ← R007
```
`R006` 自己出现 141° 和 9° 两个走向，差 132°。要么那条路在路口里急转，
要么这个盒子同时罩住了两个路口。

**Intersection_41** — `159°×4 / 85°×2 / 65°×1 / 99°×1`
```
Inter_MainRoad_R008_+0180_I00   147°      ← R008
Inter_MainRoad_R008_+0540_I00    99°      ← 同样是 R008，差 48°
Inter_MainRoad_R009_+0540_I00     9°      ← R009
Inter_MainRoad_R009_+0180_I00    65°      ← 同样是 R009，差 56°
Inter_MainRoad_R010_+0540_I08   162°      ← R010
```
`R008` 和 `R009` **各自**的两段都差了四五十度。两条路同时在路口里急转的可能性不大，
更像是盒子跨了两个路口。

### 看起来是真多岔

**Intersection_39** — `62°×5 / 4°×2 / 97°×2`
```
Inter_MainRoad_R010_-0540_I03    64°   ← R010 四条车道，走向一致
Inter_MainRoad_R010_-0180_I03    66°
Inter_MainRoad_R010_+0180_I03    69°
Inter_MainRoad_R010_+0540_I03    75°
Inter_MainRoad_R004_+0540_I00     4°   ← R004 两段一致
Inter_MainRoad_R004_-0540_I00     4°
Inter_MainRoad_R005_+0540_I00    97°   ← R005 两段一致
Inter_MainRoad_R005_+0180_I00    97°
```
三条路各自走向都自洽（R010 内部 64~75° 是正常的曲率），是**真的三岔**。

**Intersection_33** — `78°×4 / 29~41°×3 / 179°×1`
```
Inter_MainRoad_R003_±0180/±0540_I00   78°    ← R003 四条车道
Inter_MainRoad_R010_-0180_I19         29°    ← 注意 I19
Inter_MainRoad_R010_+0180_I00         31°    ← 和 I00 混在一起
Inter_MainRoad_R010_-0540_I19         41°
Inter_MainRoad_R010_+0540_I00        179°    ← R010 的另一头，差 ~140°
```
`I19` 和 `I00` 同时出现，说明 **`MainRoad_R010` 是闭环**，它的首尾在这里汇合。
所以 R010 的两端以不同朝向进入同一个路口，三个方向是真实几何，不是盒子问题。

**Intersection_38** — `14°×4 / 104°×1 / 142°×1`
```
Inter_MainRoad_R003_-0180_I08   142°
（另外几段现在的值已经正确，所以没进改动列表）
```
104° 和 142° 各只有 **1 段**，是单向的支路接头——很可能是这次新加的端点规则
（T 形接头）捡回来的。单段意味着那个方向只有一条车道。

## 三个可选做法

1. **调路口盒**（针对 40 / 41）：如果确认是盒子跨了两个路口，用
   `gen_intersections.py` 的 `CLUSTER_DIST`（现 2500）收紧，或者手动改那两个
   盒子的缩放。改完重跑 `fix_light_number_by_axis.py` 复核。
2. **接受 N 相位**（针对 33 / 38 / 39）：真多岔就让它轮 N 轮。想缩短等待就把
   该路口的 `LightDuration` 单独调小（那是每个路口实例自己的变量）。
3. **压成两组**：把第 3 组及以后并进组 1。代价是**那两个方向会同时放行**，
   真十字会撞车；三岔里如果那两个方向互不冲突（比如都是右转进同一条路）才可行，
   得按几何逐个看。

## 手动改完要做的事

`fix_light_number_by_axis.py` 顶部有 `SKIP_INTERSECTIONS`，把手调过的路口名填进去，
**否则下次跑它会按轴向重新染色、把你的手改冲掉**（尤其是做法 3：把两个不同轴向
并成同一组，在脚本眼里就是"相交却同组"的错误，它会拆回去）。

另外注意**重新跑 `gen_traffic_lanes.py` 会生成全新 actor，所有 `LightNumber`
手改都会丢**。链条顺序：

```
gen_traffic_lanes.py → gen_intersections.py → fix_light_number_by_axis.py
  →（另起执行）fill_turn_splines.py →（另起执行）fix_t_junctions.py
```

复核用 `audit_light_number_axes.py`。
