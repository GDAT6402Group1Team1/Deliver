# CLAUDE.md

给 Claude Code 在本仓库工作时的上下文说明。

## 项目是什么

**Delivery**（欢乐向奇遇送快递游戏）。玩家扮演快递员，在风格各异的地区完成配送，
沿途可以对 NPC 和场景物件恶作剧。支持单人（升级推进）和多人（抢先送达抢单）。

- 引擎：Unreal Engine 5.8
- 详细的游戏简介、Git LFS 操作、克隆/同步/提交步骤见 [README.md](README.md)，不要在这里重复。

## 目前做了什么（按提交历史整理）

### 角色与移动：主动布娃娃（Active Ragdoll）
下坡双脚防交叉：落点左右轴跟随 `CurrentFacingYaw`（身体朝向），不随 WASD 方向瞬时翻转；仅在着地可控状态对内滑的脚/小腿刚体施加有上限的水平纠偏加速度。迈步最后 15% 固定落点，最多额外等待 0.18 秒到位；停步后也允许纠正已交叉的脚。回归检查：`Delivery.Ragdoll.FootSeparation`。
角色不是常规的 Character Movement 驱动，而是**持续物理驱动**——不是"平时动画、
摔倒才切物理"的教程式布娃娃，而是从 BeginPlay 开始就常驻 Simulate Physics，
走路、站立、摔倒、起身、抓取、互殴全部发生在同一条刚体链上。设计依据见
[Document/Ragdoll.html](Document/Ragdoll.html)（原理、与教程做法的对照表、七步实现步骤、建模要求都在里面，
改动前建议先读一遍）。

核心实现：
- [DeliveryActiveRagdollComponent](Source/Delivery/Ragdoll/DeliveryActiveRagdollComponent.h) —— 主动布娃娃的核心组件（直立参考体、电机强度、倒下/起身判定、坡度上的位移处理）。髋目标从地面命中点抬起，站立高度取髋到视觉脚底的距离，以免停步悬空；前倾只在加速时施加，匀速下坡不持续前倾。
- 下坡姿态恢复：起步前倾只持续默认 0.4 秒，不再由速度误差维持；迈步最低直立点积降到 0.35，使中等前倾时还能补步找回支撑，完全倒下时仍禁止盲目迈步。
- 坡沿停步补脚若暂时探不到落点，保留补步请求到下一帧重试；失败时不再把请求直接清掉。
- [DeliveryCharacter](Source/Delivery/DeliveryCharacter.h) —— 第三人称 Pawn，胶囊体+镜头，身体由 `ActiveRagdoll` 驱动；同时实现 `IAbilitySystemInterface` 和 `IDeliveryCombatInterface`。
- 电瓶车受击：地面探测只查 `WorldStatic/WorldDynamic`，排除 `Vehicle`；离地暂停髋部世界空间电机，落地恢复。撞击镜头仅在受击者本机平滑拉远约 120 cm 并回归。
- 跳跃、移动网络复制已实现（`ServerJump`、`ServerSetMoveInput`，带最小跳跃间隔限制）。

### 互殴系统（近战战斗）
- [DeliveryRagdollCombatComponent](Source/Delivery/Ragdoll/DeliveryRagdollCombatComponent.h) —— 在布娃娃刚体链上执行出拳。
- [DeliveryCombatInterface](Source/Delivery/Combat/DeliveryCombatInterface.h) —— 近战流程接口（`StartMeleeAttack` / `GatherMeleeHits` / `EndMeleeAttack` / `IsMeleeAttacking`），由 Character 实现，GA 调用。
- [DeliveryHandPose](Source/Delivery/Combat/DeliveryHandPose.h) / [DeliveryBoxingPose](Source/Delivery/Combat/DeliveryBoxingPose.h)（含单测 `DeliveryBoxingPoseTests.cpp`）—— 拳头握拳姿势、拳击收拳/直拳三段姿态的参数化定义，详见 [DeliveryCombatTypes.h](Source/Delivery/Combat/DeliveryCombatTypes.h) 里 `FDeliveryArmPoseSettings` 的大段中文注释（每个参数为什么这样设、调错了会变成什么效果——巴掌、手刀、横扫弧线等）。
- 左右出拳通过 GAS 技能触发：[GA_DeliverPunch](Source/Delivery/GAS/Abilities/GA_DeliverPunch.h)，对应 Gameplay Tag `Ability.Attack.Punch.Left/Right`。

