# AGENTS.md

本文件面向在此仓库工作的 AI coding agent，说明项目背景、现状和约定。
（内容与 [CLAUDE.md](CLAUDE.md) 同步维护；克隆/LFS/提交流程见 [README.md](README.md)。）

## 项目概览

**Delivery** —— 欢乐向奇遇送快递游戏，Unreal Engine 5.8。玩家当快递员，
在风格各异的地区送货，途中可以对 NPC 和场景物件搞恶作剧。单人升级推进，
多人抢单竞速（先送达者拿走该单奖励）。

这是一个 Unreal Engine 工程，没有传统意义上的 `npm test` / `make` 构建命令：
- 打开工程：用 Unreal Editor 5.8 打开 `Delivery.uproject`。
- 生成 IDE 工程：右键 `.uproject` → Generate Visual Studio project files（生成的 `.sln`/`.vs` 不提交）。
- C++ 单元测试：`Source/Delivery/Combat/DeliveryBoxingPoseTests.cpp`，通过 UE 的 Automation/Session Frontend 运行，不是命令行测试。

## 目前已实现的内容（从 git 历史与代码整理）

### 1. 角色移动 —— 主动布娃娃（Active Ragdoll），非常规 Character Movement
角色采用"持续物理驱动"：从 BeginPlay 起 Simulate Physics 常驻打开，走路/站立/
摔倒/起身/抓取/互殴都在同一条刚体链上完成，而不是"平时动画、摔倒才切物理"的
教程式做法。完整设计原理、与常见教程实现的对照表、七步实现步骤、角色建模要求，
都写在 [Document/Ragdoll.html](Document/Ragdoll.html) 里——**改动布娃娃/战斗相关代码前务必先看这份文档**。

关键文件：
- [Source/Delivery/Ragdoll/DeliveryActiveRagdollComponent.h](Source/Delivery/Ragdoll/DeliveryActiveRagdollComponent.h) —— 直立参考体、电机强度、倒下/起身判定、坡地位移。
- [Source/Delivery/DeliveryCharacter.h](Source/Delivery/DeliveryCharacter.h) —— 第三人称 Pawn，组装胶囊/网格/相机/布娃娃/战斗组件，实现 `IAbilitySystemInterface` 与 `IDeliveryCombatInterface`。移动与跳跃走网络复制（`ServerSetMoveInput` 为 Unreliable，`ServerJump` 为 Reliable，带最小跳跃间隔）。

### 2. 互殴系统（近战）
- [Source/Delivery/Ragdoll/DeliveryRagdollCombatComponent.h](Source/Delivery/Ragdoll/DeliveryRagdollCombatComponent.h) —— 在布娃娃刚体链上执行出拳动作。
- [Source/Delivery/Combat/DeliveryCombatInterface.h](Source/Delivery/Combat/DeliveryCombatInterface.h) —— `StartMeleeAttack` / `GatherMeleeHits` / `EndMeleeAttack` / `IsMeleeAttacking`，Character 实现，GA 调用。
- [Source/Delivery/Combat/DeliveryCombatTypes.h](Source/Delivery/Combat/DeliveryCombatTypes.h) —— `FDeliveryArmPoseSettings`：A 姿势、收拳、直拳三段姿态的参数化定义，握拳靠手指弯曲角度模拟（无手指刚体）。注意多组参数之间有比例耦合关系（如 `WindupUp`/`WindupOutward` 需按 `PunchReach` 换算对齐，否则出拳轨迹会偏成横扫或找高度）。
- [Source/Delivery/Combat/DeliveryHandPose.h](Source/Delivery/Combat/DeliveryHandPose.h)、[DeliveryBoxingPose.h](Source/Delivery/Combat/DeliveryBoxingPose.h)（含单测）。
- 左右拳通过 GAS 技能 [GA_DeliverPunch](Source/Delivery/GAS/Abilities/GA_DeliverPunch.h) 触发，对应 tag `Ability.Attack.Punch.Left/Right`。

### 3. GAS（Gameplay Ability System）
- [DeliverAbilitySystemComponent](Source/Delivery/GAS/DeliverAbilitySystemComponent.h) 挂在 **PlayerState**（[DeliverPlayerState](Source/Delivery/GAS/DeliverPlayerState.h)）而非 Character 上，随 PlayerState 复制；`ADeliveryCharacter::OnRep_PlayerState` 里处理绑定。
- [DeliverAttributeSet](Source/Delivery/GAS/DeliverAttributeSet.h) —— 已接入属性表；HP 回血 GE 目前隐藏。
- [DeliverGameplayTags](Source/Delivery/GAS/DeliverGameplayTags.h) —— Native Gameplay Tags：`Ability.Attack.Punch.Left/Right`、`Effect.Type.Damage`、`State.Stunned`。
- Character 上的 `HealthRegenEffect` / `DamageEffect`（Instant + SetByCaller `Effect.Type.Damage`）及左右拳 GA 类，均在蓝图 `BP_DeliveryMan` 中指定，C++ 侧只留 `TSubclassOf` 插槽。

