# CLAUDE.md

给 Claude Code 在本仓库工作时的上下文说明。

## 项目是什么

**Delivery**（欢乐向奇遇送快递游戏）。玩家扮演快递员，在风格各异的地区完成配送，
沿途可以对 NPC 和场景物件恶作剧。支持单人（升级推进）和多人（抢先送达抢单）。

- 引擎：Unreal Engine 5.8
- 详细的游戏简介、Git LFS 操作、克隆/同步/提交步骤见 [README.md](README.md)，不要在这里重复。

## 目前做了什么（按提交历史整理）

### 角色与移动：主动布娃娃（Active Ragdoll）
角色不是常规的 Character Movement 驱动，而是**持续物理驱动**——不是"平时动画、
摔倒才切物理"的教程式布娃娃，而是从 BeginPlay 开始就常驻 Simulate Physics，
走路、站立、摔倒、起身、抓取、互殴全部发生在同一条刚体链上。设计依据见
[Document/Ragdoll.html](Document/Ragdoll.html)（原理、与教程做法的对照表、七步实现步骤、建模要求都在里面，
改动前建议先读一遍）。

核心实现：
- [DeliveryActiveRagdollComponent](Source/Delivery/Ragdoll/DeliveryActiveRagdollComponent.h) —— 主动布娃娃的核心组件（直立参考体、电机强度、倒下/起身判定、坡度上的位移处理）。
- [DeliveryCharacter](Source/Delivery/DeliveryCharacter.h) —— 第三人称 Pawn，胶囊体+镜头，身体由 `ActiveRagdoll` 驱动；同时实现 `IAbilitySystemInterface` 和 `IDeliveryCombatInterface`。
- 跳跃、移动网络复制已实现（`ServerJump`、`ServerSetMoveInput`，带最小跳跃间隔限制）。

### 互殴系统（近战战斗）
- [DeliveryRagdollCombatComponent](Source/Delivery/Ragdoll/DeliveryRagdollCombatComponent.h) —— 在布娃娃刚体链上执行出拳。
- [DeliveryCombatInterface](Source/Delivery/Combat/DeliveryCombatInterface.h) —— 近战流程接口（`StartMeleeAttack` / `GatherMeleeHits` / `EndMeleeAttack` / `IsMeleeAttacking`），由 Character 实现，GA 调用。
- [DeliveryHandPose](Source/Delivery/Combat/DeliveryHandPose.h) / [DeliveryBoxingPose](Source/Delivery/Combat/DeliveryBoxingPose.h)（含单测 `DeliveryBoxingPoseTests.cpp`）—— 拳头握拳姿势、拳击收拳/直拳三段姿态的参数化定义，详见 [DeliveryCombatTypes.h](Source/Delivery/Combat/DeliveryCombatTypes.h) 里 `FDeliveryArmPoseSettings` 的大段中文注释（每个参数为什么这样设、调错了会变成什么效果——巴掌、手刀、横扫弧线等）。
- 左右出拳通过 GAS 技能触发：[GA_DeliverPunch](Source/Delivery/GAS/Abilities/GA_DeliverPunch.h)，对应 Gameplay Tag `Ability.Attack.Punch.Left/Right`。

### GAS（Gameplay Ability System）接入
- [DeliverAbilitySystemComponent](Source/Delivery/GAS/DeliverAbilitySystemComponent.h) 挂载在 [DeliverPlayerState](Source/Delivery/GAS/DeliverPlayerState.h) 上（而非 Character），随 PlayerState 复制。
- [DeliverAttributeSet](Source/Delivery/GAS/DeliverAttributeSet.h) —— 接入了属性表（血量/回血等），HP 回血 GE 当前隐藏。
- [DeliverGameplayTags](Source/Delivery/GAS/DeliverGameplayTags.h) —— Native Gameplay Tag：`Ability.Attack.Punch.Left/Right`、`Effect.Type.Damage`、`State.Stunned`。
- Character 上配置了 `HealthRegenEffect` / `DamageEffect`（Instant + SetByCaller Effect.Type.Damage）以及左右拳 GA 类，均在蓝图 `BP_DeliveryMan` 里指定。