### 抓取系统（双手托举物品 + 单手物理拖人）
- [DeliveryGrabComponent](Source/Delivery/Grab/DeliveryGrabComponent.h)：左右键 0.2 秒组合抓取（原 0.12 秒，嫌窗口太紧不好按成"同时"而放宽），单键出拳，双键保持时持续寻找目标。普通物品通过服务器校验后立即视为双手持有；`FindUndersideGripPoints` 从任意物体底面两侧取真实碰撞支撑点，存为局部抓点；`CarryCenterWorld` 将抓点中点对齐胸口前的双手支撑位置，依据真实尺寸、Pivot 和缩放反推物体中心，所以物体位于手上方。首次抓取先将箱子对齐身体朝向；单人时双手追同一个胸口相对持物目标，不追箱子平滑移动后的延迟位置；双人争抢仍追共享箱子的实际抓点。拖晕倒玩家时由服务端选最近的一只手及目标身体碰撞表面，用服务端实际表面复核距离、朝向、眼位或相机视线；第一个抓人者通过遮挡检查后，把目标的整条物理刚体链平移到抓点贴手，再建立锁定的单手 Physics Constraint，不要求短手先碰到。第二个抓人者保留有限力的柔性 joint，靠近到 `DragAttachDistance` 再锁定，避免瞬移打断第一人的拖拽。双键触发，任意一键松开即释放。拖人期间髋目标平滑降低默认 25 cm、向抓点前倾默认 8° 以接近地面，不影响托举箱子。
- 拖人抓点与 joint 的目标侧锚点使用同一个物理刚体局部坐标，避免骨骼 socket 滞后；锁定 joint 启用 15 cm 容差的紧急投影，不施加持续投影力。服务器拖拽（`ApplyDragAssist`）分两步：①**被抓骨骼直接跟手**——每帧把它的线速度设成"手的速度 + 手与抓点缺口 / `DragGripResponseTime`（默认 0.05 s）"，上限 `DragGripMaxSpeed`（默认 1500 cm/s）；抓人者跳跃时保留该骨骼原竖直速度，被抓者不跟着跳。②**其余刚体跟随被抓部位**水平移动，按 `DragBodyFollowWeight`（默认 0.6）施加有上限的加速度（`DragFollowSpeed` 480 cm/s、`DragFollowAcceleration` 3200 cm/s²），被抓部位领先、其余部位带滞后跟上。肩到抓点超过 `DragMaxShoulderDistance`（默认 200 cm，如对方卡墙角）自动松手。抓点取物理胶囊表面后向该刚体质心收进 `DragGripInset`（默认 4 cm，最多一半深度），补偿胶囊比可见网格胖出的部分。服务器每秒打一条 `Drag assist: bone=… handGap=… handSpeed=… gripSpeed=…` 日志，`handGap` 应保持在几厘米内。第二名抓取者在柔性收拢阶段不参与，锁定后同样生效。
  **踩过的坑（2026-09-23，按时间顺序）**：(1) 牵引按"手骨骼速度 + 手到抓点误差"计算且只作用于被抓骨骼——joint 锁死后手钉在对方身上，两项恒为 0，拖不动，只有手臂被拉长；(2) 改成"肩膀为固定端的绳子 + 髋部速度前馈"后能拖，但锁定 joint 两端质量悬殊（约 1 kg 的手 vs 几十公斤趴地的人），Chaos 按质量比分摊误差，`handGap` 实测 25–80 cm，隔空拖；(3) 再加有上限的三维弹簧力把抓点拉向手，力一直打满仍被地面摩擦拖住，缺口随步速变大。结论：贴手这件事不能靠力或 joint 硬度，必须对被抓骨骼做速度伺服；参考量永远取抓人者自己的身体/手，不取被 joint 锁住后的相对量。
- [DeliveryGrabDebugCommands.cpp](Source/Delivery/Grab/DeliveryGrabDebugCommands.cpp) 提供非 Shipping 的 PIE 命令 `Delivery.Grab.TestSetup / TestGrab / TestStatus / TestPull / TestRelease`，分别准备倒地目标、走真实抓取路径、报告状态、后退 2 秒和释放；无需修改测试地图。
- 托举普通物品后才启用持物稳定姿态：胸和脊柱电机平滑增强（`CarryBraceStrength` 默认 20），手臂持物强度 `GrabStrength` 默认 22，不再使用出拳强度 39；关闭胸部步行摇摆，髋部摇摆保留 20%，转身限速默认 160°/s。普通行走、出拳、晕倒玩家拖拽不受影响。
- [DeliveryGrabbableComponent](Source/Delivery/Grab/DeliveryGrabbableComponent.h)：普通物品的可模拟物理 Primitive 要是 Actor 根；抓取时服务器暂时关闭物理，以默认 20 的速度平滑扫掠到胸口前方并平滑对齐身体朝向，暂时忽略 `Pawn`/`PhysicsBody` 碰撞，以免箱子把手臂顶开；释放后恢复物理、原碰撞响应并继承有限速度。携带状态与移动复制给客户端。两人抓同一物品时服务器取两个托举目标的中点及朝向合向量，正面对拉导致朝向合向量接近零时保持箱子现朝向；这是游戏化争抢，不是双方约束的真实力学拉扯。晕倒玩家仍全物理拖拽。[DeliveryGrabbableProp](Source/Delivery/Grab/DeliveryGrabbableProp.h) 默认测试质量 3 kg。NPC/背包/单手持有系统尚未接入。
- `LeftHandGap`/`RightHandGap` 在 PIE 显示手到物品表面的距离。镜头射线优先，身边准星附近的无遮挡目标可作为后备；默认 140 cm，本机 `CustomDepth/Stencil` + `M_GrabHighlight` 只提示一个未抓取候选目标，预测抓取或托举时清除高亮，松手重新瞄准才恢复；`DefaultEngine.ini` 开启 `r.CustomDepth=3`。测试蓝图 `/Game/Blueprint/Item/Test/BP_TestGrabBox` 可自行拖入地图，不占物品栏。不提交测试地图。

五格 Hotbar 用 `DeliveryInventoryItemComponent::Icon` 显示图片，不显示物品名或数字。测试斧头与快递的生成图标源图在 `Content/UI/Inventory/Source`，导入贴图在 `Content/UI/Inventory/Textures`；空格不显示图标，普通道具仍显示耐久条。

### GAS（Gameplay Ability System）接入
- [DeliverAbilitySystemComponent](Source/Delivery/GAS/DeliverAbilitySystemComponent.h) 挂载在 [DeliverPlayerState](Source/Delivery/GAS/DeliverPlayerState.h) 上（而非 Character），随 PlayerState 复制。
- [DeliverAttributeSet](Source/Delivery/GAS/DeliverAttributeSet.h) —— 接入了属性表（血量/回血等），HP 回血 GE 当前隐藏。
- [DeliverGameplayTags](Source/Delivery/GAS/DeliverGameplayTags.h) —— Native Gameplay Tag：`Ability.Attack.Punch.Left/Right`、`Effect.Type.Damage`、`State.Stunned`。
- Character 上配置了 `HealthRegenEffect` / `DamageEffect`（Instant + SetByCaller Effect.Type.Damage）以及左右拳 GA 类，均在蓝图 `BP_DeliveryMan` 里指定。