### 4. 任务系统（电话接任务）
**结构、接口清单、配置方式、完整调用链、待确认假设都写在 [Document/TaskSystem.md](Document/TaskSystem.md)——改任务系统前先读这份。** 要点：

- 任务状态全局共享（多人下解锁/来电/接取/计时/完成对所有玩家是同一份数据），权威在 GameState 上的 [DeliveryTaskManagerComponent](Source/Delivery/Task/DeliveryTaskManagerComponent.h)；只有"当前追踪哪个任务"是每玩家各自的，在 PlayerState 上的 [DeliveryTaskTrackerComponent](Source/Delivery/Task/DeliveryTaskTrackerComponent.h)。
- 计时是服务器时间戳相减算出来的，不是 Tick 累加：快递掉落、换手、进车后备箱、持有者晕倒都影响不到计时，这是"计时不停"规则的实现方式，别改成累加。
- "全世界同时只有一个进行中任务"落在 `CanAcquireItem()` 这条取件规则上，没有额外的互斥状态。
- 四个状态、无失败态：`Locked / AwaitingPickup / InProgress / Completed`。超时只打一次催促电话 + 降低奖励倍率，任务不会结束，倒计时转正计时。
- 电话队列 [DeliveryPhoneCallQueueComponent](Source/Delivery/Task/DeliveryPhoneCallQueueComponent.h) 由服务器按每通电话配置的时长推进，不等客户端播完回报（队列是全局共享的）。
- 一个任务 = 一份 [DeliveryTaskDefinition](Source/Delivery/Task/DeliveryTaskDefinition.h) 资产；关卡任务清单填在 GameState 蓝图的 `TaskDefinitions` 数组里，顺序即同时解锁时的来电顺序。
- 快递 Actor 挂 [DeliveryItemComponent](Source/Delivery/Task/DeliveryItemComponent.h)、收件人挂 [DeliveryTargetComponent](Source/Delivery/Task/DeliveryTargetComponent.h)，交互/背包系统只需要调 `CanBeAcquired` / `NotifyAcquired` / `TryDeliver` 三个口。
- 任务数值由策划在 `Design/Tasks.csv` 里维护，编辑器菜单 **Delivery → Import / Reimport Tasks** 导入成 DataAsset（脚本在 `Content/Python/delivery_task_import.py`，按 TaskId 增量更新，不会冲掉资产上手填的字段）。时间评价档位用**剩余秒数**表达，正数提前、负数超时，和策划表一一对应。
- 调试用控制台命令（`Delivery.Task.Dump` / `Acquire` / `Deliver` / `Event`）见 [DeliveryTaskDebugCommands.cpp](Source/Delivery/Task/DeliveryTaskDebugCommands.cpp)，在 PIE 里不用 UI 就能跑完整个任务流程；`DeliveryTaskDefinition` 有 `IsDataValid` 校验，阈值配反、档位顺序错会在编辑器里标红。
- UI、背包/交互、金钱结算、存档都还没做，对接点见文档第七节；文档第八节列了我在实现时替规则做的假设，需要确认。

### 5. 地图 / 交通场景
- 已导入地图并接入 **PS2DEMImporter**（`Plugins/PS2DEMImporter`，PS2 地形/道路生成插件）用于把地形转换为 landscape spline 道路。
- 交通路口、红绿灯、道路与门的破碎效果正在搭建：`Content/PS2DEM`（`BP_Intersection`、`BP_TrafficLine*`）、`Content/trafficlight`。车辆沿样条行驶、进路口的 Overlap 检测都写在 `BP_car_base` 事件图里，C++ 侧原本没有对应基类。
- [Source/Delivery/Traffic/DeliveryTrafficCarComponent.h](Source/Delivery/Traffic/DeliveryTrafficCarComponent.h) —— 挂在 `BP_car_base` 上的辅助组件，不接管移动本身：车辆走完所有预设样条、没接上下一段车道时会脱离路线按最后方向裸奔冲出画面；蓝图每帧用 `UpdateRouteFollowState(bool)` 告诉组件这一帧还在不在跟随预设样条，连续脱离满 5 秒（`RouteLostTimeout`）广播 `OnRouteLost` 让蓝图把车放回起始样条起点、形成循环车流；同时自己在 `TickComponent` 里做前方球形扫描，探测到前方另一辆挂了同组件的车就把 `GetSpeedMultiplier()` 平滑降到 0，蓝图乘到目标速度上即可实现遇前车减速到停、让开后恢复。
- 测试地图：`Content/Level/TestForCharacter.umap`（角色/互殴）、`Content/Level/testfortraffic.umap`（交通路口）。


