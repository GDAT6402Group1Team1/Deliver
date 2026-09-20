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
车道线、路口段、左右转、路口盒全部由脚本沿道路样条生成，不手摆。入口是
**`rebuild_traffic.py`**，按固定顺序跑三步，顺序不能乱（后两步都依赖第一步的产物）：

1. `gen_traffic_lanes.py` —— 直行车道 + 路口段
2. `gen_intersections.py` —— 路口盒（按上一步 `Inter_*` 的 Box 位置算）
3. `fill_turn_splines.py` —— 左右转曲线（按上一步的样条端点算）

**"跑完车道忘了跑路口盒"已经造成过两次"红绿灯没了"**：路口盒是按 `Inter_*` 的 Box 位置算的，
车道一重新生成 Box 全部挪位，旧盒子罩不住新的路口段，`BP_Intersection` 就找不到要管的子节点。
把顺序固化进 `rebuild_traffic.py` 就是为了不再靠记性。

生成物按 tag 区分，可重复运行、互不误删：`ClaudeGenLane:<路名>`（车道+路口段，按路独立）、
`ClaudeGenIntersection`（路口盒）、`ClaudeGenTurn`（早期独立转弯 actor，已弃用）、`ClaudeGenSkirt`（裙边）。

关键参数都在 `gen_traffic_lanes.py` 顶部，每个都写了为什么是这个值：
- `CROSSING_WITH = "形状 13"` —— 自动算出要处理的路 = 这条路 + 所有与它相交的路，不手工维护名单
- `EXCLUDE_ROADS` —— 排除不铺车道的路（形状43 的源样条本身是坏的：2 个点、首尾同坐标、长度却有 1597）
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
- **转弯曲线用两条切线的交点当控制点**（二次贝塞尔）。"切线各伸出 k 倍距离"那种画法要猜 k，
  猜大了曲线在反面鼓出包、整条变成 S 形。交点法没有可调参数，而且交点落在来向后方时
  能自己报错，不会硬画一条怪线
- **`get_direction_at_distance_along_spline` 在这些样条上返回零向量**，方向要用样条上两个采样点作差求。
  兜底成 `(1,0,0)` 是有害的——它把"取不到方向"伪装成"方向朝 +X"，最后报出来的错误原因和真实原因毫无关系
- 转弯路径挂在路口段 actor **自带的 `SplineLeft`/`SplineRight`** 上，和直行共用同一个 Box（车探测到一个 Box 就拿到三条候选）。
  用不上的压成零长，蓝图判 `GetSplineLength() < 1` 跳过；不压的话会留下蓝图默认的 100cm 残桩，
  152 个路口段就是 300 根方向一律朝 +X 的小棍子散在全图

地形/路面配套（同目录）：
- `deform_terrain.py` —— 沿路把地形压平到"路面顶面 −40"。**压平带宽度按大纲文件夹取标称半宽**
  （主路 900 / 次路 540）与实测取大值、再加余量 150。早先只信实测的第 30 百分位，
  等于全路 70% 的断面比压平带宽、路缘外侧根本压不到，实测路缘埋没率 **37%**；改完降到 **3%**
- `thicken_road.py` —— 路面从 6cm 加厚到 26cm；`gen_road_skirt.py` —— 路面下铺绿色裙边遮住悬空的侧面
- 这几样都可能在撤销/重载关卡时丢失。**先跑 `check_road_state.py` + `diag_road_buried.py` 定位**：
  厚度和裙边都在、埋率却高 → 是地形压平丢了（它存在 landscape 的编辑图层里，前者查不到），单跑 `deform_terrain.py` 即可

诊断脚本一律只读、结果写到 `Saved/*.txt`：`diag_lane_gaps.py`（接缝间距 + 高度差）、
`diag_seams.py`（逐点查落差来源）、`whats_here.py`（某个 actor 附近都有哪些样条）、
`inventory_level.py`、`audit_traffic_channels.py`（碰撞通道）、`dump_bp_graph.py`（导出蓝图图表）、
`monitor_car.py` / `chase_car.py`（Simulate 期间采样车状态 / 跟拍相机——用每帧回调而不是 sleep 轮询，
Python 跑在游戏线程上，轮询会把模拟本身卡死）。

**还没解决的**：
- `BP_car_base` **没有"进路口走哪条"的逻辑**，直行/左转/右转三条候选它都看得见，
  抓到哪条取决于 `TraceForNewPath` 先扫到谁
- **`Lane_*` 和 `Inter_*` 的 Box 在同一个碰撞通道**（都是 `ECC_TRAFFIC_ROAD`），`TraceForIntersection` 分不出两者。
  更要紧的是同一路口里**别的路**的 `Inter_` 也能通过 Cast，车可能读到横向车流的灯态（灯正好相反）。
  通道分离解决不了这个（两者都是 `IntersectionChild`），得给命中结果加方向校验（同向 dot > 0.7）
- **`BP_Intersection` 怎么找它管的那些 `IntersectionChild` 至今没查清**，一直靠"重跑 `gen_intersections.py`"
  这种经验性修复。蓝图图表是能读也能改的（`BlueprintEditorLibrary` + `BlueprintGraphEditor` +
  `BlueprintGraphPin` 这套 API 在本机可用，`list_all_nodes` / `list_all_pins` / `list_connected_pins` /
  `try_create_connection` 都在），但这张图一直没成功导出过。注意 `UBlueprint` 的 `FunctionGraphs`
  和 `UEdGraph` 的 `Nodes` 都是 protected、`get_editor_property` 读不到，必须走上面那套 API

### 6. 人物美术
- 已导入测试角色模型；建模要求（骨骼数量、四肢截面需容纳胶囊碰撞体、不使用标准 Mannequin/人体解剖骨骼）见 Ragdoll.html 第五节。

## 代码结构

```
Source/Delivery/
├── DeliveryCharacter.{h,cpp}
├── DeliveryGameMode.{h,cpp}          最小 GameMode，具体逻辑在蓝图里
├── DeliveryGameState.{h,cpp}         承载全局共享状态：任务管理器 + 电话队列
├── DeliveryPlayerController.{h,cpp}
├── Combat/                           姿势/类型定义、战斗接口、单测
├── GAS/                              ASC、AttributeSet、PlayerState、Tags、Abilities/
├── Ragdoll/                          主动布娃娃、布娃娃战斗组件
└── Traffic/                          交通车辆辅助组件（卡住重置循环、前车避让减速）

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