### 任务系统（电话接任务）
快递 E 交互保留 0.5 秒长按；探测目标与任务可取状态分开，未解锁、未登记、已完成或正在执行其他任务均显示原因。E 的 100cm 距离取角色与物品碰撞表面之间的实际间距。本机以 20Hz 从范围内候选中选准星附近且无遮挡的物体，小物体允许有限瞄准偏差，长按中的原目标有额外容差；服务端仍复核水平朝向、实际距离和到物体上半部的无遮挡视线，避免地板挡住指向物体原点的射线。F 路径不变。`Content/Python/diagnose_package_pickup.py` 可只读检查测试蓝图绑定、长按时间和任务引用。
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
- 旧关卡车实例的 `bReplicateMovement=false` 会覆盖蓝图默认值；交通组件在服务端和客户端均调用 `SetReplicateMovement(true)`，否则客户端会丢弃服务器位置更新。
- 撞击优先走 `DamageEffect` GE；若回复 GE 抵消本次扣血，服务器补正 HP，确保强撞必定触发晕倒。
- 强撞且受击者在地面时，髋部竖直速度一次性补到默认 400 cm/s（约 82 cm 高、0.8 秒飞行）；弱撞与空中再撞不追加升力。`StrongHitTakeoffSpeed` 可在蓝图组件上调，之后依靠重力落地。
- 上抛竖直速度变化要施加到髋以下整条刚体链；只给髋部会被全身约束分摊，视觉上几乎不腾空。
- 已导入地图，接入 **PS2UE / PS2DEMImporter** 插件（PS2 地形/道路生成工具，见 `Plugins/PS2DEMImporter`）用于生成 landscape spline 道路。
- 交通路口、红绿灯（`trafficlight`）、道路与门的破碎效果等场景内容持续在搭建中（`Content/trafficlight`、`Content/PS2DEM` 下的 `BP_Intersection`、`BP_TrafficLine*` 等蓝图）。车辆沿样条行驶、Overlap 检测路口的核心逻辑在 `BP_car_base` 蓝图事件图里（C++ 侧没有基类）。
- [DeliveryTrafficCarComponent](Source/Delivery/Traffic/DeliveryTrafficCarComponent.h) —— 挂在 `BP_car_base` 上的辅助组件（已在编辑器里以 `Delivery Traffic Car` 组件接入，C++ 侧之前一度只编进了 Game 目标、编辑器看不到，重新编译 `DeliveryEditor` target 后才在编辑器注册），不接管移动，只做两件蓝图不好独立维护的事：
- `BP_car_base` 现在只在服务器执行 `BeginPlay`/`Drive` 行驶链并复制 Actor 移动；地图里已摆放的旧车实例覆盖过 `bReplicates=false`，组件在服务器 `BeginPlay` 强制启用复制与移动复制，无需改地图。组件额外在服务器扫车辆轨迹命中角色 `PhysicsBody`，按有效质量与相对速度施加水平速度变化及 HP 伤害，同车同角色有 0.75 秒去重。强撞 HP 归零走现有晕倒/回血到 50% 起身流程（`StunRecoverHealthPercent`，原 40%，已放宽）。组件运行时让车身忽略 `Camera` 通道，场景遮挡不变；撞击参数可在蓝图组件详情中调。
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
4. **`unreal.Rotator` 的构造参数是 `(roll, pitch, yaw)`，和 C++ 的 `FRotator(Pitch, Yaw, Roll)`
   顺序不一样。** 把 yaw 填进第二个位置就变成 pitch，症状是 actor/组件被竖起来或者东倒西歪，
   而且"歪的角度"会跟着那个本该是 yaw 的值变——看着像随机，其实是确定的
   （摩托车就这么在地上立不住过一次）。`spawn_test_cars.py` 里的
   `unreal.Rotator(0.0, 0.0, yaw)` 是正确写法。**旋转出问题先数参数位置，
   别先怀疑坐标系或者轴映射**，后者要花几十倍的时间。

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
- [DeliveryInteractionProbeComponent](Source/Delivery/Interaction/DeliveryInteractionProbeComponent.h) —— 挂在玩家 Pawn 上（`ADeliveryCharacter` 构造函数里已加），20Hz 更新本机 E/F 目标及提示。只在 `IsLocallyControlled()` 的 Pawn 上跑，所以上车后被丢在车上的那具身体不会再提示。
- [DeliveryPromptSubsystem](Source/Delivery/Interaction/DeliveryPromptSubsystem.h) —— 浮窗本体，**C++ Slate 直接挂视口，没有 WBP 资产**。中文靠 Slate 自带的字体回退渲染（引擎自带 `DroidSansFallback.ttf`），不用额外导字体。调用约定是"每帧推一次"，停推 0.25 秒自动消失——调用方因此不需要成对写 Show/Hide，也就不会因为某条退出分支漏掉 Hide 把提示永久留在屏幕上。世界坐标→屏幕坐标用 `UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition`（自带 DPI 折算），为此 `Delivery.Build.cs` 加了 `Slate`/`SlateCore`/`UMG`。
- 按键走服务器复核：客户端 `ADeliveryCharacter::DoInteract()` 把本地探到的目标发 `ServerInteract(Target)`，服务器重新查组件 + 距离才执行。

### 可骑摩托车
[DeliveryMotorbike](Source/Delivery/Vehicle/DeliveryMotorbike.h) —— **运动学街机式载具，不是 Chaos Vehicle**。
- 为什么不用 Chaos：美术资产是一整套静态网格 + 一个坐姿骑手，没有轮子骨骼、没有物理资产，Chaos 要的东西一样都没有，硬上等于要先回 Blender 重新绑定；而且关卡里的交通车（`BP_car_base`）本来就是运动学沿样条走的，玩家车用同一套假设，不会出现"玩家车被物理弹飞、AI 车纹丝不动"这种两套世界观打架。
- 每帧自己算速度/转向。水平位移带 sweep 挡墙（撞上掉速不弹开）；竖直方向打射线贴地，**竖直这一步不能用 sweep**——sweep 会被地面挡住，和"我要贴到地面上"互相打架。转向量乘 `Speed/TurnSpeedReference`，所以停着打把不会原地转圈。
- **爬坡越障**（`Motorbike|爬坡越障`）。原来"一点爬坡能力都没有"是三个独立原因叠在一起，缺一条都修不好：
  1. **台阶被 sweep 当成墙**。水平推进是 `AddActorWorldOffset(..., bSweep=true)`，马路牙子和墙一样只会让它掉速。补了和角色移动同款的三步跨越 `TryStepUp()`：**抬 `MaxStepHeight`（45）→ 走完这一帧剩下的位移 → 往下探回地面**，任何一步撞住就整体撤回当撞墙。三个撤回条件都有来由：头顶撞到（桥洞）、抬起来还是几乎走不动（真墙）、落点比 `MaxClimbAngle` 还陡（墙根斜角，不撤的话车会顺着墙往上蹭）。
  2. **贴地高度上坡时一直欠着一截**。`FInterpTo(Location.Z, DesiredZ, GroundSnapSpeed)` 是指数收敛，坡越陡车越快欠得越多，而**下一帧的水平 sweep 是在那个偏低的位置做的**，于是直接撞在坡面上掉速——自己把自己绊住了。加了爬升率下限 `MaxClimbRate`（600 cm/s），上坡时保证至少这个速度往上追。
  3. **反应太晚**。贴地射线只查脚下，等车头撞上坡面才开始抬。加了 `GroundLookAhead`（90cm）的前瞻射线，**只取更高的那个**——前方更高就提前爬，前方更低（下坡/悬崖）不提前掉，否则车在坡顶就开始往下钻。前瞻距离跟车速走，停着时为 0，不然停在坡底会莫名浮起来。
  - `GroundStickTolerance`（80）替掉了原来写死的 60，且实际取值是 `max(它, MaxStepHeight + 10)`：**这个阈值必须大于 `MaxStepHeight`**，否则刚跨上一级台阶就被判成"飞出去了"，车会在台阶上反复弹。
  - `MaxGroundAlignAngle` 跟着 `MaxClimbAngle` 一起提到 40（原 35）：能爬上去的坡，车身姿态就该跟着贴上去。
  - `MaxStepHeight` 别调太大——它同时是"能凭空抬多高"的上限，过大时车会爬上本该挡住它的矮墙。