### 5b. 交通线生成工具链（`Content/Python`，编辑器 Python）
车道线、路口段、左右转、路口盒全部由脚本沿道路样条生成，不手摆。共四步，
**前两步由 `rebuild_traffic.py` 跑，后两步必须各自另起一次 `py` 执行**：

1. `gen_traffic_lanes.py` —— 直行车道 + 路口段      ← rebuild_traffic 跑
2. `gen_intersections.py` —— 路口盒（按上一步 `Inter_*` 的 Box 位置算） ← rebuild_traffic 跑
3. `fill_turn_splines.py` —— 左右转曲线（按上一步的样条端点算）
4. `fix_t_junctions.py`  —— T 形路口修正（按上一步的转弯曲线）

**3、4 不能和 1 挤在同一次 `py` 执行里**（实测三轮）：那样写进去的全是马上要
作废的组件，回读时组件名已变成 `TRASH_SplineComponent_xxxx`、点数 0，存盘什么都没有。
单独跑一次 `fill_turn_splines.py` 就正常。试过并被推翻的两个解释：
"旧 actor 没被 GC"（加了 `is_valid` 护栏，一条都没拦到，actor 全是有效的）、
"构造脚本要到下一帧才重跑"（挂 slate 回调等 5 帧再写，354/354 照样全灭）。
关键不是帧数也不是 actor 生命周期，是必须另起一次执行；真正的机制仍未查清。
`rebuild_traffic.py` 已改成跑完前两步就停下、打印剩下两条命令，不再假装能自动接力。

**顺序更不能换**：转弯没写成功就跑第 4 步，它会把蓝图默认的 100cm 残桩当成右转
抄进直行样条——实测毁过 44 条直行（报告里显示"已合并，2 点"）。第 4 步现在有
残桩护栏（点数 ≤3 且长度 <300 就拒绝并报警），但那是兜底，不是许可证。

**"跑完车道忘了跑路口盒"已经造成过两次"红绿灯没了"**：路口盒是按 `Inter_*` 的 Box 位置算的，
车道一重新生成 Box 全部挪位，旧盒子罩不住新的路口段，`BP_Intersection` 就找不到要管的子节点。
把顺序固化进 `rebuild_traffic.py` 就是为了不再靠记性。

生成物按 tag 区分，可重复运行、互不误删：`ClaudeGenLane:<路名>`（车道+路口段，按路独立）、
`ClaudeGenIntersection`（路口盒）、`ClaudeGenTurn`（早期独立转弯 actor，已弃用）、`ClaudeGenSkirt`（裙边）。

关键参数都在 `gen_traffic_lanes.py` 顶部，每个都写了为什么是这个值：
- `ALL_ROADS = True` —— **全图所有道路样条都铺**（当前状态）。实测 38 条路
  -> 车道段 278 + 路口段 274 = 552 个 actor，53 个路口。
  置 False 时才轮到 `CROSSING_WITH`（= 这条路 + 所有与它相交的路）和 `EXTRA_ROADS`
  （自动模式算不到的远处新路，如形状40，靠它显式追加）
- `EXCLUDE_ROADS` —— 排除不铺车道的路。形状43 的源样条本身是坏的（2 个点、首尾同坐标、
  长度却有 1597）；**River / BranchRiver 是河不是路**，全铺模式下不排除就会沿着河铺出车道
  （它们和道路同属 `BP_PS2DEMSplineActor_v3_C`，实测"路宽"3050 / 4000）
- 车道横向位置用 `MAIN_ROAD_OFFSETS` / `SIDE_ROAD_OFFSETS` **显式指定**，不再按"宽度/车道数"均分。
  规则是"车道落在等分点上"：主路 1800 五等分 → ±180/±540，次路 1080 三等分 → ±180
- `TARGET_GAP = 150` —— 路段与路口段的空隙。250 时实测车接不上下一段（空隙里车是脱离样条直行的，越长横向漂得越多，弯道上容易错过下一段的 Box）
- `INTER_HALF_EXTRA = 300` —— 路口段两头各加长。代价是 36 个路口里 23 个的路口盒为了罩住它压上了车道 Box，这是权衡后接受的；想回到不重叠就降到 150
- `BOX_SCALE = 2.0` —— 车道/路口段自带的 Box 半尺寸从 32 放大到 64（实际 128×128×600）
- `LIGHT_NUMBER_BY_AXIS` —— 沿 Y 走向的路口段 `LightNumber = 1`、沿 X 的保持 0，红绿灯才能交替放行

