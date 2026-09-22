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

### 任务系统（电话接任务）
完整的结构、接口清单、配置方式、调用链和待确认假设见 **[Document/TaskSystem.md](Document/TaskSystem.md)**，改动前先看这份。要点：

- 任务状态全局共享（多人下解锁/来电/计时/完成对所有人是同一份），挂在 GameState 上的 [DeliveryTaskManagerComponent](Source/Delivery/Task/DeliveryTaskManagerComponent.h)；"当前追踪哪个任务"是每玩家各自的，挂在 PlayerState 上的 [DeliveryTaskTrackerComponent](Source/Delivery/Task/DeliveryTaskTrackerComponent.h)。
- 计时用服务器时间戳而不是 Tick 累加：取件时记一个时间戳，之后掉落/换手/进后备箱/晕倒都碰不到计时，天然满足"计时不停"。
- "同时只有一个进行中任务"不是状态，是 `CanAcquireItem()` 里的取件规则。
- 四个状态无失败态（Locked / AwaitingPickup / InProgress / Completed），超时只打一次催促电话并降低奖励倍率，任务继续。
- 电话队列 [DeliveryPhoneCallQueueComponent](Source/Delivery/Task/DeliveryPhoneCallQueueComponent.h) 也在 GameState 上，由服务器按配置时长推进，不等客户端播完回报。
- 单个任务的配置是一份 [DeliveryTaskDefinition](Source/Delivery/Task/DeliveryTaskDefinition.h) 资产；关卡的任务清单填在 GameState 蓝图的 `TaskDefinitions` 上（数组顺序 = 同时解锁时的来电顺序）。
- 任务数值由策划在 `Design/Tasks.csv` 里维护，编辑器菜单 **Delivery → Import / Reimport Tasks** 导入成 DataAsset（脚本在 `Content/Python/delivery_task_import.py`，按 TaskId 增量更新，不会冲掉资产上手填的字段）。时间评价档位用**剩余秒数**表达，正数提前、负数超时，和策划表一一对应。
- 调试用控制台命令（`Delivery.Task.Dump` / `Acquire` / `Deliver` / `Event`）见 [DeliveryTaskDebugCommands.cpp](Source/Delivery/Task/DeliveryTaskDebugCommands.cpp)，在 PIE 里不用 UI 就能跑完整个任务流程；`DeliveryTaskDefinition` 有 `IsDataValid` 校验，阈值配反、档位顺序错会在编辑器里标红。
- UI、背包/交互、金钱、存档都还没做，对接点见文档第七节。

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
  - 排查后发现光改容差还不够：车被 `OnRouteLost` 瞬移回出生点时，原来只重置了位置/朝向、调用 `NotifyResetToStart()` 清计时，**`CurrentSpeed` 完全没被碰过**——如果重置前车速已经很高，瞬移后是直接带着这个速度"起步"的，不是从 0 慢慢加速，`TraceForIntersection` 里那个和车速成正比的前瞻探测点因此也会立刻跳得很远，更容易和路口对不上。现在 `OnRouteLost` 处理链路补全成真正的"全部归位"：`SetCurrentSpeed(0)` → `SetCurrentFollowSpline(None)` → `SetStoppedAtIntersection(None)` → `SetStopatInter(false)`（车自己那个局部变量，读的是上一次 `TraceForIntersection` 检查时的灯状态，一起清掉避免留着上一次的判断结果） → 清掉可能还没触发的 `GreenPollTimer`，再做原来的位置/朝向重置——但归零 `CurrentSpeed` 之后必须有东西重新武装 `Acceleration`，一开始漏了这步导致部分车重置后永久停在 0（后面在 `NotifyResetToStart()` 之后补了一次和 `PollForGreen` 一样的 `SetTimerByEvent(Acceleration, 循环)` 才修好）。要求是重置后所有参数都要和刚 `BeginPlay` 时完全一样，核对了一遍发现 `NotifyResetToStart()` 只清了 `bIsCurrentlyOnRoute`/`RouteLostElapsedTime`，组件自己的 `SpeedMultiplier`（避让系数，构造时默认 1.0）和 `BlockedElapsedTime`（卡车计时，默认 0）没清——补上了，避免车瞬移回出生点后还带着重置前"正在避让/正在卡死计时"的残留状态。另外确认过 `MaxSpeed`/`DecelerationDistance`/`AcelerationPerSecond`/`UpdateFrameRate` 这四个纯配置变量整张蓝图里都没有 Set 节点改过它们，本来就恒定，不需要重置。
  - 排查"重置后闯红灯"定位到一个根子：车靠 `TraceForNewPath` 重新接路径（出生时、或者 `OnRouteLost` 之后都会走这条）时，原来探测到东西就直接 `SetCurentSplineComp` 焊上去，**从不检查焊上的这段是不是归红绿灯管**。另外确认过一个之前怀疑过、后来用数据推翻的方向：`TraceForIntersection` 的探测查的是 `ObjectTypeQuery7`（对应项目里自定义的 `Traffic_Road` 通道），路口 `Box` 碰撞体的 `ObjectType` 正好也是这个，车辆自己碰撞体是内置的 `ECC_Vehicle`——两者不在同一通道，探测线物理上不会扫到前车，"探测被前车挡住"不成立。
    - **曾经尝试过的修法（已撤销，暂不生效）**：让 `TraceForNewPath` 也接一遍和 `TraceForIntersection` 一样的判断（cast 成 `BP_TrafficLine1_IntersectionChild`、读 `StopAtInter`、红灯先 `StopCar` 再焊样条）。带出了一个新问题：`StopCar` 触发 `Deceleration` 时，`BeginPlay` 时武装的 `Acceleration` 循环计时器可能还在跑，两个计时器互相拉扯，实测出现过车速该降却直接冲到 `MaxSpeed` 的情况；补了一个 `StopCar` 开头 `ClearTimerByFunctionName(self, "Acceleration")` 先停掉加速计时器。但改完之后车虽然不再直接闯灯，却会停在"当前样条终点之后"而不是路口那段样条上（重置前的行为是停在路口样条线上）——为了不在没搞清楚前继续叠加改动，**这两处（`TraceForNewPath` 接灯检查 + `StopCar` 里的 `ClearTimerByFunctionName`）已经从 `TraceForNewPath`/`StopCar` 里撤回**，蓝图现在等价于"重置后闯红灯"问题还没修那一版，下次要重新想思路再改。
    - 补充排查发现：**道路样条和路口样条本来就是断开的**，两段之间 `CurrentFollowSpline` 短暂变 `None` 是设计如此、不是 bug。但实测抓到过一次一辆车明明骑在路口自己的样条上（`StoppedAtIntersection` 指向的路口当时确认是红灯），`CurrentFollowSpline` 变 `None` 的前后两次查询里 `CurrentSpeed` 全程都是 600、完全没降过——说明就算 `TraceForIntersection`/`StopCar` 确实触发了，`Deceleration` 也可能根本没起作用，大概率还是没有互斥的 `Acceleration`/`Deceleration` 计时器竞态在作怪（就是上面那条被撤销的 `ClearTimerByFunctionName` 想解决的问题），只是这次没有再改代码验证，先记录着。
    - **已用埋点日志定位到真正的根因（2026-09-17）**：在 `TraceForIntersection` 的红/绿灯分支上各串了一个 `PrintString`（`"RED - StopCar"` / `"GREEN - no stop"`，绿灯那条仍然是纯打印、不接任何逻辑，保留"else 是死路"的原设计），在 `Deceleration` 和 `Acceleration` 两个自定义事件开头各串了一个 `PrintString`（`"Deceleration TICK"` / `"Accel TICK"`），全部是只读埋点、不改变任何行为。跑 PIE（Simulate 模式，关卡里 3 辆车）看日志时间戳，结论是：
      - "车闯红灯"跟 `TraceForIntersection` 没关系——日志里 `RED - StopCar` 每帧都在正常打印，探测和 cast 都命中了，`StopCar` 也确实被调用了。
      - 真正的问题在 **`Acceleration` 的循环计时器清不掉**。`Acceleration` 事件内部那个 `ClearandInvalidateTimerbyHandle`（`K2Node_CallFunction_13`）的 `Handle` 引脚，是**直接连线**到 `Drive` 事件里那个 `SetTimerbyEvent` 节点（`K2Node_CallFunction_3`）的返回值上的。跨事件的数据连线在蓝图里会被编译成"缓存上一次执行结果"的隐藏变量，而 `K2Node_CallFunction_3` 只在 `BeginPlay → Drive` 里跑过一次——所以 `Acceleration` 到达 `MaxSpeed` 时，**只能清掉 BeginPlay 那一次武装的计时器**。
      - 后来补的另外两处 `SetTimerbyEvent(Acceleration)`——`PollForGreen` 绿灯重新起步那个（`K2Node_CallFunction_20`）和 `OnRouteLost` 重置末尾那个（`K2Node_CallFunction_23`）——**返回的 handle 根本没有存到任何地方**。于是这两条路武装出来的加速计时器是**永远清不掉的**：到了 `MaxSpeed` 之后它还在每 0.1 秒跑一次 `CurrentSpeed = MaxSpeed`，一直跑到关卡结束。
      - 这就完整解释了症状：第一波车走的是 `Drive` 武装的那个计时器，handle 对得上、到 `MaxSpeed` 就自清，所以之后 `StopCar → Deceleration` 没人干扰，能正常减速停住（日志实测 4 个 `Deceleration TICK` 减到 0）。但**只要车停过一次再起步**（不管是绿灯放行还是 `OnRouteLost` 重置），新的加速计时器就再也停不下来，之后每次遇红灯 `Deceleration` 和 `Acceleration` 会在同一帧里交替出现、互相拉扯（日志实测同一帧内 `RED - StopCar` / `Accel TICK` / `Deceleration TICK` 三者混在一起），车速被反复顶回 `MaxSpeed`，表现就是"短暂减速然后恢复速度通过"、轮询查到的 `CurrentSpeed` 永远是 600。
      - 也就是说这个 bug **不是"重置"独有的**，绿灯起步后同样中招；之前之所以觉得只有重置后的车会闯灯，是因为观察窗口正好落在重置那一段。同时也解释了为什么"时序窗口"理论解释不通——这是个**永久性的状态差异**（车在不在原始那条可清除的加速计时器上），不是时间竞态。
      - **已修复（2026-09-17）**：新增了一个 `FTimerHandle` 类型的蓝图成员变量 `AccelTimer`，三处武装加速计时器的地方全部改成“**先 Clear 再 Set**”的形式——`Drive`（`SetDriveForwardTimer` 之后）、`PollForGreen`（清掉 `GreenPollTimer` 之后）、`OnRouteLost`（`NotifyResetToStart` 之后），每处都是 `ClearandInvalidateTimerbyHandle(AccelTimer)` → `SetTimerbyEvent(Acceleration, 0.1, 循环)` → `SetAccelTimer(返回值)`；`Acceleration` 事件里那个 `ClearandInvalidateTimerbyHandle` 的 `Handle` 引脚，从原来直连 `K2Node_CallFunction_3` 返回值改成读 `GetAccelTimer`。“先 Clear 再 Set”这一步是必须的：三处共用同一个变量，如果只 Set 不 Clear，反复重置时旧 handle 会被新值冲掉、旧计时器却还在跑，等于把同一个 bug 换个形式留下来。`StopCar` 本身**没有改动**（之前撤销过的那个 `ClearTimerByFunctionName` 没有重新加）——修好 handle 之后加速计时器到 `MaxSpeed` 就会自清，红灯时本来就不该有加速计时器在跑，不需要再加一道互斥。
      - 修复效果（同一套埋点，PIE 前后对比，3 辆车）：修复前单次加速突发最长能连续跑 **9.4～13.0 秒**（即永不停止），`Accel TICK` 和 `Deceleration TICK`/`RED - StopCar` 在同一帧里打架的情况每辆车有 3～7 帧；修复后三辆车的最长加速突发**一致收敛到 1.36 秒**（就是 0 加速到 `MaxSpeed` 然后自清所需的时间），同帧冲突 **0 帧**，每次红灯的 `Deceleration TICK` 都是稳定 4 次减到 0。
      - 验证完成后那 4 个调试用的 `PrintString` 埋点（`RED - StopCar` / `GREEN - no stop` / `Deceleration TICK` / `Accel TICK`）**已全部删除**，删时把上下游重新直接接回去了，`TraceForIntersection` 里 `IfThenElse_1` 的 `else` 分支也恢复成原来“什么都不接”的死路。下次再排查交通问题时可以按同样的方式临时加回来：串联插入、只打印、不改任何行为逻辑，用日志自带的时间戳理清真实时序，比靠 PIE 轮询属性猜可靠得多。
      - 还没做的验证：`OnRouteLost` 那一处只做了节点接线核对 + 编译通过，**没有运行时实证**——因为日志里分不出某次重新起步到底是绿灯放行还是 `OnRouteLost` 重置（两条路都没有专属日志标记）。`Drive` 和 `PollForGreen` 两条路径是有运行时证据的。如果以后要补这个验证，在 `OnRouteLost` 里加一个 `PrintString "RESET"` 跑一轮就行。
  - **避让检测一直是死的（2026-09-17 修复）**：`TraceChannel` 默认值是 `ECC_WorldDynamic`，但 `BP_car_base` 的 `Box` 碰撞盒对 `WorldDynamic` 的响应是 **Ignore**（它的 ObjectType 是 `ECC_Vehicle`，只对 `Vehicle` 和 `Traffic_Road` 是 Overlap）。`SweepMultiByChannel` 按"被击中物体对该通道的响应"筛选，Ignore 直接跳过——所以 `IsCarAhead()` **恒为 false**，"前车减速"这套逻辑从来没生效过，后车当然会直接怡上去。已把 C++ 默认值改成 `ECC_Vehicle`；**注意光改 C++ 默认值不够**，蓝图组件模板（`BP_car_base_C:Default__DeliveryTrafficCarComponent`）和关卡里的车实例都把旧值序列化过了，还得把模板 `set_properties` 成 `ECC_Vehicle`、再对每个实例 `reset_properties` 清掉覆盖才能生效。
  - **裸奔计时改为从满速才开始算（2026-09-17）**：新增 `UpdateSpeedState(InCurrentSpeed, InMaxSpeed)`，由 `DriveForward` 每帧调用。只有 `bHasReachedMaxSpeed` 为 true 时才累计 `RouteLostElapsedTime`，掉速（红灯刹车/避让减速/刚被重置）就清零重新计。原因：车刚重置完或刚从红灯起步时速度是 0，这段时间它本来就还没接上样条（要靠 `TraceForNewPath` 现找），若从脱离那一刻就开始计时，很可能在还没加速起来时又被判定裸奔超时，反复自我重置。
  - **红灯期间的避让死锁超时放宽到 8 秒（2026-09-17）**：新增 `MaxBlockedTimeAtIntersection = 8.0f` 和 `UpdateStoppedAtIntersection(bool)`。蓝图在 `TraceForIntersection` 的 `SetStopatInter` 之后、以及 `OnRouteLost` 的 `SetStopatInter(false)` 之后各调一次。红灯时前车是"合法地长时间不动"、不是死锁，还按 `MaxBlockedTime`（3 秒）就放弃避让的话，后车会在红灯没结束时压上去；`LightDuration` 是 3 秒，8 秒足够覆盖一整个红灯。
  - 车辆碰撞盒保持 `Overlap` **没改成 `Block`**：车是运动学的（`bSimulatePhysics=false`，靠 `SetActorLocation` 沿样条走），Block 在不开 sweep 的情况下根本不起作用；而开了 sweep 会和样条跟随打架（样条要它往前、sweep 把它顶回来）导致抖动。"后车停在后方不超车"靠的是修好之后真正生效的前车探测 + 减速，不是物理阻挡。