- **爬坡越障**（`Motorbike|爬坡越障`）。原来"一点爬坡能力都没有"是三个独立原因叠在一起，缺一条都修不好：
  1. **台阶被 sweep 当成墙**。水平推进带 sweep，马路牙子和墙一样只会让它掉速。补了和角色移动同款的三步跨越 `TryStepUp()`：**抬 `MaxStepHeight`（45）→ 走完这一帧剩下的位移 → 往下探回地面**，任何一步撞住就整体撤回当撞墙。三个撤回条件都有来由：头顶撞到（桥洞）、抬起来还是几乎走不动（真墙）、落点比 `MaxClimbAngle` 还陡（墙根斜角，不撤的话车会顺着墙往上蹭）。
  2. **贴地高度上坡时一直欠着一截**。`FInterpTo(Location.Z, DesiredZ, GroundSnapSpeed)` 是指数收敛，坡越陡车越快欠得越多，而**下一帧的水平 sweep 是在那个偏低的位置做的**，于是直接撞在坡面上掉速——自己把自己绊住了。加了爬升率下限 `MaxClimbRate`（600 cm/s）。
  3. **反应太晚**。贴地射线只查脚下，等车头撞上坡面才开始抬。加了 `GroundLookAhead`（90cm）的前瞻射线，**只取更高的那个**——前方更高就提前爬，前方更低（下坡/悬崖）不提前掉，否则车在坡顶就开始往下钻。前瞻距离跟车速走，停着时为 0，不然停在坡底会莫名浮起来。
  - `GroundStickTolerance`（80）替掉了原来写死的 60，且实际取 `max(它, MaxStepHeight + 10)`：**这个阈值必须大于 `MaxStepHeight`**，否则刚跨上一级台阶就被判成"飞出去了"，车会在台阶上反复弹。
  - `MaxGroundAlignAngle` 跟着 `MaxClimbAngle` 一起提到 40（原 35）：能爬上去的坡，车身姿态就该跟着贴上去。
  - `MaxStepHeight` 别调太大——它同时是"能凭空抬多高"的上限，过大时车会爬上本该挡住它的矮墙。
- **美术资产分两层挂：`MeshRoot`（只做侧倾 Roll）→ `MeshAlign`（车头朝向 + 居中偏移）→ 车体/骑手。** 不能合成一层：`FRotator` 的施加顺序是 Roll→Pitch→Yaw，侧倾和朝向修正写在同一个组件上时，Roll 会绕"修正之前"的局部 X 轴转，而那根轴在修正 90 度之后是车的横向——本该压弯，实际变成点头。UE 里正 Roll 是往左倒，所以右转取负值。
- **骑车时镜头写死在车尾后方**（`bUsePawnControlRotation=false` + 只继承 Yaw），鼠标不参与。之前用"控制旋转 + 延时回正"，上车瞬间镜头还停在人物原来的朝向上、车头却朝别处，玩家按 W 看到车"横着走"，方向感整个是错的。载具阶段"W 永远是往屏幕里开"比自由视角重要。控制旋转仍然每帧同步成车头朝向——镜头自己不用它，但下车后角色的弹簧臂要用，同步着视角才连续。
- 上车顺序：先 `GrabComponent->ForceRelease()`（手里还抓着东西的话，约束会把货物/别的玩家一路拖在车上）→ `StopRagdoll()`（关刚体模拟并把网格挂回胶囊，不停的话被挂到车上的身体会一路抽搐）→ 隐藏 + 关碰撞 + 挂到车上 → `Controller->Possess(bike)`。下车反过来，先摆好位置再 `StartRagdoll()`（它内部带 `PlaceOnGround`，顺序反了人会掉在原地）。
- **车把与骑手独立求解**：SteerPivot 前叉、BarPivot 车把继续沿用原转角；RiderPivot 不再整人旋转。双手目标挂 BarPivot，双脚目标挂 MeshAlign，由骑手动画代理解四肢 IK。转向角仍不乘速度系数，停车打把也有反馈。
- **被交通车撞**：`UDeliveryTrafficCarComponent` 在原有的逐帧扫掠撞击之外**单开一次 `ECC_Pawn` 查询**（摩托车碰撞盒是 Pawn 配置，而撞人查的是布娃娃刚体 `ECC_PhysicsBody`；并进同一次查询会把角色胶囊也扫进来、扰动那条已经调通的撞人逻辑），命中后调 `ADeliveryMotorbike::NotifyTrafficImpact()`。
  **共用同一张冷却表 `LastImpactTimeByActor`（0.75 秒）**——一次碰撞会连着好几帧重叠，不设冷却的话"撞两下才下车"会在同一次碰撞里就被扣完。
  每撞一下：按来车方向给击退速度、按左右分量给撞歪的角速度和车身倾斜（`KnockTilt` 直接叠在转弯侧倾上，**不再插值**，插了会把撞击那一下的尖峰抹平）、镜头两轴不同频率抖动。四个量统一按 `KnockDecay` 指数衰减。
  撞满 `ImpactsToDismount`（默认 2）次 → `ExitVehicle()`（和按 F 同一条路）→ 再 `KnockDownDriver()`：走既有伤害 GE 把血清零（回血 GE 可能抵消，所以跟着补一次 `SetNumericAttributeBase`，和撞人那边一样），ASC 自己会切 Stunned/Limp，复用现成的倒地表现，不另写一套。**顺序不能反**：`ExitVehicle` 内部的 `StartRagdoll` 会重建刚体，先加的冲量会被冲掉。
  被掀下车时那记抛射的力度**按来车速度缩放**（`KnockDownSpeedReference` 1200 cm/s 到满、`KnockDownLaunchMinScale` 0.35 保底）：交通车的 `MaxSpeed` 本来就是 600~1500 随机的，一律同一个飞法就看不出"被快车撞"和"被慢车蹭"的区别；保底不能给 0，否则慢车蹭一下人是原地瘫软，看着像自己躺下的。