### 地图与交通场景
- 已导入地图，接入 **PS2UE / PS2DEMImporter** 插件（PS2 地形/道路生成工具，见 `Plugins/PS2DEMImporter`）用于生成 landscape spline 道路。
- 交通路口、红绿灯（`trafficlight`）、道路与门的破碎效果等场景内容持续在搭建中（`Content/trafficlight`、`Content/PS2DEM` 下的 `BP_Intersection`、`BP_TrafficLine*` 等蓝图）。车辆沿样条行驶、Overlap 检测路口的核心逻辑在 `BP_car_base` 蓝图事件图里（C++ 侧没有基类）。
- [DeliveryTrafficCarComponent](Source/Delivery/Traffic/DeliveryTrafficCarComponent.h) —— 挂在 `BP_car_base` 上的辅助组件（已在编辑器里以 `Delivery Traffic Car` 组件接入，C++ 侧之前一度只编进了 Game 目标、编辑器看不到，重新编译 `DeliveryEditor` target 后才在编辑器注册），不接管移动，只做两件蓝图不好独立维护的事：
  - ①车辆按预设样条链接力行驶，走完所有预设路径、没能 Overlap 接上下一段车道时会脱离样条按最后方向"裸奔"冲出画面。这个状态对应 `BP_car_base::DriveForward` 里对 `CurrentFollowSpline` 的有效性判断（`IsValid` 分支）：有效分支正常跟随+调 `TraceForIntersection`，无效分支走直线裸奔+调 `TraceForNewPath` 重新搜路——蓝图在这两个分支上分别调用 `UpdateRouteFollowState(true/false)`，每帧告诉组件当前是否在跟随预设路线。连续脱离超过 5 秒（`RouteLostTimeout`）就广播 `OnRouteLost`；`BP_car_base` 在 `BeginPlay` 记录出生位置+朝向（`SpawnLocation`/`SpawnRotation`，两个新增变量），绑定 `OnRouteLost` 后把车瞬移回出生点（位置和朝向都要恢复，只重置位置会导致车用错误朝向继续搜路），再调 `NotifyResetToStart()` 清空计时——注意 `BP_car_base` 本身并没有"起始样条"这个概念（关卡里所有车实例的 `CurrentFollowSpline` 默认都是 `None`，全靠 `TraceForNewPath` 在出生点附近自己找路），所以重置到出生点位置朝向、交给现有自愈机制重新接管，比试图记一个不存在的"起始车道"更稳妥。
  - ②`TickComponent` 里做前方球形扫描，探测到前方另一辆挂了同组件的车时把 `GetSpeedMultiplier()` 平滑降到 0，蓝图在 `DriveForward` 调 `GetFuturePostionandRotationAlongSpline` 时把这个系数接到 `RelativeSpeedMult` 上，实现"遇前车减速到停、前车让开后恢复"。两条车道汇入同一点时双方会互相判定为"前车"、永久互相礼让形成死锁，所以加了 `MaxBlockedTime`（默认 3 秒）超时兜底：卡够久就放弃避让强行通过。探测距离/半径（`ForwardTraceDistance`/`ForwardTraceRadius`）调大过（600→1000、120→150）、速度系数变化速率（`SpeedMultiplierChangeRate`）调小过（1.5→0.6），让车能提前发现前车、慢慢减速，而不是靠近了才急刹。
  - `BP_car_base` 自身的车速逻辑（`CurrentSpeed`/`Acceleration`/`Deceleration`，都在 `EventGraph` 里，和本组件无关但一起排查/修过）：
    - `Deceleration`（`StopCar` 触发，`SetTimerByEvent` 每 0.1 秒循环调用一次）原来算新 `CurrentSpeed` 时误用了 `GetMaxSpeed`（应为 `GetCurrentSpeed`），且刹车所需的总减速量（`MaxSpeed² / (2×DecelerationDistance)`，v²=2ad）没有再乘 tick 间隔就整个减掉——等于每次都从 `MaxSpeed` 重新计算一次性砍掉全部减速量，`MaxSpeed=600`、`DecelerationDistance=100` 时算出来正好是 `600-1800=-1200`（红灯停车后变绿灯会看到速度瞬间变成 -1200 就是这个 bug；原来加的"小于 0 强制设 0"是有效的兜底，问题在于减速全程只有一个硬拐点，没有真正的渐变过程）。现在改成 `CurrentSpeed -= (MaxSpeed²/(2×DecelerationDistance)) × 0.1`，用当前速度渐进递减。
    - 更严重的问题：车速的 `Acceleration` 定时器（`SetTimerByEvent`，每 0.1 秒循环）只在 `BeginPlay` 里启动过一次，达到 `MaxSpeed` 后自己清掉，此后整张图里没有任何地方会再重新武装它——车一旦被 `StopCar`→`Deceleration` 刹停，就永远停在 `CurrentSpeed=0`，不会再起步。修法：`TraceForIntersection` 里 `DynamicCast` 成功时，除了读 `StopAtInter`，同时把转换出来的路口 Actor 存进新变量 `StoppedAtIntersection`；`Deceleration` 刹停并清掉自己的计时器后，启动一个新的自定义事件 `PollForGreen`（`SetTimerByEvent`，0.3 秒循环，计时器句柄存进 `GreenPollTimer`），每次轮询 `StoppedAtIntersection` 的 `StopAtInter`：还是 `true`（红灯）就继续等，变 `false`（绿灯）就清掉轮询计时器、重新 `SetTimerByEvent` 武装 `Acceleration`。全程只读路口 Actor 已经暴露的 `StopAtInter` 属性，没有改动 `BP_Intersection`/`BP_TrafficLine1_IntersectionChild` 那边的蓝图。
  - 排查过"车会闯红灯"：`TraceForIntersection` 里那条球形探测的终点，是靠"沿样条前瞻 30 个 tick 的未来点"和"当前样条最后一个点"做 `Equal(容差 10cm)` 判断决定要不要延伸过去的——车速正常在几百的量级时，两次调用之间这个未来点本身就会移动几十个单位，比 10cm 的判断窗口宽得多，很容易直接跳过这个窗口，探测线因此常年只在半路做"原地一个点"的检测、够不着路口碰撞盒；而 `IfThenElse_0`（判断"这次探测有没有扫到东西"）的 `else` 分支又是完全没接任何节点的死路——没扫到就什么都不做，等于这一帧路口检查被整个跳过，车照常往前开。已经把这个容差从 10 改成 100（和 `DriveForward` 自己判断"是否到样条终点"用的容差保持一致，代价很小、收益很大，不算调大）。
  - 排查后发现光改容差还不够：车被 `OnRouteLost` 瞬移回出生点时，原来只重置了位置/朝向、调用 `NotifyResetToStart()` 清计时，**`CurrentSpeed` 完全没被碰过**——如果重置前车速已经很高，瞬移后是直接带着这个速度"起步"的，不是从 0 慢慢加速，`TraceForIntersection` 里那个和车速成正比的前瞻探测点因此也会立刻跳得很远，更容易和路口对不上。现在 `OnRouteLost` 处理链路补全成真正的"全部归位"：`SetCurrentSpeed(0)` → `SetCurrentFollowSpline(None)` → `SetStoppedAtIntersection(None)` → 清掉可能还没触发的 `GreenPollTimer`，再做原来的位置/朝向重置。
