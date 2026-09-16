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