- **晕倒的人不能按 F 上车。** 判据是 `ADeliveryCharacter::IsIncapacitated()`——读**复制过来的 `State.Stunned` Tag**，不是 ASC 上那个 `bStunned`（它只在服务器维护，客户端恒 false，本机提示照样会弹），血量 ≤0 兜一刀防 Tag 还没同步。两处都要判：探测组件里清掉 `Focused`（同时挡住提示浮窗和按键，`DoInteract`/`DoPickup` 拿的都是它，不用每个调用点各写一遍），`TryEnter` 里再判一次（按键走 Server RPC，客户端状态不可信）。
- **"按 F 下车"只在上车后显示 `ExitPromptDuration`（2 秒）。** 它吊在车顶上方、一直挂着挡视野，而这条信息玩家看一次就记住了；"按 F 驾驶"那条本来就只在 `InteractRadius` 内出现（`FindBest` → `CanInteract` 逐个比距离），不需要额外做什么。
- **骑车时骑手的材质换成驾驶员自己那套**（`bUseDriverMaterials`，默认开）：骑手那 9 个槽共用一个 `tripo_mat_c9b1ab96`，**里面一张贴图都没有**，进游戏是一块灰白。
  **映射按槽位名一一对应。** 玩家网格 `Content/Characters/A/renwu` 和车模型自带的骑手**是同一个基础角色**（都是 Tripo 出的），槽位名完全一致：`tripo_mat_c9b1ab96` / `材质` / `材质_001`…`材质_006` / `Eyes_Black`；区别只在玩家那边给这 9 个槽配了 `character_hat`（绿帽）/`cloth`（黄衣）/`pants`（浅蓝裤）/`shoe1`/`shoe2`（深蓝鞋）/`socks`/`skin`/`eyes`/`prime`。同一个基础角色也意味着 UV 是同一套，贴过去就是玩家本人的样子。`RiderMaterialOverrides` 按下标手填可覆盖；没人骑时 `EmptyOverrideMaterials()` 还原。
  **踩过的坑**：第一版写成"名字里带 eye 的配眼睛，其余所有槽都用第一个非眼睛材质"，结果整个人被涂成衣服那一种黄色。起因是先用 `grep`/ASCII 扫 `.uasset` 判断槽位数，**`材质_00x` 是以 UTF-16 存在名字表里的，ASCII 扫描一条都看不见**，于是误判骑手只有 2 个槽。查 `.uasset` 里的中文名要按 UTF-16 扫，别用 `strings`。
- **骑车时的镜头有两套，局内按 `P` 随时切**（`CameraToggleKey`，默认 `EKeys::P`；`bFreeLookCamera` 是初始模式，默认开 = 自由视角）。两套设置都完整写在 `ApplyCameraMode()` 里，没有哪一套是被删掉的。
  - 切视角走 `BindKey` **直接绑键**，不新建 InputAction + 在 IMC_Default 上映射：那个资产从来没被提交过，每次 git 拉取都会把映射冲掉（F 键就这么没过两次）。物品栏 1~5 也是同样理由直接绑的。P 是扫 `IMC_Default` 的**名字表**确认空闲的（里面只有 A/D/E/F/J/S/SpaceBar/W，J 是电话）——注意只能按 FName 表扫，按 ASCII 正则乱扫单字母会把一堆无关字符串认成按键。
  - 切换那一帧要接控制旋转，否则视角会跳：切到固定视角调 `SyncControlRotation()`，切到自由视角保留当前朝向当起点。
  - **自由视角**（开）：弹簧臂 `bUsePawnControlRotation=true`，`bInheritPitch/Yaw` 都必须设成 true——`USpringArmComponent::GetTargetRotation()` 会拿组件的相对角度把没继承的那一轴顶掉，只开一个就只剩一个轴能转。鼠标/右摇杆复用角色身上同两个 IA（`IA_Look` / `IA_MouseLook`），俯仰上下限走 `PlayerCameraManager` 的 `ViewPitchMin/Max`。**Tick 里必须停掉 `SyncControlRotation()`**：自由视角下控制旋转就是镜头，每帧同步成车头朝向等于把鼠标抹掉。
  - **固定车尾视角**（关）：`bUsePawnControlRotation=false`、只继承 Yaw、俯仰吃 `CameraPitch` 的相对角度，鼠标完全不参与，"W 永远是往屏幕里开"。
  - 代价写在这里免得以后重新纠结：自由视角下 **W 按的是车头方向、不是屏幕里的方向**，镜头转到侧面时按 W 车还是往自己车头那边开。载具游戏普遍如此，但和固定视角手感确实不同，这就是留开关的原因。
  - **被撞抖镜头在自由视角下只剩 Roll 一轴**：Pitch/Yaw 归控制旋转管，写进相对角度会被顶掉，硬抖就得每帧改控制旋转、和玩家鼠标打架。`bInheritRoll` 保持 false，弹簧臂那一轴取的正是相对 Roll，左右晃一样读得出"被撞了"。