生成时踩过并已修掉的坑，都有实测依据，别再走回头路：
- **高度必须逐点打射线重测**，不能信源样条的 Z（形状13 与真实路面偏差 −546 ~ +411cm）
- **一竖线上可能有多层路面**：路口处两条路的路面叠着，实测差约 200cm。只取最上面那层的话，
  路口内的点会爬到交叉路的面上、路口外的点还在本路面上，接缝出现 250cm 台阶。现在
  `surface_layers()` 取所有层，`build_profile()` 做**链式选层**——从只有单层的站点起锚、
  向两头传播、每步挑离上一站最近的那层，而不是挑最上面的
- **命中同一块板要聚成一层**：射线每次只降 1cm，26cm 厚的板会被连续命中二十几次，
  预算全耗在同一块板上够不到下层；命中路面后直接跳 `SLAB_SKIP = 30` 一步跨过整块板
- **高度剖面按整条车道算一次**（`PROFILE_STEP = 250`），所有段共用锚点。按段各自插值会让
  相邻两段用不同的锚点，各自平滑但接缝对不上
- **路口段的高度不实测，由两侧车道段的端点桥接**（`BRIDGE_INTERSECTIONS`）。路口内部不存在
  唯一正确的"路面"，桥接保证车开得顺；代价是路口里车可能浮起/陷入，超过 `BRIDGE_DEV_WARN = 80`
  的会在报告里点名——偏差大说明那个路口两条路的路面本来就对不上，是关卡几何问题
- **转弯曲线用"直线进 → 定半径圆弧 → 直线出"**（`TURN_RADIUS`，当前 400cm）。
  切点到角点的距离 `T = R*tan(|偏转角|/2)`，所以**半径同时控制弯的急缓和首尾直线段的长短**：
  半径大 → 切点离角点远 → 直线段短、弯缓；半径小 → 弯急、但首尾留下更长的直线。
  端点位置是定死的，这两个没法同时要。切点越过端点时半径自动夹小，报告会标注。
  之前两版都留着（`USE_ARC=False` 退回贝塞尔），失败原因值得记：
    * 切线各伸出 k 倍距离的三次贝塞尔 —— k 要猜，猜大了曲线在反面鼓包、整条变成 S 形
    * 以两条切线交点为控制点的二次贝塞尔 —— 不会 S 形，但整段都在转，
      车到终点时还没转到目标朝向、扫不到下一条线
  实测转弯终点、目标 Box、距离都和直行完全一致（149~151cm），所以"接不上"只可能是朝向问题，
  这也是换成圆弧（出口前有一段直的）的依据。
- **`get_direction_at_distance_along_spline` 在这些样条上返回零向量**，方向要用样条上两个采样点作差求。
  兜底成 `(1,0,0)` 是有害的——它把"取不到方向"伪装成"方向朝 +X"，
  报出来的错误原因（"两端切线平行"）和真实原因毫无关系，白查一轮
- **端点取控制点，不按弧长采样**：`get_location_at_distance_along_spline(L)` 用的是近似弧长，
  曲线样条上采到 L 处未必正好是最后一个控制点。贝塞尔首尾也直接钉死成 p0/p3，不靠多项式算
- 转弯路径挂在路口段 actor **自带的 `SplineLeft`/`SplineRight`** 上，和直行共用同一个 Box
  （车探测到一个 Box 就拿到三条候选，选路是随机的，这是想要的行为）。
  用不上的那条**写成和直行一模一样**的样条——选中它就等于直行，蓝图不需要任何特判。
  早先压成零长，坏在它和蓝图默认的 100cm 都是"短东西"，排查时反复看混；
  更早什么都不做，会留下 304 根方向一律朝 +X 的 100cm 残桩散在全图。
- `check_turns.py` 全图核对 304 条转弯样条的状态（转弯曲线 / 直行副本 / 默认残桩 / 零长 / 缺组件）。
  **改完关卡重启前后各跑一次比数字**，是验证"存住了没有"的标准动作。
  当前基准：126 条转弯曲线 + 178 条直行副本，残桩 0。

**编辑器 Python 写数据的三条边界**（都是实测撞出来的，不只适用于样条）：
1. **改之前必须 `modify(True)`**。`clear_spline_points` / `add_spline_point` / `update_spline`
   这类普通函数调用只改内存、**不把对象标记为已修改**，保存时整个对象不被序列化。
   现象极具迷惑性：写完立刻读回是好的，存盘重载就变回蓝图默认值。
   补上 `sp.modify(True)` + `owner.modify(True)` 之后，存活率从 1/8 变成 8/8。
   脚本里"写入 N 条"这种计数器只能证明函数被调用过，**证明不了结果留住了**——
   要验证就得写完回读，或者重启后用 `check_turns.py` 比数字。