- 测试地图：`Content/Level/TestForCharacter.umap`（角色/互殴测试）、`Content/Level/testfortraffic.umap`（交通路口测试）。


### 交通线生成工具链（`Content/Python`，编辑器 Python）
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

### 人物美术
- 已导入测试角色模型，最近有一版人物相关提交（"人物"）。布娃娃对建模的要求见 Ragdoll.html 第五节：约 15–18 根骨骼、四肢截面需容纳胶囊碰撞体、不用人体解剖骨骼/标准 Mannequin。

### 交互系统（走近按 F）
通用框架，摩托车是第一个用户，都在 `Source/Delivery/Interaction/`。**和 `Grab/` 是两回事**：
Grab 是双键按住、用物理约束把东西抓在手上的连续动作；这里是一次性的按键交互。
- [DeliveryInteractableComponent](Source/Delivery/Interaction/DeliveryInteractableComponent.h) —— 挂在可交互 Actor 上，持有提示词/半径/浮窗高度，交互时广播 `OnInteract`。查找走**静态注册表**而不是球形 Overlap：可交互物就几个，遍历代价可忽略，而碰撞查询在本项目已经栽过一次（`DeliveryTrafficCarComponent` 的前车探测通道配错、恒为空，肉眼完全看不出来）。注册表没有这个失败模式。
- [DeliveryInteractionProbeComponent](Source/Delivery/Interaction/DeliveryInteractionProbeComponent.h) —— 挂在玩家 Pawn 上（`ADeliveryCharacter` 构造函数里已加），10Hz 探测最近目标并推浮窗。只在 `IsLocallyControlled()` 的 Pawn 上跑，所以上车后被丢在车上的那具身体不会再提示。
- [DeliveryPromptSubsystem](Source/Delivery/Interaction/DeliveryPromptSubsystem.h) —— 浮窗本体，**C++ Slate 直接挂视口，没有 WBP 资产**。中文靠 Slate 自带的字体回退渲染（引擎自带 `DroidSansFallback.ttf`），不用额外导字体。调用约定是"每帧推一次"，停推 0.25 秒自动消失——调用方因此不需要成对写 Show/Hide，也就不会因为某条退出分支漏掉 Hide 把提示永久留在屏幕上。世界坐标→屏幕坐标用 `UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition`（自带 DPI 折算），为此 `Delivery.Build.cs` 加了 `Slate`/`SlateCore`/`UMG`。
- 按键走服务器复核：客户端 `ADeliveryCharacter::DoInteract()` 把本地探到的目标发 `ServerInteract(Target)`，服务器重新查组件 + 距离才执行。