- **骑手已改为 SkeletalMesh + ABP_MotorbikeRider**：原生父类 DeliveryRiderAnimInstance 用动画代理从坐姿参考骨架生成姿态，不使用旧 PoseableMesh 直接拧脖子的实现。详细流程、调参和验证见 [Document/MotorbikeRider.md](Document/MotorbikeRider.md)。
  迎风后仰：HeadWindSway 默认 18°乘 WindExaggeration（默认 1.7），用 .72 基准 + .28 波动得到约 13～31°的后仰回摆目标，避免顶住 35°限幅。直行无周期性左右摇头/扭腰；身体前后阵风系数 .3，转弯侧倾乘 1.35，侧向阵风系数 .65，头部增加滞后的转弯侧向回摆。侧摆由转向强度淡入，直行归零；头部在 IK 后叠加，手脚仍固定。停车回正，倒车不套用前进迎风后仰。测试检查后仰方向、前后变化、横向稳定、接触点和停车回正。
  可达性缩幅只查双腿；手臂改由腰部连续补偿后解 IK。把手臂放进全身缩幅会在边界突然清零整套身体动作，强化版测试曾产生 5.178 cm 单帧跳变，修正后为 0.761 cm。停车应比较带握把补偿的中立姿势，下车才是裸参考姿势。
  最新加强档覆盖上述幅度：前后阵风 .55、转弯侧倾 1.6、侧向阵风 .95、头部侧摆 .6，头部后仰目标约 14～42°/限幅45°。按用户要求，允许手部5cm间隙以释放上身摆动，HandSlack弹簧平滑随风压淡入/停车归零，脚仍严格贴合。测试通过：匀速角度变化13.39°、最大手部间隙4.901cm、最大单帧头部位移1.345cm；角度连续性和脚部贴合断言也通过。
  资产迁移只运行 Content/Python/setup_motorbike_rider.py，保留材质和模型，不改地图及输入。不要为了迁移组件重跑整套地图放置脚本。
  车辆读取速度、加速度、转向、颠簸和撞击，服务器复制表现输入；本机预测、远端弹簧平滑。动画每帧从参考坐姿重新计算胸腰、头部，再补腰部可达性和四肢 IK，不累积旋转、不拉长手臂。旧 RiderSteerRatio/RiderNeckSteerRatio/RiderNeckBone 仅保留序列化兼容，不再控制动作。
- 骑手网格平时隐藏，有人骑才显示——模型自带骑手，不藏起来路边空车上永远坐着个人。
  - 手脚贴合由接触目标和 IK 保证，不再靠车把转角与整人转角的差值近似。骨架/目标改变后须重新运行 Delivery.Vehicle.Rider 测试。
- **轮子自转默认关着**（`bSpinWheels=false`）。整套逻辑都留着，勾上就生效，不用重编也不用重跑脚本。
- **轮子按车速自转**（`WheelPartIndices` / `WheelCenters` / `WheelRadius`，都由脚本按几何写，别手填）。每个轮子再多挂一层自己的轮轴 `WheelPivots[k]`，**前轮那一层挂在 `SteerPivot` 下面**：先跟着龙头转、再绕轮心自转，两件事互不干扰；挂在 SteerPivot 下时轮心要减掉 `SteerPivotLocation`，因为父级原点已经是转向轴。
  - 认轮子的判据两条缺一不可：**正圆**（长/高比 > 0.9）且**窄**（宽 < 直径 × 0.6）。实测两个轮子都是 61.3×61.3、宽 27.7、圆度 1.00；第三名 `车.007` 圆度 0.96 但宽 63.3 比直径还大，靠"窄"这条挡掉，只用圆度会误抓。
  - 转速 = 线速度 / 半径（纯滚动，接地点速度为 0），只写 Roll（绕 MeshAlign 局部 X = 轮轴）。**方向要取负**：局部 +Y 是车头，UE 里正 Roll 把 +Z 转向 -Y，也就是轮顶往后 = 倒着滚。符号做成了 `WheelSpinSign`，推反了改成 +1 即可，不用重编。
  - **远端客户端上 `CurrentSpeed` 恒为 0**（那边不跑 `UpdateSpeed`），轮子会僵住，所以非权威非本地时改用"实际位移在车头方向上的分量 / dt"反推车速。
- 车体是 9 个 `UStaticMeshComponent` **固定槽位**（`MaxBodyParts = 12`，构造函数里建好），按 `BodyMeshes` 数组填充。没用运行时 `NewObject` 动态建组件，避免构造脚本反复重建组件/丢实例覆盖。

### 召唤载具（按 R）
[DeliveryVehicleSummonComponent](Source/Delivery/Vehicle/DeliveryVehicleSummonComponent.h) —— 挂在玩家 Pawn 上，按 R 把最近一辆没人骑的摩托车挪到身前，冷却 10 秒。
- **单独一个组件**而不是塞进 `ADeliveryCharacter`：它需要**按固定频率 Tick**来刷左下角提示，而角色的 Tick 是 `bStartWithTickEnabled=false`、只在被车撞的镜头拉远期间才临时打开的，借它推 HUD 会把那套按需开关搅乱。
- 冷却是**服务器权威**的：`ReadyServerTime` 服务器写、`COND_OwnerOnly` 只复制给拥有者，客户端只拿来显示，不做预测（召唤本来就要等一个来回）。**探不到地面的召唤不计冷却**——玩家什么都没得到，不该被罚等 10 秒。
- 落位交给 `ADeliveryMotorbike::SummonTo()`，车自己往下打地面射线，并**清空 `CurrentSpeed` / `VerticalVelocity` / 油门转向 / 被撞状态**：残余车速会让车刚出现就从脚边滑走，残余被撞状态会让它一边抖一边歪着出现。车上有人时拒绝，不能把别人正骑的车抽走。
- `PlaceDistance` 默认 230cm，要大于车长（256cm）的一半再留余量，否则车会压在玩家身上把布娃娃顶翻。