2. **样条的点数据存得住，组件的存在与否存不住。** 实例上 `destroy_component` 当场生效，
   但重载后组件按蓝图 SCS 重建（加 `modify` 也没用，被删的对象本身没了、没有覆盖数据可存）。
   所以用不着的组件**不能删，只能写成别的值**。真要彻底去掉只有复制出变体蓝图各删一条
   （`make_turn_bp_variants.py`），代价是三个资产要同步维护。
   注意在**共用蓝图**上删组件是真删、会一次性作用到全部实例——删掉 `SplineRight`
   等于所有车道都失去右转。
3. **蓝图被改动（加/删组件）会重跑所有实例的构造脚本，期间写进去的实例覆盖会丢。**
   要写数据就等蓝图定型（编译 + 保存）之后再写。

地形/路面配套（同目录）：
- `deform_terrain.py` —— 沿路把地形压平到"路面顶面 −40"。**压平带宽度按大纲文件夹取标称半宽**
  （主路 900 / 次路 540）与实测取大值、再加余量 150。早先只信实测的第 30 百分位，
  等于全路 70% 的断面比压平带宽、路缘外侧根本压不到，实测路缘埋没率 **37%**；改完降到 **3%**
- `thicken_road.py` —— 路面从 6cm 加厚到 26cm；`gen_road_skirt.py` —— 路面下铺绿色裙边遮住悬空的侧面
- 这几样都可能在撤销/重载关卡时丢失。**先跑 `check_road_state.py` + `diag_road_buried.py` 定位**：
  厚度和裙边都在、埋率却高 → 是地形压平丢了（它存在 landscape 的编辑图层里，前者查不到），单跑 `deform_terrain.py` 即可

诊断脚本一律只读、结果写到 `Saved/*.txt`：`diag_lane_fit.py`（车道贴不贴现在的路面：
逐点测高差 + 沿法线探到路缘的余量）、`diag_road_width.py`（源道路样条两侧路面的实际半宽，
用中位数避开路口处扫到交叉路造成的虚高）、`diag_spline_tangent.py`（端点/中间点的切线与
旋转前向是否退化）、`check_turn_targets.py`（每条转弯的终点有没有同向下家）、
`spawn_test_cars.py`（清掉所有 `BP_car_base` 并在车道上零散放一批测试车，
随机颜色 + 600~1500 的随机 `MaxSpeed`）、`diag_lane_gaps.py`（接缝间距 + 高度差）、
`diag_seams.py`（逐点查落差来源）、`whats_here.py`（某个 actor 附近都有哪些样条）、
`inventory_level.py`、`audit_traffic_channels.py`（碰撞通道）、`dump_bp_graph.py`（导出蓝图图表）、
`monitor_car.py` / `chase_car.py`（Simulate 期间采样车状态 / 跟拍相机——用每帧回调而不是 sleep 轮询，
Python 跑在游戏线程上，轮询会把模拟本身卡死）。

**还没解决的**：
- **`Lane_*` 和 `Inter_*` 的 Box 在同一个碰撞通道**（都是 `ECC_TRAFFIC_ROAD`），`TraceForIntersection` 分不出两者。
  更要紧的是同一路口里**别的路**的 `Inter_` 也能通过 Cast，车可能读到横向车流的灯态（灯正好相反）。
  通道分离解决不了这个（两者都是 `IntersectionChild`），得给命中结果加方向校验（同向 dot > 0.7）
- **形状7 的路口段首尾控制点重合**，自动切线仍是零（见下面「端点切线」一条）。
  三个写样条的地方都加了相邻重合点去重（阈值 2cm），但**要下次重建才生效**。
  全图范围没统计过，只知道抽查的 8 条里坏的全在形状7 上。
- **`BP_Intersection` 怎么找它管的那些 `IntersectionChild` 仍然没查清**，一直靠"重跑
  `gen_intersections.py`"这种经验性修复（车道一重新生成，`Inter_` 的 Box 全挪位，旧盒子就罩不住了）。
  已排除的方向：`BP_TrafficLine1` 和 `BP_TrafficLine1_IntersectionChild` 两个蓝图的
  构造脚本和事件图**全是空的**（只有入口节点和调父类的空事件），没有任何蓝图代码碰样条。
  蓝图图表现在能完整导出了，见下。