- 测试地图：`Content/Level/TestForCharacter.umap`（角色/互殴测试）、`Content/Level/testfortraffic.umap`（交通路口测试）。

### 人物美术
- 已导入测试角色模型，最近有一版人物相关提交（"人物"）。布娃娃对建模的要求见 Ragdoll.html 第五节：约 15–18 根骨骼、四肢截面需容纳胶囊碰撞体、不用人体解剖骨骼/标准 Mannequin。

## 代码结构速览

```
Source/Delivery/
├── DeliveryCharacter.{h,cpp}        角色 Pawn，输入、组件组装
├── DeliveryGameMode.{h,cpp}         最小 GameMode，具体类在蓝图里配
├── DeliveryPlayerController.{h,cpp}
├── Combat/                          战斗类型、姿势定义、战斗接口
├── GAS/                             AbilitySystemComponent、AttributeSet、PlayerState、GameplayTags、Abilities/
├── Ragdoll/                         主动布娃娃、布娃娃战斗组件
└── Traffic/                         交通车辆辅助组件（卡住重置循环、前车避让减速）
```

蓝图层：`Content/Blueprint/Character/BP_DeliveryMan`、`Content/Blueprint/GameMode/BP_DeliverGameMode`、
`Content/Blueprint/PlayerController/BP_DeliveryManPC` 等，负责把 C++ 组件和具体资产（GE、GA 子类、动画）接起来。

## 约定与注意事项

- **每次新增或修改功能后，同步更新本文件（以及 [AGENTS.md](AGENTS.md)）里对应的模块说明**，不要让文档停留在旧状态。哪怕只是改了一个组件的职责或新增一个系统，也要在"目前做了什么"里补一句，保持文档和代码同步。
- 代码注释以中文为主，且偏"解释为什么"而不是"解释是什么"（尤其是 `FDeliveryArmPoseSettings` 和 Ragdoll 相关代码），修改这些参数前先理解注释里说明的耦合关系（例如 WindupUp/WindupOutward 必须和 PunchReach 按比例对齐，否则出拳轨迹会跑偏）。
- 二进制资源走 Git LFS；`Binaries/` `Intermediate/` `Saved/` `DerivedDataCache/` `.sln` `.vs/` 已被 `.gitignore` 忽略，不要强制添加或提交生成产物。
- GAS 的 AbilitySystemComponent 挂在 PlayerState 而不是 Character 上，涉及网络复制/GetAbilitySystemComponent 相关改动时注意这一点（`OnRep_PlayerState` 里做了处理）。