### 屏幕左下角常驻提示
`UDeliveryPromptSubsystem::PushCornerHint()` 和世界浮窗 `PushPrompt()` 是**两个独立槽位**，各有各的时间戳，同一个 SConstraintCanvas 上两个锚点。分开是必须的：合在一起的话走到车边时"按 R 召唤"会被"按 F 驾驶"顶掉。同样是"每帧推一次、停推 0.25 秒自动消失"的约定，所以不需要成对写 Show/Hide。
- 步行时由 `UDeliveryVehicleSummonComponent` 推召唤提示（冷却中压暗成灰色 + 读秒）；骑行时由 `ADeliveryMotorbike::PushCameraHint()` 推"按 P 切换视角（当前：自由/固定）"。
- **两者天然不会打架**：上车后控制器去 Possess 摩托车，被丢在车上的那具身体 `IsLocallyControlled()` 变成 false，召唤组件自动停推。
- 提示里的键名一律从**实际绑定的 FKey** 取 `GetDisplayName()`，改了 `SummonVehicleKey` / `CameraToggleKey` 提示会跟着变，不会说一套做一套。
- 图里一辆摩托车都没有时不推召唤提示——告诉玩家一个按了什么都不会发生的键没有意义。

### 摩托车资产接入（`Content/Python/setup_motorbike.py`）
菜单 **Delivery → Setup Motorbike**，或控制台 `py setup_motorbike.py`。四步幂等、可单独重跑：
**摩托车现在是自配置的，`setup_motorbike.py` 从"每次改完必须跑"降级成可选。** 这是被"又要重跑一次脚本"逼出来的结论，别再往回走：
- **凡是脚本算得出来的几何，C++ 自己也能算。** `AutoConfigureFromMeshes()` 在 `ApplyBodyMeshes()` 开头跑，用和脚本**一模一样**的判据（轮子=正圆且窄、车把=X 最宽、前轮前叉=中心落在前 1/4、转向轴=车把与最前部件的水平中点）从 `UStaticMesh::GetBoundingBox()` 现场量。顶点本来就在 FBX 场景绝对坐标里（导入时 `transform_vertex_to_absolute=True`），包围盒即整车里的位置，不用再拼相对变换。
- **只填空的**：已经有值的一律不碰，脚本量过的和在 BP 上手调过的都算数。
- **组件上的资产引用要有软引用兜底。** `RiderMesh` 的网格引用存在 BP CDO 里，**组件类型一变就会丢**（`USkeletalMeshComponent` → `UPoseableMeshComponent` 那次就丢了，表现是"车上突然没人"），只能靠重跑脚本修。现在 `RiderMeshAsset`（`TSoftObjectPtr`）在组件为空时自动加载。和 `InteractAction` 用软引用是同一个理由：构造函数只在模块加载时跑一次，软引用才能在运行时解析。
- **不要改成"编辑器启动时自动跑脚本"**：`place_in_level()` 会挪动/重放关卡里的车，每次开编辑器都重来一遍；而且打包后根本没有 Python，自配置在打包版里一样有效，自动跑脚本没有。
- 脚本保留的价值：导入 FBX、建 BP 和 IA、摆进关卡、以及在报告里把量出来的数字打给人看。**改 C++ 默认值（手感、镜头、爬坡参数等）从来都不需要跑它。**

导入 FBX → 建 `IA_Interact` 并在 IMC_Default 上映射 F → 建 `/Game/Vehicle/Motorbike/BP_Motorbike` → 在当前关卡出生点前方放一辆。报告写到 `Saved/setup_motorbike.txt`。

这份 `摩托车.fbx`（工程根目录）离线解析出来的三个坑，改导入流程前先读：
- **坐姿写在骨骼的当前变换里，不在网格顶点里。** 绑定姿势（Cluster 的 `TransformLink`）是站姿——膝盖在髋正下方；骨骼节点的当前变换才是坐姿——大腿前伸下压 131°、小腿回折 55°、两手落在把手宽度上。所以骑手只能按**骨骼网格**导入；按静态网格导入只有原始顶点 = 站姿，会得到一个站在车里的人。
- **而且骨骼网格导入必须开 `use_t0_as_ref_pose`**，这是坐姿能不能进来的开关。名字像是给有动画的文件用的，实际含义见 `FbxSkeletalMeshImport.cpp:1291`：**关着时参考骨架取自 BindPose（站姿），开着才用 `GetNodeGlobalTransform(Link, 0)`（节点当前变换 = 坐姿）**。没有动画不影响，t0 取的是节点变换、不需要 AnimStack。第一版按字面意思关掉了，导进来的骑手是站着的。
- **车体那 9 个网格没有蒙皮、挂在场景根节点下**（不在骨架层级里），骨骼网格导入器会直接跳过。所以一份 FBX 必须导两次：骨骼网格拿骑手，静态网格拿车体。静态那次用 `combine_meshes=False`（不然会和站姿骑手焊成一块、永远分不开）+ `transform_vertex_to_absolute=True`（顶点留在场景绝对坐标里，于是 9 个组件都摆在相对变换零点就能原样拼回整车，不用手工还原每个部件的相对位置）。脚本里有检测：部件原点全挤在 10cm 以内就说明这个选项没生效，会在报告里点名并给手动兜底步骤。
- **UE 5.8 默认用 Interchange 接管 FBX 导入，那条路完全不读 `FbxImportUI`。** `AssetTools::ImportAssetTasks` 里 `bUseInterchangeFramework = IsInterchangeImportEnabled() && (SpecifiedFactory == nullptr)`，所以导入任务要显式 `task.factory = unreal.FbxFactory()` 才走老的 FBX 导入器。
- **但指定 factory 只对"全新导入"管用。** 实测：同一个任务，首次导入走老路（日志 `LogFbx: Bones digested - 37`），而目标资产已存在时用 `replace_existing=True` 重导，**会被路由到 Interchange**（日志 `LogInterchangeEngine`），`FbxImportUI` 的选项整份失效，还会报「使用所选的管线选项，提供的源数据中没有要导入的内容」却照样写出一个资产。所以改骨骼导入选项要**先删掉旧资产再干净导入**（`reimport_rider()` 就是这么做的：摘掉蓝图引用 → 删 SK/Skeleton → 重导 → 挂回去）。
- **别假设走了哪条路，查一下。** `asset.get_editor_property("asset_import_data")` 的类型就是答案：`FbxSkeletalMeshImportData` = 老路（选项有效），`InterchangeAssetImportData` = Interchange（选项被忽略）。这一条让"选项读回 True、结果却全错"从一个查半天的谜变成一行日志——脚本现在每次重导都打印它。