**端点切线：`CURVE_CLAMPED` 会把端点切线归零**（这一条牵连很广，别再绕过去）。
现象是车的探测球在快到路口时**甩向世界 +X 一两帧**，车本身不跟着动。追下来：
`TraceForIntersection` 的探测终点 = 未来点 + `GetForwardVector(Rotation)` × 200，
其中 `Rotation` 来自 `GetRotationAtDistanceAlongSpline`，而引擎那个函数内部是
`MakeFromXZ(切线, Up)`——**切线为零时构造退化，前向掉回世界 +X**。
而 UE 的 "Clamped" 自动切线，定义行为就是把端点和局部极值处的切线归零。
实测端点切线长恒为 0、旋转前向恒为 0.0°，真实走向却是 −96°（相对车头正好是右侧）。
只闪一两帧是因为探测终点只在 `Equal(未来点, 最后一个控制点, 容差100)` 为真时才延伸，
其余时间是零长扫描、根本看不见方向。
修法：中间点保持 `CURVE_CLAMPED`（不规则高程上防过冲），**首尾两点改成 `CURVE`**，
自动切线指向邻点、非零，曲线形状几乎不变（控制点一个都不动）。
实测 `Lane_` 的端点异常从 25% 降到 0%。
这条同时推翻了旧记录里那句"`get_direction_at_distance_along_spline` 在这些样条上
返回零向量"——不是这些样条特殊，是**只在端点**取值才为零，当初正好在端点采的样。
`fix_endpoint_tangents.py` 可以就地改点型（不重建 actor，保住转弯和 T 形修正的成果）。

**`mark_edited` 要在写点之前调**（推论，尚未验证）。它是 `set_editor_property`，
而在组件上改属性会触发那个 actor 重跑构造脚本、组件整批重建；放在最后调的话，
刚写进去的点正好落在被丢弃的那批上。这个标记是实例覆盖、会存住，所以第二次跑时
属性值没变、不触发重跑，点就留住了——这能解释"同一个转弯脚本要跑两三次才出来"。
三个写样条的脚本都已改成先标记后写点。**验证方法**：看报告里
「同一次运行内回读」是不是**第一次**就"全部一致"。

**T 形路口的处理规则**（`fix_t_junctions.py`，判据只看数据、不认几何形状）：
车沿样条从第 0 点开到最后一点，于是"进得来"= 有 `Lane_` 的终点落在本段起点附近，
"出得去"= 有 `Lane_` 的起点落在本段终点附近（阈值 600cm）。三种处理，顺序不能换：
1. **进不来的整个 actor 删掉**（"从对面出来"的段，对面根本没有路）。必须排最前，
   因为后两步要用存活段的起点当锚点，孤儿段的起点本身就在虚空里。
   删 actor 是安全的——脚本 spawn 的 actor 删除是真删、存得住；**组件**才是删不掉的那种。
2. **终点没有下家的转弯写成直行副本**。锚点必须带方向：只看位置的话，
   对向车道的起点也在附近（实测 360cm），会把逆行当成合法下家——
   `Inter_形状13_-0540_I03` 和 `Inter_形状13_+0180_I03` 的废转弯就是这么漏过去的。
   加上同向判据（dot > 0.5）后一轮就多抓出 17 条。
3. **进得来但出不去的，直行样条写成右转曲线的副本**（T 形岔路的进入段，
   直行开出去是虚空）。排在第 2 步之后：若右转刚被判成废转弯写回直行，
   这里合并就是空操作，脚本会识破并点名。
`check_turn_targets.py` 是同一套判据的只读全图审计。

**"只有一条路"的路口**：路口存在、但交叉的那条路没有交通线时，两条转弯都会被
写成直行副本，车开到那儿三条路全是死的。全铺之前这种路有 13 条（River、形状9、
形状42/34/57/17/56 等）。`ALL_ROADS = True` 之后基本消失。

**蓝图图表 API**（`dump_bp_graph.py`，已验证可用）：
`BlueprintEditorLibrary`（`list_graphs` / `find_graph` / `get_node_title` / `list_all_pins`）
+ `BlueprintGraphEditor`（`get_graph_editor_by_name` / `list_all_nodes`）
+ `BlueprintGraphPin`（`get_pin_name` / `list_connected_pins` / `get_owning_node` / `try_create_connection`）。
连线能读也能改。两个坑：`UBlueprint` 的 `FunctionGraphs` 和 `UEdGraph` 的 `Nodes` 都是 protected、
`get_editor_property` 读不到，必须走上面这套；`get_pin_name()` 返回的是 `unreal.Name` 不是 `str`，
直接切片会抛 `'Name' object is not subscriptable`，整个导出断在第一个节点。

### 6. 人物美术
- 已导入测试角色模型；建模要求（骨骼数量、四肢截面需容纳胶囊碰撞体、不使用标准 Mannequin/人体解剖骨骼）见 Ragdoll.html 第五节。