### 可骑摩托车
[DeliveryMotorbike](Source/Delivery/Vehicle/DeliveryMotorbike.h) —— **运动学街机式载具，不是 Chaos Vehicle**。
- 为什么不用 Chaos：美术资产是一整套静态网格 + 一个坐姿骑手，没有轮子骨骼、没有物理资产，Chaos 要的东西一样都没有，硬上等于要先回 Blender 重新绑定；而且关卡里的交通车（`BP_car_base`）本来就是运动学沿样条走的，玩家车用同一套假设，不会出现"玩家车被物理弹飞、AI 车纹丝不动"这种两套世界观打架。
- 每帧自己算速度/转向。水平位移带 sweep 挡墙（撞上掉速不弹开）；竖直方向打射线贴地，**竖直这一步不能用 sweep**——sweep 会被地面挡住，和"我要贴到地面上"互相打架。转向量乘 `Speed/TurnSpeedReference`，所以停着打把不会原地转圈。侧倾只改 `MeshRoot` 的 Roll，不碰碰撞和行驶（UE 里正 Roll 是往左倒，所以右转取负值）。
- 上车顺序：先 `GrabComponent->ForceRelease()`（手里还抓着东西的话，约束会把货物/别的玩家一路拖在车上）→ `StopRagdoll()`（关刚体模拟并把网格挂回胶囊，不停的话被挂到车上的身体会一路抽搐）→ 隐藏 + 关碰撞 + 挂到车上 → `Controller->Possess(bike)`。下车反过来，先摆好位置再 `StartRagdoll()`（它内部带 `PlaceOnGround`，顺序反了人会掉在原地）。
- 骑手网格平时隐藏，有人骑才显示——模型自带骑手，不藏起来路边空车上永远坐着个人。
- 车体是 9 个 `UStaticMeshComponent` **固定槽位**（`MaxBodyParts = 12`，构造函数里建好），按 `BodyMeshes` 数组填充。没用运行时 `NewObject` 动态建组件，避免构造脚本反复重建组件/丢实例覆盖。