**第一次跑完踩出来的三条（都已修）：**
- **骑手的静态副本要靠"排除"而不是"删除"。** `EditorAssetLibrary.delete_asset` 对刚导入、还没存盘的资产会**静默失败**（返回 False 不抛异常），于是 12 个部件全进了 `BodyMeshes`，车上永远站着一个 T-pose 的人（`RiderMesh` 藏得再好也没用，站着的那个是静态网格）。现在在 `_collect_body_meshes()` 里过滤：名字命中 `character_body/eye_*`，或者**整份材质都是骑手材质**（`tripo_mat_*`、`Eyes_Black`；车体用的是 `材质*`）。两道判据都会把每个部件的名字+材质打进报告，万一还有漏网的一眼就能认出来。
- **导入产物必须显式存盘。** 第一版只存了蓝图，静态网格和骨骼网格全是内存里的脏包，用户一关编辑器就没了（盘上只剩 `BP_Motorbike.uasset`，引用全断）。现在跑完 `save_directory(DEST, only_if_is_dirty=False, recursive=True)`。
- **车头朝向：模型的车头朝 +Y，Pawn 朝 +X 开，差 90 度。** 从第一次的报告数字反推出了轴映射：`UE_X = FBX_X`、`UE_Y = FBX_Z`、`UE_Z = FBX_Y`（用 166×256×242 逐项对上 FBX 实测尺寸；256 = 车长 134.7×1.9 落在 UE 的 Y 上）。车头是 +FBX_Z 那一侧——依据是骑手的手（Z=34.1）在髋（Z=19.5）前面。所以 `MeshAlign` 要转 **−90°**，脚本按"长边在不在 X 上"自动判定，`MESH_YAW_OVERRIDE` 可强制。注意组件变换是"先转再平移"，**居中偏移也要跟着转过去再取反**，否则转完整车就偏出去了。

其他两条：
- `IMPORT_SCALE = 1.9` —— FBX 里骑手站立高度 100 个单位，游戏角色胶囊 96 半高（约 192cm）。车体和骑手必须用同一个值，否则比例会错。车体在 FBX 单位下是 135×88×72，乘 1.9 约 256×168×136 cm。
- 脚本会验证坐姿真的导进来了，判据是**大腿偏离竖直的夹角**（坐姿约 49°、站姿约 4°），临时 spawn 一个 `SkeletalMeshActor` 读参考姿势下的骨骼位置，读完就删。
  **别用包围盒高度判**——实测坐姿 182cm、站姿 190cm，只差 8cm，拍不出阈值，第一版就是这么一直误报"站姿"的（骨骼网格的包围盒还带动画余量）。骨头指向哪和缩放无关，才是能用的判据。
- 重跑 `place_in_level()` 会**沿用上一辆车的位置朝向**，摆好之后再调参数不用重新找地方摆；关卡里没有 `PlayerStart` 时（`testfortraffic` 就没有）落点取编辑器视口镜头前方，而不是世界原点。
- `IA_Interact` 是**复制 `IA_Jump`** 建出来的——`InputAction` 没有暴露给 Python 的工厂，`create_asset` 那条路不通。C++ 侧 `InteractAction` 用 `TSoftObjectPtr` 晚绑而不是 `ConstructorHelpers`：构造函数只在模块加载时跑一次，脚本这次会话里新建的资产永远解析不到，晚绑才能当场生效。

## 开发环境与编译

- **引擎：UE 5.8。** 编译走 `Build.bat`，目标是 **`DeliveryEditor`**（不是 `Delivery`——只编后者的话
  新的 C++ 组件不会在编辑器里注册，`DeliveryTrafficCarComponent` 就因为这个"在编辑器里找不到"过一次）：

  ```
  "<引擎目录>/Engine/Build/BatchFiles/Build.bat" DeliveryEditor Win64 Development -Project="<工程目录>/Delivery.uproject" -WaitMutex
  ```

  引擎目录**每台机器不一样**，Epic 默认装在 `C:\Program Files\Epic Games\UE_5.8`。

- **编辑器开着就编不了**：会直接报 `Unable to build while Live Coding is active`。
  可靠做法是**先关编辑器再编**；在编辑器里按 Ctrl+Alt+F11 走 Live Coding 也行，但它对
  新增 UPROPERTY / 改类布局这类改动不可靠，结构一变就该用完整编译。
  等人关编辑器时别写 `sleep` 轮询，起个后台任务等 `UnrealEditor` 进程退出再编。

- **什么改动需要做什么**：

  | 改了什么 | 要做什么 |
  |---|---|
  | C++ 里的默认值（手感、镜头、参数） | 只要重编；BP 没覆盖过的默认值重开就生效 |
  | 新增/删除 UPROPERTY、改组件类型 | 重编；**改组件类型会丢 BP CDO 上那个组件的实例覆盖**（见摩托车一节的软引用兜底） |
  | 只在 BP 上调参数 | 什么都不用做 |
  | 导入新资产、建新 BP/IA、往关卡里摆东西 | 跑对应的编辑器 Python 脚本 |

- **编辑器 Python** 从编辑器控制台跑 `py 脚本名.py`，常用的也挂在菜单 **Delivery** 下。
  Python **只在编辑器里有**，打包版里没有——所以运行时要用的东西不能依赖脚本，
  得让 C++ 自己算（摩托车的 `AutoConfigureFromMeshes()` 就是这个原因）。

- **`Content/Input/IMC_Default.uasset` 至今没有被提交过。** 它里面有 F 键到 `IA_Interact` 的映射，
  每次 `git pull` 都会被冲掉，症状极具迷惑性：浮窗照常显示、按 F 毫无反应（浮窗不依赖按键绑定）。
  新加的按键因此一律用 `BindKey` 直接绑（物品栏 1~5、切视角 P、召唤 R），不走 IMC。

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
├── Interaction/                     走近按 F：可交互组件、探测组件、Slate 浮窗子系统（含左下角常驻提示）
├── Inventory/                       五格物品栏、手持道具
├── Ragdoll/                         主动布娃娃、布娃娃战斗组件
├── Traffic/                         交通车辆辅助组件（卡住重置循环、前车避让减速）
└── Vehicle/                         可骑载具（摩托车，运动学街机式）+ 按 R 召唤载具组件
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