### 7. 交互系统 + 可骑摩托车

**交互（`Source/Delivery/Interaction/`）** —— 通用的"走近按 F"框架，摩托车是第一个用户。
**和 `Grab/` 是两回事**：Grab 是双键按住、用物理约束把东西抓在手上的连续动作，这里是一次性按键交互。
- [DeliveryInteractableComponent](Source/Delivery/Interaction/DeliveryInteractableComponent.h) —— 挂在可交互 Actor 上，持有提示词/半径/浮窗高度，交互时广播 `OnInteract`。查找用的是**静态注册表**而不是球形 Overlap：可交互物就几个，遍历代价忽略不计，而碰撞查询在本项目已经栽过一次（见 5 节交通组件"通道配错导致探测恒为空"）。
- [DeliveryInteractionProbeComponent](Source/Delivery/Interaction/DeliveryInteractionProbeComponent.h) —— 挂在玩家 Pawn 上（`ADeliveryCharacter` 构造函数里已加），10Hz 探测最近目标并推浮窗。只在 `IsLocallyControlled()` 的 Pawn 上跑，所以上车之后被丢下的那具身体不会再提示。
- [DeliveryPromptSubsystem](Source/Delivery/Interaction/DeliveryPromptSubsystem.h) —— 浮窗本体，**C++ Slate 直接挂视口**，没有 WBP 资产。中文靠 Slate 自带字体回退（引擎自带 `DroidSansFallback.ttf`）渲染。调用约定是"每帧推一次"，停推 0.25 秒自动消失——这样调用方不需要成对写 Show/Hide，不会因为某条退出分支漏掉 Hide 把提示永久留在屏幕上。世界坐标→屏幕坐标用 `UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition`（自带 DPI 折算），为此 Build.cs 加了 `Slate`/`SlateCore`/`UMG`。
- 交互键走服务器复核：客户端 `ADeliveryCharacter::DoInteract()` 把本地探到的目标发 `ServerInteract(Target)`，服务器重新查组件 + 距离才执行。

**摩托车（[DeliveryMotorbike](Source/Delivery/Vehicle/DeliveryMotorbike.h)）** —— 运动学街机式载具，不是 Chaos Vehicle：
- 美术资产是一整套静态网格 + 一个坐姿骑手，**没有轮子骨骼、没有物理资产**，Chaos 需要的东西一样都没有；且关卡里的交通车本来就是运动学沿样条走的，玩家车用同一套假设不会出现"玩家车被物理弹飞、AI 车纹丝不动"。
- 每帧自己算速度/转向，水平位移带 sweep 挡墙，竖直方向打射线贴地（竖直**不能**用 sweep，会和"贴到地面上"互相打架）。转向量乘 `Speed/TurnSpeedReference`，停着不能原地转圈。侧倾只改 `MeshRoot` 的 Roll，不影响碰撞。
- 上车顺序：`GrabComponent->ForceRelease()`（不松手的话约束会把货物/别的玩家拖在车上）→ `StopRagdoll()`（关刚体并把网格挂回胶囊）→ 隐藏 + 关碰撞 + 挂到车上 → `Controller->Possess(bike)`。下车反过来，先摆好位置再 `StartRagdoll()`（它内部带 `PlaceOnGround`）。
- 骑手网格平时隐藏，有人骑才显示——模型自带骑手，不藏起来的话路边空车上永远坐着个人。
- 车体是 9 个 `UStaticMeshComponent` **固定槽位**（`MaxBodyParts=12`，构造函数里建好），按 `BodyMeshes` 数组填充。没用运行时 `NewObject` 建组件，避免构造脚本反复重建/丢实例覆盖。