### 摩托车资产接入（`Content/Python/setup_motorbike.py`）
菜单 **Delivery → Setup Motorbike**，或控制台 `py setup_motorbike.py`。四步幂等、可单独重跑：
导入 FBX → 建 `IA_Interact` 并在 IMC_Default 上映射 F → 建 `/Game/Vehicle/Motorbike/BP_Motorbike` → 在当前关卡出生点前方放一辆。报告写到 `Saved/setup_motorbike.txt`。

这份 `摩托车.fbx`（工程根目录）离线解析出来的三个坑，改导入流程前先读：
- **坐姿写在骨骼的当前变换里，不在网格顶点里。** 绑定姿势（Cluster 的 `TransformLink`）是站姿——膝盖在髋正下方；骨骼节点的当前变换才是坐姿——大腿前伸下压 131°、小腿回折 55°、两手落在把手宽度上。所以骑手只能按**骨骼网格**导入（UE 的参考骨架取自节点当前变换 = 坐姿）；按静态网格导入只有原始顶点 = 站姿，会得到一个站在车里的人。
- **车体那 9 个网格没有蒙皮、挂在场景根节点下**（不在骨架层级里），骨骼网格导入器会直接跳过。所以一份 FBX 必须导两次：骨骼网格拿骑手，静态网格拿车体。静态那次用 `combine_meshes=False`（不然会和站姿骑手焊成一块、永远分不开）+ `transform_vertex_to_absolute=True`（顶点留在场景绝对坐标里，于是 9 个组件都摆在相对变换零点就能原样拼回整车，不用手工还原每个部件的相对位置）。脚本里有检测：部件原点全挤在 10cm 以内就说明这个选项没生效，会在报告里点名并给手动兜底步骤。
- **UE 5.8 默认用 Interchange 接管 FBX 导入，那条路不读 `FbxImportUI`。** `AssetTools::ImportAssetTasks` 里 `bUseInterchangeFramework = IsInterchangeImportEnabled() && (SpecifiedFactory == nullptr)`，所以导入任务必须显式 `task.factory = unreal.FbxFactory()` 才会回到老的 FBX 导入器、上面那堆选项才有人读。