**资产接入（`Content/Python/setup_motorbike.py`，菜单 Delivery → Setup Motorbike）** ——
导入 FBX、建 `BP_Motorbike`、配 `IA_Interact` + IMC_Default 的 F 键、在当前关卡放一辆，四步幂等可单独重跑。
这份 `摩托车.fbx` 有三个必须知道的坑：
1. **坐姿写在骨骼的当前变换里，不在网格顶点里。** 绑定姿势（Cluster 的 TransformLink）是站姿，骨骼节点的当前变换才是坐姿。所以骑手只能按**骨骼网格**导入（参考骨架取自节点变换=坐姿）；按静态网格导入会得到一个站在车里的人。
2. **车体那 9 个网格没有蒙皮、挂在场景根节点下**，骨骼网格导入器会跳过它们。所以必须一份 FBX 导两次。静态那次用 `combine_meshes=False`（不然会和站姿骑手焊成一块）+ `transform_vertex_to_absolute=True`（顶点留在场景绝对坐标里，于是 9 个组件都摆在相对零点就能原样拼回整车）。脚本里有检测：部件原点全挤在 10cm 内就是这个选项没生效，会在报告里点名。
3. UE 5.8 默认用 **Interchange** 接管 FBX 导入，那条路**不读 `FbxImportUI`**。`AssetTools::ImportAssetTasks` 里 `bUseInterchangeFramework = IsInterchangeImportEnabled() && (SpecifiedFactory == nullptr)`，所以任务必须显式 `task.factory = unreal.FbxFactory()` 才会回到老导入器、上面那些选项才有人读。
- `IMPORT_SCALE = 1.9`：FBX 里骑手站立高度 100 单位，游戏角色胶囊 96 半高（约 192cm）。车体和骑手必须同一个值。
- `IA_Interact` 是**复制 `IA_Jump`** 建出来的——`InputAction` 没有暴露给 Python 的工厂。C++ 侧 `InteractAction` 用 `TSoftObjectPtr` 晚绑而不是 `ConstructorHelpers`，因为构造函数只在模块加载时跑一次，脚本新建的资产在同一次会话里永远解析不到。

## 代码结构

```
Source/Delivery/
├── DeliveryCharacter.{h,cpp}
├── DeliveryGameMode.{h,cpp}          最小 GameMode，具体逻辑在蓝图里
├── DeliveryGameState.{h,cpp}         承载全局共享状态：任务管理器 + 电话队列
├── DeliveryPlayerController.{h,cpp}
├── Combat/                           姿势/类型定义、战斗接口、单测
├── GAS/                              ASC、AttributeSet、PlayerState、Tags、Abilities/
├── Grab/                             双键抓取：抓取组件、可抓取组件、可抓道具
├── Interaction/                      走近按 F：可交互组件、探测组件、Slate 浮窗子系统
├── Ragdoll/                          主动布娃娃、布娃娃战斗组件
├── Traffic/                          交通车辆辅助组件（卡住重置循环、前车避让减速）
└── Vehicle/                          可骑载具（摩托车，运动学街机式）

Plugins/PS2DEMImporter/               地形转 landscape spline 道路插件
Content/Python/                       编辑器 Python 工具链：车道/路口/转弯生成、
                                      地形压平、裙边，以及一整套只读诊断脚本。
                                      入口 rebuild_traffic.py，见上面 5b 节
Content/Blueprint/                    角色/GameMode/PlayerController 蓝图（C++ 与资产的粘合层）
Document/Ragdoll.html                 布娃娃系统设计文档（原理+对照表+实现步骤+建模要求）
Document/TaskSystem.md                任务系统设计文档（状态机+类结构+接口清单+配置方式）
```

## 给 agent 的约定

- **每次新增或修改功能后，必须同步更新本文件（以及 [CLAUDE.md](CLAUDE.md)）里对应的模块说明**——"目前已实现的内容"这一节要跟着代码变化，不要让文档停留在旧状态。哪怕只是一个组件职责的调整，也要顺手补一句，否则后续 agent 会依据过时信息做判断。
- **改任务系统前先读 [Document/TaskSystem.md](Document/TaskSystem.md)**：任务状态全局共享挂在 GameState、计时靠服务器时间戳而非 Tick 累加，这两点是规则（"多人全局同步"、"掉落/换手/晕倒计时不停"）的实现方式，不要改成每玩家一份或每帧累加。
- **改布娃娃/战斗参数前先读 [Document/Ragdoll.html](Document/Ragdoll.html)**：这套系统刻意区别于常规 UE 摔倒教程（持续模拟、力驱动位移、约束抓取、电机强度随状态切换），不要按"降低弹簧数值"的思路去改。
- 中文注释里经常在解释"为什么这样设、调错了会变成什么"（尤其 `FDeliveryArmPoseSettings`），改参数前先读注释里说明的耦合关系。
- GAS 的 AbilitySystemComponent 挂在 PlayerState 上而非 Character，涉及网络复制或 `GetAbilitySystemComponent` 的改动要留意这一点。
- Git LFS 管理二进制资源；`Binaries/`、`Intermediate/`、`Saved/`、`DerivedDataCache/`、`*.sln`、`.vs/` 已被忽略，不要提交生成产物或强制 add。
- 这是团队协作的 Unreal 工程（GitHub: GDAT6402Group1Team1/Deliver），很多逻辑最终落在蓝图（`Content/Blueprint/*`）里，C++ 只暴露必要的 `TSubclassOf`/`UPROPERTY` 插槽给蓝图配置，改 C++ 时注意对应蓝图是否需要同步调整。