其他两条：
- `IMPORT_SCALE = 1.9` —— FBX 里骑手站立高度 100 个单位，游戏角色胶囊 96 半高（约 192cm）。车体和骑手必须用同一个值，否则比例会错。车体在 FBX 单位下是 135×88×72，乘 1.9 约 256×168×136 cm。
- `IA_Interact` 是**复制 `IA_Jump`** 建出来的——`InputAction` 没有暴露给 Python 的工厂，`create_asset` 那条路不通。C++ 侧 `InteractAction` 用 `TSoftObjectPtr` 晚绑而不是 `ConstructorHelpers`：构造函数只在模块加载时跑一次，脚本这次会话里新建的资产永远解析不到，晚绑才能当场生效。

## 代码结构速览

```
Source/Delivery/
├── DeliveryCharacter.{h,cpp}        角色 Pawn，输入、组件组装
├── DeliveryGameMode.{h,cpp}         最小 GameMode，具体类在蓝图里配
├── DeliveryGameState.{h,cpp}        承载全局共享状态（任务管理器 + 电话队列）
├── DeliveryPlayerController.{h,cpp}
├── Combat/                          战斗类型、姿势定义、战斗接口
├── GAS/                             AbilitySystemComponent、AttributeSet、PlayerState、GameplayTags、Abilities/
├── Grab/                            双键抓取：抓取组件、可抓取组件、可抓道具
├── Interaction/                     走近按 F：可交互组件、探测组件、Slate 浮窗子系统
├── Ragdoll/                         主动布娃娃、布娃娃战斗组件
├── Traffic/                         交通车辆辅助组件（卡住重置循环、前车避让减速）
└── Vehicle/                         可骑载具（摩托车，运动学街机式）
```

`Content/Python/` 是编辑器 Python 工具链（车道/路口/转弯生成、地形压平、裙边、以及一整套只读诊断脚本），
入口 `rebuild_traffic.py`，详见上面「交通线生成工具链」一节。

蓝图层：`Content/Blueprint/Character/BP_DeliveryMan`、`Content/Blueprint/GameMode/BP_DeliverGameMode`、
`Content/Blueprint/PlayerController/BP_DeliveryManPC` 等，负责把 C++ 组件和具体资产（GE、GA 子类、动画）接起来。

## 约定与注意事项

- **每次新增或修改功能后，同步更新本文件（以及 [AGENTS.md](AGENTS.md)）里对应的模块说明**，不要让文档停留在旧状态。哪怕只是改了一个组件的职责或新增一个系统，也要在"目前做了什么"里补一句，保持文档和代码同步。
- 代码注释以中文为主，且偏"解释为什么"而不是"解释是什么"（尤其是 `FDeliveryArmPoseSettings` 和 Ragdoll 相关代码），修改这些参数前先理解注释里说明的耦合关系（例如 WindupUp/WindupOutward 必须和 PunchReach 按比例对齐，否则出拳轨迹会跑偏）。
- 二进制资源走 Git LFS；`Binaries/` `Intermediate/` `Saved/` `DerivedDataCache/` `.sln` `.vs/` 已被 `.gitignore` 忽略，不要强制添加或提交生成产物。
- GAS 的 AbilitySystemComponent 挂在 PlayerState 而不是 Character 上，涉及网络复制/GetAbilitySystemComponent 相关改动时注意这一点（`OnRep_PlayerState` 里做了处理）。
