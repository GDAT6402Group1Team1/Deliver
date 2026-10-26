# CLAUDE.md

给 Claude Code 在本仓库工作时的上下文说明。

## 项目是什么

**Delivery**（欢乐向奇遇送快递游戏）。玩家扮演快递员，在风格各异的地区完成配送，
沿途可以对 NPC 和场景物件恶作剧。支持单人（升级推进）和多人（抢先送达抢单）。

- 引擎：Unreal Engine 5.8
- 详细的游戏简介、Git LFS 操作、克隆/同步/提交步骤见 [README.md](README.md)，不要在这里重复。

## 目前做了什么（按提交历史整理）

### 角色与移动：主动布娃娃（Active Ragdoll）
- 坡上松键的髋部切向站立目标保持在停步位置，不再每帧跟随下滑的身体重建；恢复移动、跳跃、攻击/受击、失衡或离支撑点超过 80 cm 时解除。只固定电机参考，不修改刚体变换；移动地面按命中点速度推进参考。
- 坡面防滑：落脚最后 15% 不再被跟随髋部的横向纠偏移动目标。已贴地且未启用位置驱动的支撑脚，在 5° 至翻滚阈值之间施加有上限的切向摩擦辅助（`SlopeSupportTraction` / `SlopeSupportMaxAcceleration`），补偿顺坡重力并阻尼相对地面速度；不锁位置、不施加法线吸附，摆动/释放恢复/离地及明显失衡时不介入。动态地面按表面速度计算。
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

- 撞后缩身恢复：脊柱、肩根、腿和脚踝的父空间电机使用 `CreateReferencePoseControls` 从资产参考骨架计算固定相对旋转，不依赖可变动画/物理骨骼缓存；迈步仍由脚位置目标驱动。起身完成额外要求胸相对髋目标误差 <25°；清醒、非抓取/出拳/跳跃、在安全地面低速且直立点积 <0.35 持续 0.6 秒时重新进入原地恢复，不切物理、不瞬移、不改 HP。测试显式使用玩家覆盖的 `renwu_Physics1`，验证畸变缓存不改变目标；自动化不等于真实撞飞验收。
- 行走手感回退：在实际关卡中，提速到 `DesiredMoveSpeed=260`、`ControlledStrideLength=90`，以及后续全身速度伺服、脚落点限速与输入平滑，都未达到用户想要的手感，已回退到此前较慢的蓝图配置 150/33.6。普通行走不额外加强胸腰电机；地面点继续完整平滑。保留更早的撞击、起身与坡地修复。`Delivery.Ragdoll.LegIsolation` 的自动化检查不能代替真实地图体感；编译 C++ 前关闭编辑器，避免 UE 热重载重建 CDO 时崩溃。

- 急转身与卡腿（2026-10-15）：`GetSlopeRight`/横向纠偏改用 live 髋刚体相对参考姿势的真实朝向，不能把 `CurrentFacingYaw`（电机目标）当作身体已转到的方向；摆动中的防交叉也使用当前真实侧轴。迈步到期且距离目标仍 >18 cm 时取消两脚位置驱动，保持 0.45 秒释放期并暂停移动目标，避免同一帧立即重新选中失败脚、让超时释放形同虚设。`LegRecoveryDiag` 记录真实腿部目标误差、脚电机/距离/步伐计时。`Delivery.Ragdoll.LegIsolation` 用生产蓝图在独立平地世界逐帧对比正常、关闭脚拉力、关闭内部碰撞（后两种仅测试），并断言超时不会同帧重启、物理确实移动；仍需真实地图复测。

### 互殴系统（近战战斗）
- 出拳接走路：起步前倾计时跟踪原始移动意图，不能使用被脚步超时/恢复临时归零的 `EffectiveWish`，否则持续按键也会重复前倾。胸腰支撑覆盖 `HasPendingPunchRecovery()` 收拳阶段，不在攻击窗口结束时提前放松。`Delivery.Ragdoll.LegIsolation` 包含不出拳、左拳、右拳后立即前走对照及持续按键跨脚步释放的回归断言；平地自动测试不代替实际关卡复现。
- 撞飞后髋部质量异常修正：`DeliveryRagdollMass.cpp` 在 ConfigurePhysics 创建真实刚体后检查髋部与直接连接躯干/大腿的质量比。仅当相邻最大质量 > 髋部 20 倍时，把髋部设为该最大质量的一半，并将各轴惯量至少补到相邻最小主惯量的 1/4；解决 renwu 髋部 0.0115 kg 连接 24–32 kg 刚体的病态配置。只改 live BodyInstance，不扩大碰撞、不保存原资产、不提高电机/伤害。每次创建物理状态后需要重新应用；测试覆盖两次重建、幂等性及正常腿部不变。此修正仍需真实撞击复测，不把质量断言当作完整动力学验收。
- 撞飞恢复诊断：非 Shipping 的 `CrashDiagJoint` 记录全部 live 关节（包括胸腰、髋腿、膝踝）的参考框架间距/角差、实际角度/限位/质量；`CrashDiagDrive` 记录全部电机启用状态、力度倍率及目标偏移。晕倒期间也采样，便于定位起身后半身扭曲；frameAngleDeg 是关节参考框架角差，不等于动画驱动误差，不能直接作为统一恢复阈值。
- 出拳胸腰摆动：保留左右拳的转体，蓄力沿拳路反方向后摆默认 2°、释放朝拳路前摆默认 7°，通过原拧身速度平滑过渡，收拳回零。仅叠加胸腰目标，不改变骨盆/脚支撑、手臂旋转算法、冲量或伤害；组件 `Ragdoll|出拳` 的 `PunchTorsoWindupLeanAngle/PunchTorsoFollowLeanAngle` 可调。
- 运行时物理资产补全修改约束后必须同步 `UPhysicsConstraintTemplate::SetDefaultProfile`（WITH_EDITOR）：PIE 创建/重建关节时会在复制 DefaultInstance 后再次应用 DefaultProfile；只改 DefaultInstance 会让肩/肘限位回到 45°，新增右腕还会回到 Free 轴。同步仅修改运行时副本，原资产不保存。回归测试在实际 renwu 网格上重建两次物理状态，直接检查 live constraint 的肩/肘限位、右腕角限位/平移锁定/相邻碰撞禁用，不能只检查模板。
- 非 Shipping 的拳击诊断自动写入 `Saved/Logs/Delivery.log`：`PunchDiagEvent` 标记开始/释放/结束/取消，`PunchDiag*` 与 `UpperBodyTrace` 记录本机及服务器的状态、实际/目标旋转、目标角速度、质量、关节角度与限位。出拳/收拳时每 0.2 秒采样，待机每 0.5 秒采样，正常姿势也保留以便对照卡住前后；仅观察，不改物理参数。
- 收拳旋转连续性：实际主角 renwu 的旧参考姿态最短弧解算在收拳时出现单帧约 150°–163° 目标翻转。收拳从释放目标旋转平滑回到 A 姿势，不再混合拳路和肘部 Pole 重新求解；每只手保留自己的拳路方向，收拳目标未归零时拒绝下一拳。禁止在蓄力/出拳期间独立积分上臂和前臂的旋转（parallel transport）：它们会积累不同轴向扭转，曾造成实机剧烈乱动回归，现恢复参考姿态锚定的解算。测试 `Delivery.Boxing.PlayerRecoveryTargets` 覆盖实际主角左右手及 ±50° 瞄准方向、出拳旋转不依赖历史、收拳目标连续性；这不等价于场景中的完整物理验收。
- **持斧下劈动画**：`UseFromInventory` 按物品的 `bUsesAxeSwing` 显式开关接左键（手柄沿用左攻击键），正式 `/Game/Blueprint/Item/BP_Axe` 与测试斧头都已启用，普通物品仍调用原来的蓝图 Use，由布娃娃组件的 Server RPC 核验当前手持斧头、可控状态和冷却，再多播服务器起始时间。源动画 `/Game/Characters/Animations/Axe/A_AxeDownward` 长 2.533 秒，默认播放速率 1.8（实际约 1.407 秒）；`DeliveryAnimationArmPose` 按源骨架参考姿势建立前/右/上坐标，只将双臂方向重映射进现有 `BoxingPose` 电机，不复制源手臂长度、不覆盖腿部步态、不应用 Root Motion、不关闭物理。`Ragdoll|Axe` 可调动画、播放速率和 0.12/0.2 秒（动画时间）的淡入淡出。切换物品、抓取、晕倒或停止布娃娃会中止动作；斧头挥动时按实际前臂在前后平面的角度摆动，仍过滤轴向滚转。角色和普通道具仍没有新增斧头伤害或耐久消耗。导入脚本 `Content/Python/import_axe_swing.py` 只创建独立源资产，不改主角骨架或地图。正式斧头配置脚本 `Content/Python/setup_axe_attack.py` 只设置动作开关，不重设父类、模型、材质或地图。回归测试 `Delivery.Axe.HeldItemFlow` 覆盖正式/测试斧头、普通道具拒绝及取消；`Delivery.Axe.AnimationTargets` 核对完整剪辑、朝向变换、两级电机目标和物理模拟保持开启。
- **第一单斧击破门**：`ADeliveryBreakableDoor` 使用 `/Game/model/door` 与 `/Game/crack/crackdoor`；服务器在持斧下劈的命中时刻做近距离扫掠，命中后复制破门状态，完整门取消碰撞并显示物理碎片。`Content/Python/place_first_task_door.py` 将门和正式 `BP_Axe` 放到 `TestForCharacter` 的老头家旁，按房屋实际底部校正高度；门自带双面黄色 `OLD MAN HOUSE / AXE DOOR` 标牌，关卡中搜索 `Task001_BreakableDoor` 可定位。`Delivery.Axe.HeldItemFlow` 覆盖持斧破门与丢斧后不能破门。此门目前独立于敲门/任务状态，第一单的完整门铃流程尚未接入。
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
**快递从取货点领，不再手摆在地上**（2026-10-20 改）。按住 E 和取货点交互，
服务器当场生成一件快递交给玩家：**步行拿在手上，骑车绑在车上**。

- [DeliveryPickupPointComponent](Source/Delivery/Task/DeliveryPickupPointComponent.h) ——
  挂在取货点上。**不填任务**，而是按地点 ID 反查：登记在册、状态是待取件、
  且 `PickupLocationId` 等于本点 ID 的那个任务。所以同一个取货点能在一局里
  先后发好几个任务的快递，和 `Tasks.csv` 里"每个任务各自写取货点"的数据形状对得上。
  `PackageClass` 留空时退回内置测试快递——摆了点却一件都领不到又没有报错，
  是最难查的那种空配置。
- [ADeliveryPickupPoint](Source/Delivery/Task/DeliveryPickupPoint.h) ——
  拖进关卡填个 `LocationId` 就能用的现成 Actor（继承 `ADeliveryLocationMarker`，
  连 Billboard 和"Python 加的组件存不住"那条都一起继承了）。取货点本来就是关卡里
  某个模型时不用摆它，直接在那个 Actor 上加
  `DeliveryLocationComponent` + `DeliveryInteractableComponent` + `DeliveryPickupPointComponent` 三件套。
- [ADeliveryDropOffPoint](Source/Delivery/Task/DeliveryDropOffPoint.h) ——
  成对的收件点，填 `LocationId` + `ExpectedTask` 就能交付。
  一个能用的收件点需要**三个**组件（地点 + 可交互 + 收件），手加三次还要把
  `InteractionKey` 调成 `PickupE`、`HoldDuration` 调成 0.5，漏任何一个的表现
  都是"走过去按 E 没反应"、且三种漏法长得一模一样，所以做成现成 Actor。
  `DeliveryRadius` 构造时就对齐 `InteractRadius`：两个数不一样会出现
  "提示出来了但按不动"（提示走前者、交付走后者），而玩家只看得到后半截。
  **和取货点刻意不对称**：收件点手填 `ExpectedTask`，取货点不填任务——
  取货点是"这个柜台现在该发哪一单"（状态），收件点是"这一户只收这一单"（身份）。
- 取件点和收件点的 `HighlightActors` 是旧版建筑透视高亮配置，当前任务追踪不再读取，
  无需在关卡实例里填写；字段保留是为了兼容已有场景。
- `ReceiverNpcId` 目前**没有任何代码在读**，只是存在资产里，摆点时不用管它。
- **这同时修掉了旧流程的死锁**：以前快递掉出世界 → `EndPlay` 上报丢件 → 任务退回待取件，
  但地上那件已经没了，这一局再也推不动。现在退回待取件之后回取货点再领一件就行。
- **"放哪"只有一处判断**：`ADeliveryCharacter::ReceiveTaskPackage()`。取货点发货、
  地上捡回、上车、下车四条路全走它。散在四处各判一遍的话迟早出现"车上和手上各有一件"，
  而交付时取到哪一件是不确定的。
- **车上的货**：`ADeliveryMotorbike::AttachCargo/ReleaseCargo`，焊在 `CargoPivot`
  （挂 `MeshAlign` 下面，所以继承车头朝向修正和压弯侧倾）。**用焊接不用物理约束**——
  车是运动学的，约束另一端相当于无穷重刚体，Chaos 会把误差全算到快递头上，
  表现是快递在车后面疯狂抽动（拖人那套踩过同一个坑）。`CargoOffset` **留零时由
  `AutoConfigureFromMeshes()` 自动量**（取车尾 35% 那批部件的顶面中点——整车包围盒的
  `Max.Z` 是车把高度，用它会把快递架到半空）。要手填的话注意"往后"是 **−Y 不是 −X**：
  MeshAlign 局部空间里 +Y 才是车头，填错的表现是快递挂在车侧面。
- **上下车自动换手**：`TryEnter` 把手上的快递搬到车上（**必须排在 attach 之后**，
  因为 `GetCurrentVehicle()` 是从附着父级推出来的，attach 之前这个人还"不在车上"），
  `ExitVehicle` 搬回手上（排在 `StartRagdoll` 之后，刚体没重建时算出来的手部位置是旧的）。
  交回去失败（背包满、人晕了）就留在车上——车还能用 R 召唤回来，掉出世界的快递要重跑一趟。
- **骑车时按 E**：摩托车绑 `IA_Pickup` 后**转发给驾驶员那具身体**
  （`Driver->DoPickup()` / `DoPickupRelease()`），不在车上另写一套。
  配套地，交互探测器的门槛从"本机操控"放宽成"本机操控**或者**我是本机操控的车的驾驶员"
  ——骑车时控制器 Possess 的是车，那具身体的 `IsLocallyControlled()` 是 false，
  只判那一句的话上车之后 E 整个失灵。`CanUsePickupTarget` 里的布娃娃状态门槛同理跳过
  （骑车时 `StopRagdoll` 过了，控制模式不是 `Active`）。非任务道具（斧头）骑车时仍不给捡：
  它要进背包，而背包表现挂在那具被隐藏的身体上，捡了也看不见。
- **交付判定必须走 `GetCarriedTaskPackage()`**，不能用 `Inventory->GetHeldItem()`：
  骑车时快递在车上、手上是空的，用背包那句会让"骑到收件点交不了货"。
  `GetCargo()` 走 `IsValid` 而不是裸判空——快递被别处销毁时指针会悬着，
  裸判空会让"已经没货了却说有货"，下一件永远领不到；`TryDeliver` 也会先从车上摘再销毁。
- **骑车时 `GetPlayerState()` 是空的**，任务/结算一律走
  `ADeliveryCharacter::GetTaskPlayerState()`。引擎在 `APawn::UnPossessed()` 里
  `SetPlayerState(nullptr)`（UE 5.8 `Pawn.cpp:719`），而骑车时控制器 Possess 的是车，
  所以那具身体的 PlayerState 就是空的。直接用它去交付的结果是
  `TryCompleteDelivery(Task, nullptr)`——任务完成了、钱包一分没进，**且没有任何报错**。
- **收件点的交付判距离和提示/按键必须取同一个点**：`UDeliveryTargetComponent::GetDeliveryPoint()`
  转调可交互组件的 `GetInteractionPoint()`（锚点 > 根碰撞表面 > Actor 原点），
  不自己再实现一遍。早先这里用的是 `GetOwner()->GetActorLocation()`，挂在整栋楼上时
  提示浮窗在门口出现、`CanInteract` 也过，但走到 `CanAcceptDelivery` 会按楼心算距离并失败
  ——玩家看到的是**"提示出来了、按住 E 了、什么都没发生"，日志里一句话都没有**。
  和 `DeliveryRadius`/`InteractRadius` 不一致是同一类错，只是走另一条代码路径。
- **交互半径只有一个填写位置**：取货点填 `UDeliveryPickupPointComponent::InteractRadius`、
  收件点填 `UDeliveryTargetComponent::InteractRadius`（现成 Actor 上转发到 Actor 详情）。
  组件每次 `BeginPlay` 都把它写进可交互组件，**自动建的和手加的都盖掉**，
  所以"调了不生效"不可能发生；收件点还会顺带把 `DeliveryRadius` 对齐过去。
  早先按"自动建的用 A、手加的用 B、现成 Actor 用 C"分流，结果同一个概念三个位置，
  问一次"怎么调半径"要分三种情况回答——**一个旋钮永远生效 > 三个旋钮各管一段**。
- 调试：`Delivery.Pickup.Dump`（列全部取货点 + 此刻该发哪个任务 + 发不出的原因）、
  `Delivery.Pickup.Take`（从最近的点领一件，跳距离不跳任务状态）、
  `Delivery.DropOff.Dump`（收件点收哪个任务、交互配得对不对、你和快递各自离测量点多远）、`Delivery.Carry.Dump`（快递到底在手上还是车上）。`Delivery.Task.Validate`
  也认取货点了：地点 ID 能解析只说明那儿有个标记，标记上没挂取货点组件玩家按不动 E。

快递 E 拾取与交付改为纯范围判定，不要求准星、朝向或视线；本机以 20Hz 优先选可用目标；同等可用性下准星附近目标优先，未瞄准任何目标时回退到最近目标（准星只排序，不作为交互门槛），长按期间目标仍有效则保持选中，不可取快递仅作为原因提示的后备。服务端复核范围、任务状态和长按时间。保留快递 0.5 秒长按及现有交付半径，F 交互和双键物理抓取不变。
完整的结构、接口清单、配置方式、调用链和待确认假设见 **[Document/TaskSystem.md](Document/TaskSystem.md)**，改动前先看这份。要点：

- 任务状态全局共享（多人下解锁/来电/计时/完成对所有人是同一份），挂在 GameState 上的 [DeliveryTaskManagerComponent](Source/Delivery/Task/DeliveryTaskManagerComponent.h)；"当前追踪哪个任务"是每玩家各自的，挂在 PlayerState 上的 [DeliveryTaskTrackerComponent](Source/Delivery/Task/DeliveryTaskTrackerComponent.h)。
- 计时在**接通任务解锁电话**时开始，用服务器时间戳而不是 Tick 累加；通话、前往取餐、掉落/换手/进后备箱/晕倒都碰不到计时。漏接后直接取餐会以取餐时刻兜底。
- **取件要求"按过接听"**，不是"电话响过"：`CanAcquireItem` 查 `bTimerStarted`（只由 `AnswerCurrentCall` 置上），`IsTaskAccepted()` 是它的公开查询。早先允许漏接直接取件、由 `TryAcquireItem` 以取件时刻兜底开始计时，结果是电话沦为纯播片——不接也照样做任务，接听这个动作没有任何后果。那条兜底已删。
- **挂断必须配重拨，这两件不能拆开做。** 拒接/漏接会把电话**移出队列且永不重拨**，所以只加"必须接听"这道门槛、不补重拨的话，挂一次电话那个任务就永久推不动，和"四个状态无失败态"直接冲突。现在 `DeliveryPhoneCallQueueComponent::ScheduleRedial`在 `RedialDelaySeconds`（默认 20 秒）后重拨，重拨前复核"状态仍是待取件且仍未接单"，否则不打；**只重拨 `TaskUnlocked`**，催促电话错过就错过、重拨只会烦人。
- 取件被拦时要分得出原因：「先接电话才能接这一单」和「请先完成正在进行的快递任务」都会让 `CanAcquireItem` 返回 false，但玩家该做的事完全不同（去接电话 vs 先送完手上这单）。
- 全世界同时只有一个已接单并计时的任务：后续任务解锁来电在 PhoneQueue 暂停，当前任务催促电话可插队；`CanAcquireItem()` 同时阻止拿起其他任务快递。
- 四个状态无失败态（Locked / AwaitingPickup / InProgress / Completed），超时只打一次催促电话并降低奖励倍率，任务继续。
- 电话队列 [DeliveryPhoneCallQueueComponent](Source/Delivery/Task/DeliveryPhoneCallQueueComponent.h) 也在 GameState 上，由服务器按配置时长推进，不等客户端播完回报。
- 单个任务的配置是一份 [DeliveryTaskDefinition](Source/Delivery/Task/DeliveryTaskDefinition.h) 资产；关卡的任务清单填在 GameState 蓝图的 `TaskDefinitions` 上（数组顺序 = 同时解锁时的来电顺序）。
- 任务数值由策划在 `Design/Tasks.csv` 里维护，编辑器菜单 **Delivery → Import / Reimport Tasks** 导入成 DataAsset（脚本在 `Content/Python/delivery_task_import.py`，按 TaskId 增量更新，不会冲掉资产上手填的字段）。时间评价档位用**剩余秒数**表达，正数提前、负数超时，和策划表一一对应。
- 调试用控制台命令（`Delivery.Task.Dump` / `Acquire` / `Deliver` / `Event`）见 [DeliveryTaskDebugCommands.cpp](Source/Delivery/Task/DeliveryTaskDebugCommands.cpp)，在 PIE 里不用 UI 就能跑完整个任务流程；`DeliveryTaskDefinition` 有 `IsDataValid` 校验，阈值配反、档位顺序错会在编辑器里标红。
- **钱包**：[DeliveryWalletComponent](Source/Delivery/Economy/DeliveryWalletComponent.h) 挂在 `ADeliverPlayerState` 上（和 ASC 同理——钱要在角色死亡/重生/上下载具之后还在）。**入账全自动**：组件自己订阅 GameState 上的 `OnTaskCompleted`，那个委托自带 `APlayerState* Deliverer`，多人抢单时钱记给谁天然就对，任务系统那边一行都不用改。余额 `COND_OwnerOnly` 复制，不做客户端预测。PlayerState 可能比 GameState 先 `BeginPlay`，订阅不到时每 0.5 秒重试——不重试的话这个玩家整局收不到钱且毫无报错。UI 接 `OnTaskPaid(Task, Reward)` 拿完整奖励明细做结算飘字。
- **地点 ID 解析**：[DeliveryLocationRegistry](Source/Delivery/Task/DeliveryLocationRegistry.h)（WorldSubsystem）+ [DeliveryLocationComponent](Source/Delivery/Task/DeliveryLocationComponent.h)。关卡里的取件点/收件点/收件人 NPC 挂后者、填 `LocationId`（和 `Tasks.csv` 一致），任务定义里那几个裸 `FName` 才能变成世界坐标。三种点共用一个组件，因为在数据上它们是同一种东西；继承 SceneComponent 是为了能带相对偏移（收件点挂在整栋楼上时楼的原点可能在地下，箭头该指门口）。查找走注册表而不是遍历关卡，和交互系统同一个理由：碰撞查询在本项目静默失效过一次。
- **任务地点建筑高亮已停用**：`TaskTracker` 不再给目的地建筑开启 `CustomDepth/Stencil`，也不再定时扫描建筑；罗盘、小地图和脚边箭头继续提供指引。`DeliveryPlayerController` 的后处理仍用于交互物和抓取候选，不能为了关建筑高亮而移除。
- **"该去哪"**：`Tracker::GetTrackedTaskDestination()`。**目的地按任务状态自动切换**（待取件→取件点，进行中→收件点），调用方不要自己判——判错的表现是"箭头指向已经拿过的地方"，四个界面会各错各的。ID 在关卡里找不到对应点时返回 false 并在日志点名；静默失败的话表现只是"箭头不显示"，根因几乎查不到。
- **丢件恢复**：`NotifyItemLost` 把进行中的任务退回待取件，但接电话时开始的计时、催促状态和特殊事件继续保留；`UDeliveryItemComponent::EndPlay` 在 Actor 被销毁时自动上报（掉出世界会走到）。**"正常交付"和"意外丢失"靠执行顺序区分**——`TryDeliver` 先标已完成再销毁快递，所以 `EndPlay` 时状态已不是进行中，`NotifyItemLost` 自己返回 false；**调换这两步会让每次成功交付都被误判成丢件**。退回待取件之后**回取货点再领一件**即可，不再需要关卡里还躺着那件原物——这是取货点流程修掉的那个死锁。
- **指引换算**：[DeliveryGuidanceLibrary](Source/Delivery/Task/DeliveryGuidanceLibrary.h) 返回距离、相对镜头的水平夹角、在不在前方、高度差，UI 画屏幕边缘箭头直接用。**角度基于镜头不是角色**——骑车时镜头和车头可能不一致（摩托车有自由/固定两套视角），用角色朝向算的话自由视角下箭头会指错。
- 顶部水平罗盘条用 `DeliveryGuidanceLibrary::GetTrackedTaskCompass`：目标与 N/E/S/W 都返回同一条水平轴上的 `-1~1` 偏移，乘罗盘条半宽即可。北方取项目设置 `MapNorthWorldYaw`（和小地图同一个值，不再是函数参数）；背后的任务目标会夹在左右边缘持续提示。
- **罗盘条只在接到任务后出现**（`WBP_Compass`，蓝图侧；C++ 只出数据）。
  判据用 `GetTrackedTaskCompass().bHasTarget`，它正好等于想要的条件：
  `Locked`/`Completed` 为 false，`AwaitingPickup`（指取件点）/`InProgress`（指收件点）为 true。
  做法是在 `UpdateCompass`（事件Tick 每帧调）末尾，用那个以 `bHasTarget` 为 Index 的
  「选择」节点（Option 0 = `Collapsed`、Option 1 = `Visible`）去 `SetVisibility`
  **根面板 `CanvasPanel_19`**——只有一处判断。
  - **原来的 bug 是那个 Select 的两个选项都填了 `Visible`**，等于没判断，
    所以目标标记一开局就在。
  - **控件树必须先查清，不能只盯着蓝图变量。** 五个 `Compass_N/S/E/W/Target` 是变量，
    但树里还有个 `Image_138`（那条横线）**不是变量**、压根没有可见性逻辑——
    第一版只把那五个接上，结果"屏幕上还剩一条线"。真实形状是
    `CanvasPanel_19 →（SizeBox_20 → CanvasPanel_97 → 五个 Image）+ Image_138`。
  - **收根面板是安全的，之前"绝对不能收根"那条结论画得太宽。** Slate 不 tick
    `Collapsed` 的控件——但那只适用于 **UserWidget 自己**；`CanvasPanel_19` 是
    UserWidget **内部**的顶层面板，「事件Tick」挂在 UserWidget 上，收它不受影响。
    真要收 UserWidget 自己才会自锁：`UpdateCompass` 再也不跑、`bHasTarget` 变 true
    也永远恢复不了，表现是"罗盘再也不出现"且极难查。
  - **怎么拿到根面板的引用**（三条路试了两条死路，结论值得记）：
    `CanvasPanel_19` 没勾 Is Variable，所以没有 Get 节点；
    `bIsVariable` 是 protected、Python 写不了；`GetWidgetFromName` 五种路径全返回 None。
    最后走的是 **`Get Compass_N` → `GetParent` ×3**（`UWidget::GetParent()` 是
    BlueprintCallable，三次正好到根）。
    **代价**：这条链把"三层嵌套"写死了，以后在 Designer 里加一层容器或改层级，
    链子会指到错的控件或拿到 null（运行时 Accessed None，表现是罗盘一直显示）。
    层级一旦要改，就回去在 Designer 勾上根面板的 Is Variable、换成直接 Get。
  - 脚本（按使用顺序）：`dump_compass_widget.py`（导图表）→ `gate_compass_on_task.py`
    （改 Select 的 Option 0）→ `dump_compass_tree.py`（导控件树，找出那条线）→
    `gate_compass_root.py`（接根面板 + 删旧节点）→ `clean_compass_orphans.py`（清孤立 Get）
    → `compile_compass.py`（编译 + 核对接线）。
- **罗盘图钉按任务阶段切换**：`FDeliveryCompassGuidance.Marker`（`EDeliveryCompassMarker`）——
  待取件 → `Pickup`（箱子图钉 `T_CompassTaskTarget`），进行中 → `Delivery`（房子图钉
  `T_CompassTaskDelivery`），未解锁/已完成/没接任务/地点 ID 解析不出来 → `None`，**整个图钉隐藏**。
  手上已经有包裹却还挂着取件图钉，玩家会折返回取件点，所以 `None` 时连图钉带距离一起藏。
  **UI 不要自己读任务状态判一遍**——判断只在 `GetTrackedTaskMarker` 这一处，和
  `GetTaskDestination` 取同一个状态；两边各判一次迟早不一致，表现是"箭头指送达点、图钉还是箱子"。
- **送达图钉是从取件图钉"改画"出来的**，不是从零再画一枚：
  `Content/Python/generate_delivery_icon.py` 拿 `T_CompassTaskTarget.png` 当底，
  只重绘圆盘正中那个图标（按掩码擦掉箱子、画房子），外圈描边、蓝环、奶油环、
  尖端、橙色箭头、投影全是**字面意义上的同一份像素**，天然 1:1。
  从零画整枚的那条路试过并失败：蓝环被奶油圈挤掉只剩顶上一道、尖端拉成一根针、
  箭头掉到尖上、而且全平涂没有立体阴影——外壳那堆渐变和内外阴影靠参数复刻
  代价极高且永远差一口气，而**真正要换的只有圆盘中间那个图形**。
  - **几何必须打分类图读，不能用射线扫**。第一版用"从中心发射线找奶油边界"，
    射线被正中间的箱子截断，量出来的圆心比真值低 14 像素、半径也偏大；
    后果是顶部一圈箱子描边擦不掉（留两道淡弧）、房子偏右下、左上空一块。
    实测真值：圆盘心 (280, 163.5)、半径 94、箱子连描边最远 88、描边色 `#020c1e`
    （**深海军蓝不是黑**，和图钉外圈同色；用纯黑画出来并排一看就不是一套）。
  - **擦箱子要按掩码、不要整片重绘**。整片重绘时补出来的奶油和外面真奶油之间
    必然留一道接缝（拟合再准也差几个色阶）。掩码判据是"和该位置的奶油模型
    差超过 12 级"——不能用"不是奶油色"，箱子高光 `#fccc90` 会被当成奶油漏掉。
  - **图形尺寸按最远顶点的半径定，不按外接框**。箱子是六边形、四角没有料，
    同样外接框下矩形的房子对角线会戳出奶油圈；而顶点半径还要减去半个描边宽
    （8/2=4），所以顶点留到 84、连描边正好贴住 88，和箱子一致。
  - 房子刻意做得**扁而宽**：角半径 = √((w/2)²+(h/2)²)，高度压下来同样半径才能
    放宽，h=100 时 w 能到 150（接近箱子的 155），h=130 时只剩 134。
  - 脚本带自检：**奶油圈以外被改动的像素必须是 0**，并且报色差幅度而不只报个数
    （裁剪掩码自带 2px 羽化，边界上必然有几十个像素差 1~2 级，只报个数的话
    每次都像出了问题）。
- 导入走 `Content/Python/import_compass_icons.py`（**已提交**——它和生成器都
  因为是未跟踪文件在 `git pull` 的 autostash 恢复失败里丢过两次）。
- 罗盘图钉的美术（早期从零画整枚的版本）：`Content/Python/generate_compass_icons.py` 离线画 PNG 到
  `Content/UI/Minimap/Source/`，`import_compass_icons.py` 在编辑器里导入并配 UI 贴图参数
  （`UserInterface2D` + 无 mip + sRGB，漏任一项分别表现为描边脏边 / 缩小糊成一团 / 整张发暗）。
  调色板和几何都是从原图逐像素实采的（最宽 272px、圆头半径 136、奶油圈半径 100、尖端 y≈390），
  别手改色号。**画图钉时切点必须在圆心下方 ±36°、圆弧走大的那段 252°**——算到圆上方的话
  只画出顶上 72°，头缩掉四分之一、蓝环被奶油圈挤没，渲出来是一根针。生成器里留了自检。
- **鼠标光标归 C++ 统一管，蓝图一律不要写 `bShowMouseCursor`。**
  规则就一句：**光标 = 有没有 UI 面板开着**。`ADeliveryPlayerController::SetUIPanelOpen(PanelId, bOpen)`
  维护一个面板集合，在「有/没有」翻转时设 `bShowMouseCursor` 和输入模式
  （有 → `GameAndUI`、不锁视口、捕获时不藏光标；全关 → `GameOnly`）。
  配套 `IsAnyUIPanelOpen()` / `IsPhoneUIOpen()`。用集合而不是一个 bool：
  同时开手机和任务面板时，关掉一个不该把光标收走。
  **症状是「骑车时鼠标转不了视角，必须按住左键或右键」**——光标开着时鼠标移动归光标，
  视口只在按住鼠标键时才捕获鼠标。根因是 `BP_DeliveryManPC` 原来在手机键上
  `Set bShowMouseCursor = NOT(Get bShowMouseCursor)`，和面板开关是**两个互不相干的
  取反开关**，只要有一次只翻了其中一个就永久失同步。改成「光标是面板状态的函数」之后，
  这种失同步在结构上不可能发生。
  **唯一需要调用方配合的地方**：面板若有「自己关掉自己」的路径（关闭按钮、`Destruct`），
  那里也要调一次 `SetUIPanelOpen(..., false)`，C++ 探测不到 widget 被移出视口。
  按 J 的流程现在是：C++ 翻 `bPhoneUIOpen` → 更新集合（光标跟着变）→ 调蓝图事件
  `TogglePhoneUI` → 蓝图只负责面板显示/隐藏。**IMC 那条路已经拆掉**（`IA_TogglePhone`
  节点已从蓝图删除），CLAUDE.md 里要求的那步迁移这次才真正做完。
  脚本：`diag_input_mode.py`（只读定位）→ `fix_phone_cursor.py` → `fix_phone_event.py`。
  **`WBP_TaskHUD` 有两份真资产**：`/Game/UI/Task/WBP_TaskHUD` 被 `WBP_Main` 引用（在用），
  `/Game/Task/UI/WBP_TaskHUD` 零引用、可删。它那边有 `IsPhoneOpen` 变量和
  `SetPhoneOpen(bool)` setter，所以以后可以把蓝图事件从 `TogglePhone()` 换成
  `SetPhoneOpen(C++ 状态)`，面板就成了 C++ 状态的纯函数，连 toggle 漂移都没有。
- **手机开关键 J 已从 `IMC_Default` 搬到 `BindKey`**（`ADeliveryPlayerController::TogglePhoneKey` → `BlueprintImplementableEvent TogglePhoneUI()`，蓝图实现加/移视口）。理由和物品栏 1~5、切视角 P、召唤 R 一样：`IMC_Default.uasset` 从来没被提交过，每次 `git pull` 都会冲掉，而症状极具迷惑性——手机面板的显示逻辑不依赖按键绑定，表现是"UI 突然坏了"而不是"按键没绑"。F 键已经因此丢过两次。**蓝图那边要把原来绑 `IA_TogglePhone` 的逻辑改挂到 `TogglePhoneUI` 事件上。**
- 调试命令（非 Shipping）：`Delivery.Wallet.Dump` / `Delivery.Wallet.Add <金额>` / `Delivery.Locations.Dump` / `Delivery.Task.LoseItem` / **`Delivery.Task.Validate`**。最后一条是摆取件点/收件点时的主力工具——一次列全所有配置问题（地点 ID 解析不出来、任务没有收件点指向它、收件点忘了填 `ExpectedTask`），不是遇到第一个就停。
- 奖励结算有单元测试（`Task/DeliveryRewardTests.cpp`，筛 `Delivery.Task` 跑）。`EvaluateReward` 是纯函数，档位顺序、超时边界、特殊事件重复计数这些在 PIE 里只表现为"钱好像不太对"，靠肉眼对不出来。
- 仍没做：手机 UI、存档；`DeliveryItemId` / `SpecialEventId` 的解析。对接点见文档第七、八节。

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
  - **第二波车流（2026-09-24）**：`bSpawnSecondWave` / `SecondWaveDelay`（默认 5 秒）——
    关卡开始若干秒后，在每辆车的**出生点**再生成一辆同类车，让车流密度翻倍而不用手摆两遍。
    两个坑都是实测撞出来的：①**不能"先放一辆隐形的、到点显形"**——隐形只影响渲染和碰撞，
    蓝图那套 Drive/Acceleration 计时器照样跑，那辆车会隐形地开走、5 秒后出现在半路上；
    延迟生成才真的"在出生点出现"。②**必须用 `SpawnActorDeferred`**：`SpawnActor` 返回时
    克隆体的 `BeginPlay` 已经跑完，再打"我是克隆体"的标记已经晚了，它自己也会挂上第二波
    计时器，每 5 秒翻一倍、一分钟 4096 辆。延迟生成把 `BeginPlay` 推到 `FinishSpawning`，
    中间那段正好用来打标记 + 用反射把实例上的 `MaxSpeed` 覆盖搬过去
    （不搬的话克隆体全是类默认速度）。出生变换在 `BeginPlay` 当场记下，
    到点再取会拿到半路上的位置。
  - **红灯期间的避让死锁超时放宽到 8 秒（2026-09-17）**：新增 `MaxBlockedTimeAtIntersection = 8.0f` 和 `UpdateStoppedAtIntersection(bool)`。蓝图在 `TraceForIntersection` 的 `SetStopatInter` 之后、以及 `OnRouteLost` 的 `SetStopatInter(false)` 之后各调一次。红灯时前车是"合法地长时间不动"、不是死锁，还按 `MaxBlockedTime`（3 秒）就放弃避让的话，后车会在红灯没结束时压上去；`LightDuration` 是 3 秒，8 秒足够覆盖一整个红灯。
  - **红灯超时改为沿车队传递、并放宽到 10 秒（2026-09-24）**：原来的判据只看**本车自己**的 `StopatInter`，而 `TraceForIntersection` 的前瞻距离和车速成正比——队伍里第二辆之后的车已经被前车逼停、前瞻缩到接近 0，根本探不到路口盒，自己的 `bStoppedAtIntersection` 恒为 false，于是只有队首享受宽松超时，后面几辆照样 3 秒就放弃避让怼上前车。现在组件公开 `IsStoppedAtIntersection()`，`GetDistanceToCarAhead()` 扫描时顺手记下最近那辆车的红灯状态（`bLeaderStoppedAtIntersection`，扫不到车时清 false，否则前车开走后还会一直以为"前面在等红灯"），超时判据变成 `bStoppedAtIntersection || bLeaderStoppedAtIntersection`，红灯状态就能一辆辆往后传。阈值同时从 8 秒提到 **10 秒**：要盖住的不只是一个 3 秒红灯，还有前面几辆依次起步、拉开距离的时间，排队越长最后一辆等得越久。`GetDistanceToCarAhead()` 因此去掉了 `const`（它本来就会写前车的 `MaxSpeed`，并非纯查询）。
  - 车辆碰撞盒保持 `Overlap` **没改成 `Block`**：车是运动学的（`bSimulatePhysics=false`，靠 `SetActorLocation` 沿样条走），Block 在不开 sweep 的情况下根本不起作用；而开了 sweep 会和样条跟随打架（样条要它往前、sweep 把它顶回来）导致抖动。"后车停在后方不超车"靠的是修好之后真正生效的前车探测 + 减速，不是物理阻挡。

- **红绿灯模型（`Content/trafficlight/trafficlight`）自带完整变色循环，不要从外部驱动它。**
  用 `dump_light_bp.py` 导出过真实图表确认：`BeginPlay → set traffic light color(start color)`，
  那个自定义事件 `Switch` 颜色枚举后 `toggle green/yellow/red light` 改三个 SpotLight 的可见性，
  再 `Delay(对应时长)` 递归调用自己换下一色。所以**亮灯只发生在这个事件内部**——
  从 C++ 写它的颜色变量一点效果都没有（这条路试过，组件已撤掉）。要同步只能让它自己的节拍和路口一致。
  - 变量真名**带空格**：`trafficlight colors`（当前色）、`start color`、
    `green/yellow/red light duration`。**没有 `LightColor` 这个蓝图变量**——
    `strings` 扫 uasset 能扫到 `LightColor`，那是 SpotLightComponent 自带的原生属性，不是它。
  - `trafficlight_color_enum` 顺序是 **0=green / 1=yellow / 2=red**。
    用 `strings` 查会得出"只有 green 和 yellow"的错结论——**默认只输出 ≥4 字符的串，
    3 个字母的 `red` 被过滤了**，要加 `-n 3`。
  - **一圈时长必须 = 路口 `LightDuration` × 2**（路口是两相位轮流放行，
    单盏灯的视角就是绿一个周期、红一个周期），黄灯从绿的那段里切出来、不额外占时间。
    原始值绿 10/黄 3/红 20 = 33 秒，和路口 6 秒毫无关系，这就是"灯和车对不上"的根源。
  - **两组灯靠相反的 `start color` 错开相位，而这个值只能存在「类」上、存不进实例。**
    `start color` 带 `CPF_DisableEditOnInstance`（蓝图里没勾 Instance Editable），
    `PropertyAccessUtil.cpp:739` 对非 CDO 对象直接 PermissionDenied；
    而那个开关只能在蓝图界面点——`UBlueprint::NewVariables` 是裸 `UPROPERTY()`，
    Python 读不到也写不了。**写 CDO 是允许的**（挡模板要的是另一个标记
    `CPF_DisableEditOnTemplate`，这个变量没有）。所以做法是：
    `make_light_variant.py` 建一个**只改默认值的空子类** `trafficlight_green_start`
    （一个节点都没有、逻辑全继承，不存在两份逻辑要同步），父类起始红、子类起始绿，
    三个时长写在两个 CDO 上；`gen_traffic_lights.py` 按组号选类。
    `GREEN_FIRST_GROUP` 在两个脚本里都有，**必须一致**，否则两边各按各的分组走。
    灯已经摆好、只改默认值时**不用重摆**：灯没有实例覆盖，改 CDO 即生效。
  - **写这个枚举要传枚举值对象，不能传裸整数**（`ByteProperty` 由 UserDefinedEnum 撑着，
    裸 `int` 报 `Failed to convert type 'int'`）。而 `unreal.trafficlight_color_enum`
    **取不到**——蓝图里定义的 UserDefinedEnum 不作为模块属性暴露。可行的办法是
    **先读回现值、用 `type(现值)` 反推枚举类型**再取成员（实测 `type(现值).GREEN` 成功）。
    兜底时别静默退回整数：那会让报错和改之前一字不差，看着像"改了没生效"。
  - `tune_traffic_lights.py` 现在只当验证工具：起始色由类提供，它在实例上写入被挡是
    **预期行为**，只要现值等于期望值就算过。
  - **已知会漂**：灯和路口是两个独立定时器，蓝图 `Delay` 每次多走不到一帧，
    灯相对路口缓慢往后漂（60fps 下一圈约 25ms）。几分钟试玩看不出；
    要彻底不漂就得让灯读路口相位，那就不是"纯时间"了——选纯时间时接受的代价。
- **`BP_Intersection` 怎么找它管的子节点：查清了（2026-10-26）。**
  `Get Lane Traffic Lights` 里 `GetOverlappingActors(ClassFilter=BP_TrafficLine1_IntersectionChild)`
  —— 就是**它自己的 Box 和段的 Box 重叠**，然后按 `LightNumber` 塞进
  `IntersectionLane`（`Map<int, Traffic_Array>`，**键只有实际出现过的组号**）。
  这解释了为什么"车道一重新生成就得重跑 `gen_intersections.py`"——盒子挪位就罩不住了。
  **脚本要按路口分组时也必须用这个判据**，不能按距离聚类：2500cm 聚类会把相邻
  两个路口并成一个（实测一簇里出现 4 个轴向、同一条路两段差 132°），
  按它染色会把两个路口的段混在一起。
- **单组路口恒红（`Length == 1`）**。`NextTrafficLight` 每 `LightDuration` 秒跑一次，
  固定三步：**①用当前游标放绿 ②游标++并回绕 ③用新游标压红**。
  两组时第③步压的是另一组，正常交替；**只有一组时游标 `0→1→` 回绕成 `0`，
  第③步压的还是刚放绿的那一组**，净结果恒红。绿只存在于同一帧内两个节点之间，
  而车的 `PollForGreen` 是 0.3 秒轮询，永远观测不到那个 `false`。
  组号是 0 还是 1 都一样（全是组 1 时 `Find(key=0)` 找不到、两个分支都空转，
  `StopAtInter` 保持序列化默认值 `True`，同样常红）——**杠杆是 `Length`，不是组号**。
  修法：`fix_single_group_intersection.py` 在第③步前插
  `Branch(Length(IntersectionLane) > 1)`，单组就跳过压红、那组一直绿
  （切向汇入和"交叉路没铺车道"的假路口本来就不该拦车）。
  - **它必须幂等**。第一版没查"图里已经有闸没有"，连跑三次串了三道一模一样的闸
    （节点数 39→41→43→45）。功能无害但会无限累积。现在按
    「Branch 的 `Condition` 来自一个比较节点，而该比较节点的 **A** 接着 `Length`」
    识别已有的闸；`CLEAN_EXTRA_GATES=True` 可以只留一道、删掉多余的并重新接线。
    **只查 A 是刻意的**：图里原本那个 `分支`（判游标回绕）的 `Condition` 来自
    `Equal (Integer)`，它的 A 接 `Increment Int`、B 才接 `Length`，所以不会被误删——
    误删它游标就永远不回绕了。
  - 图里有**两个**长得一样的 `For Each Loop`（放绿/压红），标题完全相同，
    靠它 `LoopBody` 连到的 `Set StopAtInter` 的**默认值是 `true` 还是 `false`** 区分。
    只看标题会挑错、把放绿分支掐掉，表现是全图永远红灯。
- **`LightNumber` 必须按走向二染色，不能按世界坐标轴。**
  生成器原来的 `set_light_number` 判 `|Δy|` vs `|Δx|`（沿世界 X 还是沿 Y），
  本图是自由角度路网，于是两类错都出现了（`audit_light_number_axes.py` 实测
  55 个路口里 10 个错）：
    * 45° 附近落哪边随机——`BranchRoad_R024_Merged` 的**对向车道** 133°→组1、
      135°→组0 被拆开（夹角仅 3°，一个方向走另一个干等）；
    * 两条互相垂直的斜路可能都被判到同一边——实测 91° 和 176°（夹角 85°）都是组 1，
      那个路口永远不交替。
  正确判据（**同轴同组、相交异组**；"同轴"含同向和对向，所以按**无向轴角** `[0,180)` 归类）
  由 `fix_light_number_by_axis.py` 执行，容差 30°。
  - **赋值要最小改动**：第一版按"段数最多的给 0"直接排名赋值，246 段里 151 段"要改"，
    而绝大多数只是把本来就正确的 (1,0) 对调成 (0,1)——行为毫无变化，却弄脏 151 个
    actor，还把真正的 10 处修复淹没在噪声里。改成"每组先要它成员里最常见的现有编号、
    冲突才让小组另选"之后，要改的降到 **8 段**。
  - **这一步要排在 `gen_intersections.py` 之后**（需要路口盒才知道分组），
    而且**每次重新生成车道都要补跑**——重建出来的是全新 actor，手改和脚本改的值都会丢。
  - 手动调过的路口填进 `SKIP_INTERSECTIONS`，否则下次跑它会按轴向重新染色、
    把手改冲掉（尤其"把两个不同轴向并进同一组"这种，在脚本眼里就是错误，它会拆回去）。
  - **两个修复是成对的**：按轴向正确分组之后，切向汇入（两条路夹角只差十几度）的
    路口自然变成单组，`Length == 1` 的恒红就暴露出来——所以上面那道闸是必需的，
    不是可选项。实测分组修完后仍有 7 个单组路口（4 个是切向汇入、3 个是单路假路口）。
- **T 形路口判不出来：检测要加端点规则。**
  原判据只问"两条路的**中线**靠多近"（`XY_CROSS`，当前 400），而 T 形接头的几何是
  **支路停在主路外面**——队友把长样条在路口切断、手工补路之后，支路端点到主路中线
  常差 600~900cm。直接调大 `XY_CROSS` 不行：那是全局阈值，一调大所有**并行经过**
  的路都会被判成路口（表现是"车在直路中间莫名停下等红灯"，极难往这个方向想）。
  所以做**定向放宽**：只有"近点落在某条路的端点上"时才用更大的 `ENDPOINT_CROSS`。
  四种几何的行为（第三条是必须排除的）：
    1. 十字交叉 —— 老规则就命中，端点规则不参与
    2. **T 形**（支路端点 ↔ 主路中段）—— 要的就是这个。走主路时"对方端点离我近"命中，
       走支路时"我站在自己端点上、对方从旁边过"命中；**两边都要命中**，
       才能两条路各自切出 `Inter_` 段、各自有灯
    3. **首尾续接**（端点 ↔ 端点）—— 不是路口，是同一条路线的延续。
       两条子规则都要求"另一头不是端点"，否则每处接驳都会多出假路口
    4. 并行经过 —— 两处都没有端点，不触发，行为不变
  `ENDPOINT_CROSS = 900` 是**从实测分布的空档里挑的，不是手感**
  （`preview_endpoint_junctions.py` 的直方图：400~900 共 99 处、
  **900~1000 整档为 0**、1000~1200 只有 3 处，最大 1188）。要再调先重跑那个预览看分布。
  实测段数 192 → 246。
  - 预览脚本**不能 import 生成器**：`gen_traffic_lanes.py` 的 `run()` 写在模块末尾、
    **import 即执行**，一 import 就等于把全图 552 个 actor 重建一遍。所以判据是
    镜像的，改一边记得改另一边。
  - 兄弟段那个"629cm 上限"只约束**全局调大 `XY_CROSS`**，不约束端点规则——
    `_PartNN` 已经按 `base_road_name` 排除掉了。
- **交通线生成链的完整顺序**（每一步都不能省，后两步各自另起一次 `py` 执行）：
  ```
  gen_traffic_lanes.py → gen_intersections.py → fix_light_number_by_axis.py
    →（另起执行）fill_turn_splines.py →（另起执行）fix_t_junctions.py
  ```
  复核：`audit_light_number_axes.py`（分组）+ `audit_light_groups.py`（单组路口）
  + `preview_endpoint_junctions.py`（端点规则命中）。
  只读诊断：`diag_stuck_intersection.py`（某个路口为什么卡车，一次查全盒子覆盖/
  灯组/端点切线/进出连接）、`watch_stuck_intersection.py`（PIE 期间采样灯态和车状态，
  用每帧回调不是 sleep 轮询）。
- **蓝图图表 API 的几个坑（2026-10-26 全部实测）**：
  * **Branch 有专用入口 `editor.add_branch_node()`**。`create_node_from_name` 是按菜单
    「分类|名字」找的，对 Branch 12 种写法全返回 None（不是抛异常）。
    **先用 `dir()` 或 `list_available_nodes(context_pins)` 把方法表列出来，不要猜名字。**
  * 引脚默认值用 `pin.set_pin_value(value) -> bool`。**返回 bool，值不合法时返回
    False 而不抛异常**——只 `try/except` 会把失败当成功。
  * **UE5 的 `>`/`+`/`==` 是 `K2Node_PromotableOperator`，类型跟着连线推导。**
    不接任何东西时可能解析成任意类型（实测 `Timespan > Timespan`），这时填整数字面量
    会被拒。**必须先接输入、再填字面量**；接上之后标题会变成 `integer > integer`，
    可以用它确认推导生效了。
  * `get_pin_direction()` 返回 `EEdGraphPinDirection.EGPD_OUTPUT`，取尾段是
    **`EGPD_OUTPUT`**，拿它 `startswith("Output")` 永远不成立。栽过一次，
    而失败表现是"没找到那个节点，图改过了？"，看着像蓝图被人动过。
  * 写入 API（`try_create_connection` / `set_pin_value` / `set_editor_property`）
    **返回值和回读都要查**，不能只看有没有抛异常。
  * **验证要写在每一条写入路径上，不能只写在主路径。** 真实事故（2026-10-26）：
    `fix_single_group_intersection.py` 的「添加新闸」路径有回读验证，
    而「清理多余闸」路径的 `keeper.then → 压红ForEach.Exec` 只有 `try/except pass`。
    那一根静默失败，结果**压红分支的 Exec 悬空 → 永不压红 → 全图绿灯、
    红色 DrawDebugSphere 再也不出现、没有车停**，比原来的 bug 更糟。
  * **`BS_UP_TO_DATE` 不代表接线完整。** ForEach 的 Exec 悬空**不是编译错误**，
    所以上面那次事故的报告里编译状态是干净的，光看它发现不了。
    判空要自己查关键引脚的 `list_connected_pins()`
    （`check_intersection_gate.py` 就是干这个的，默认只读，`REPAIR=True` 能按
    正确结构重接）。
  * **改共用蓝图每写一次就要立刻读报告。** 上面那次事故的上游原因是：第一次写入后
    我没读报告就去做别的事，于是连写三次、串了三道一模一样的闸
    （节点 39→41→43→45），才需要那段风险更高的清理手术。
    一次影响 51 个路口的操作，没有"回头再看"的余地。
- **编辑器 Python 脚本只能在编辑器里跑，一个 `NameError` 要绕一整圈才知道**，
  所以写完除了 `ast.parse` 还要查"用了但没定义的名字"——`ast.parse` 只查语法，
  查不出这个。实际栽过：用"从 A 到 B 整段替换"的方式打补丁，把夹在中间的常量定义
  一起删掉，脚本启动即崩、一个值都没写，而报告里只有一行 traceback。
- `gen_traffic_lights.py` 沿路口四个方向各摆一盏灯，打标签 `ClaudeGenTrafficLight:<组号>`，
  组号取自该方向 `Inter_` 段的 `LightNumber`。灯位按 `BP_Intersection` 的 actor 位置算——
  `testfortraffic` 里这个 actor 恰好在路口附近所以没出错，**换图前要改成按 `Inter_` 段算几何中心**，
  因为路口 actor 的原点不保证在路口中央。
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
`spawn_test_cars.py`（清掉所有 `BP_car_base` 并在车道段起点重新摆一批测试车，
六种车型轮换 + 600~1500 的随机 `MaxSpeed`。**不是只读**，见下面「车怎么接上车道」）、
`diag_lane_gaps.py`（接缝间距 + 高度差）、
`diag_seams.py`（逐点查落差来源）、`whats_here.py`（某个 actor 附近都有哪些样条）、
`inventory_level.py`、`audit_traffic_channels.py`（碰撞通道）、`dump_bp_graph.py`（导出蓝图图表）、
`monitor_car.py` / `chase_car.py`（Simulate 期间采样车状态 / 跟拍相机——用每帧回调而不是 sleep 轮询，
Python 跑在游戏线程上，轮询会把模拟本身卡死）。

**车怎么接上车道：摆位是算出来的，不是手感（2026-10-29 从图表 dump 反推）。**
`Saved/bp_graph.txt` 里两段图表给出了全部约束：
```
TraceForNewPath:  球半径 150
                  Start = GetActorLocation()
                  End   = Start + Forward × FrameRate × CurrentSpeed × 5
GetFuturePostionandRotationAlongSpline:
                  每帧 GetDistanceAlongSplineAtLocation(GetActorLocation())
```
- **出生那一刻 `CurrentSpeed = 0`，于是 `End == Start`，这是条零长度扫描——
  UE 的零长度 sweep 不返回命中。** 所以不存在「一出生就接上路」，
  车必须先被加速计时器（0.1 秒一跳）推动起来，探测才有长度。
- **能接上路的最远出生距离 = 150（球半径）+ 83（`LANE_BOX_HALF` 沿行驶方向的
  半尺寸）= 233 cm**，Box 挂在车道段的**第一个控制点**上。超出这个距离，
  车只能沿着出生朝向裸奔瞎撞；撞不到就被 `RouteLostTimeout`（3 秒）缩小消失、
  瞬移回出生点，再撞一次——**表现正是「这辆车永远走不上交通线」**。
  `BEHIND_BOX` 原来是 **300**，正好落在 233 外侧，所以那批车全靠运气。
  现在是 **250**：刻意略大于 233（250−83=167 > 150），出生姿势下球还没压着 Box，
  于是第一次有速度的那一跳是一次**真正的 sweep 命中**，不赌「初始重叠算不算命中」
  这个没实测过的语义；而要走的距离只有 17 cm，裸奔几乎不发生。
- **车不必落在样条起点，也不怕「开过了起点」**：位置是每帧用自己的世界坐标
  现算的。脚本里原来那句「放在起点之后等于一出生就开过了自己的盒子，
  永远接不上路」是错的——门槛是距离，不是先后。
- 配套的四道筛子（都是为了「摆下去的车都能走」，不是为了摆得多）：
  ①出生点底下必须有路面（逐点射线只认 `SplineMesh`，跳过地形/裙边/建筑，
  取离车道点 Z **最近**的那层而不是最上面的——路口处两条路的面是叠着的）；
  ②出生点 300 cm 内不能有**别的段**的第一个控制点，否则焊到谁身上取决于
  sweep 先碰到哪个，车会当场横向瞬移到横向车流或对向车道上；
  ③前方 1050 cm（`ForwardTraceDistance` 900 + `TraceStartForwardOffset` 150）的
  锥体里不能已经有车，否则一出生就互相判成前车集体停住。**判据必须是有向的**
  （沿前方距离 + 横向偏移 ≤ 250），用「车与车的直线距离」会把同一横断面上的
  4 条平行车道误杀掉——它们横向只差 360，但互相不在对方的探测锥里。
  这也是旧版「每隔 `STRIDE` 条取一条」做不到的事：排序按离锚点的距离，
  相邻两条既可能是平行车道也可能首尾相接，`STRIDE` 既误杀又漏放；
  ④车道末端 700 cm 内要有同向下家，否则车开到头就裸奔——那是「路到头了」
  不是「接不上路」，混在一起会把真问题盖住。
- 报告里每辆车都打「到箱心」和「地面差」两列，淘汰的逐条给原因，
  所以「为什么只摆了 N 辆」是可查的，不用猜。
- **「车浮着」和 `CAR_Z` 无关**：焊上样条之后车的高度完全由
  `GetLocationAtDistanceAlongSpline` 决定，也就是车道点自己的 Z。
  调 `CAR_Z` 只会掩盖问题，要跑 `diag_lane_fit.py` 查车道的高度剖面。

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

**加节点也能用，签名已实测**（2026-10-20，罗盘条那次）。都是 `BlueprintGraphEditor`
实例上的绑定方法，**只收自己那几个参数、不要再传 editor**，形状猜错会报
`takes at most 1 argument (3 given)`：
- `editor.add_call_function_node(function_path)` —— 传裸名 `"SetVisibility"` 实测也能解析上
  （正式写法是完整路径）。**怎么确认函数真解析到了**：看新节点上有没有按签名生成的参数引脚
  （`InVisibility`/`self`）——有引脚就说明解析成功，没有就是个编译才报错的空节点。
- `editor.add_get_member_variable_node(member_name, class_path="")` ——
  第二个参数是**类路径字符串**，不是坐标；传 `Vector2D` 会报
  `Cannot nativize 'Vector2D' as 'ClassPath' (StrProperty)`。
- `editor.create_node_from_name(node_with_category, location, context_pins, declaring_class=None)`
  —— 这个才收坐标，按 `'Development|PrintString'` 这种「分类|名字」找。
- 这两个 add 函数**都不接受坐标**，所以新节点全叠在原点附近，图上会挤成一团、连线却是对的。
- 清理用 `remove_nodes`（`remove_*` 系列里只有这个能删普通节点）。
- `try_create_connection` 可能静默失败，**接完要回读 `list_connected_pins` 验证**；
  写完还要 `modify(True)` + 存盘，并且**编译一次**（`BlueprintEditorLibrary.compile_blueprint`，
  Python 能直接编、不必手点；状态读 `bp.get_editor_property("status")`，
  `BS_UP_TO_DATE` 才算干净，`BS_ERROR` 就是接错了线）。

**UMG 控件树 / 蓝图变量那几道墙**（2026-10-20 罗盘条那次全撞过一遍）：
- **`UWidgetBlueprint.WidgetTree` 和 `UWidgetTree.RootWidget` 都是 protected**，
  `get_editor_property` 一律读不到。前者能用 `unreal.find_object(bp, "WidgetTree")`
  绕过去（按子对象名找，不走属性权限检查），**后者绕不过去**——
  所以「从根往下遍历」这条路是死的。
  可行的是**从叶子往上爬**：`unreal.find_object(tree, "<变量名>")` 拿到一个已暴露成
  变量的控件，再用 `UWidget::GetParent()`（BlueprintCallable）一层层到顶，
  然后用 `UPanelWidget::get_children_count/get_child_at` 往下铺开整棵树。
- **`bIsVariable` 是 protected，写不了**（`set_editor_property` 直接报
  `Property 'bIsVariable' ... is protected and cannot be set`）。
  所以「把某个没暴露的控件变成变量再 Get」只能人在 Designer 里勾那个复选框，
  勾完还必须**编译**，变量才真正进入类。
- **`add_call_function_node` 对 `GetWidgetFromName` 解析不上**，
  `/Script/UMG.UserWidget:GetWidgetFromName`、`.` 分隔、裸名等五种写法全返回 None
  （而 `SetVisibility` 用裸名却能解析——**裸名能成是运气，不能推广**）。
  没解析上时建出来的是个**一个引脚都没有的节点**，留在图里会让编译报
  `Could not find a function named`，所以每建完一个节点都要检查引脚、
  没有就立刻 `remove_nodes` 掉。
- **拿不到变量时的替代路子**：从已是变量的兄弟控件 `GetParent()` 往上爬到目标。
  代价是把层级结构写死了，层级一改就指错或拿到 null（运行时 Accessed None）。
- **判断「某个 API 失败是因为数据还是因为 API」要带对照组。** 实测
  `add_get_member_variable_node("CanvasPanel_19")` 返回 None，光看这一条
  分不清是「它不是变量」还是「这个 API 对面板类型不管用」；
  同一次调用拿已知是变量的 `Compass_N` 跑一遍（成功），答案立刻就清楚了。
- **返回 None 也必须打日志。** 有一版在返回值为 None 时一条日志都不打，
  报告里整段消失，看着像「这段代码根本没跑」——静默失败伪装成没执行，最难查。
- **删节点的判据要按连线、不能按标题。** `Compass_N` 这类变量在同一张图里可能有
  **两个 Get 节点**（一个喂 `SetRenderTranslation` 管位置、一个喂 `SetVisibility` 管显隐），
  按「标题是 Get Compass_N」删会把管位置的那个一起删掉、罗盘位置逻辑整个断。
  按「输出引脚没有任何连线」删才是自我保护的：还在被用的节点天然有连线、选不中。
- **exec 输出引脚只能连一处**，所以把上游接到新节点时旧链会自动断开——
  「先接新的、验证通过再删旧的」这个顺序因此是安全的，删除只是清理、不是功能的一部分。

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
- 人物和摩托车自由视角的鼠标灵敏度 `MouseLookSensitivity` 默认 0.75（原输入的 75%）；手柄右摇杆不变，摩托车固定车尾视角仍不响应鼠标。
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
  **来车那边有一张冷却表 `LastImpactTimeByActor`（0.75 秒）**，一次碰撞会连着好几帧重叠，不设冷却的话"撞两下才下车"会在同一次碰撞里就被扣完。
  **但那张表只够挡"同一辆车连着撞"**——它是 `UDeliveryTrafficCarComponent` 的成员、**每辆车各一份**，
  多辆车可以在同一帧各打一下，`ImpactsToDismount`（2）于是在一次碰撞里被扣满，
  玩家体验是"被撞一下就直接下车加晕倒"。日志实证（2026-10-15）：
  同一帧两条记录、来车速度完全相同（很可能是第二/三波克隆车和本体重叠在同一位置、速度向量一致）：
  `[12.33.55:490][678] 被交通车撞击 1/2 次（来车速度 3053 cm/s）` 紧跟一条 `2/2 次`。
  所以摩托车自己再挡一道 **`MinImpactInterval`（默认 1 秒）**，在 `NotifyTrafficImpact` 开头、
  **计数/音效/击退之前**判断并返回 false（放后面等于白挡；返回 false 让来车不记录这次命中，
  它下一帧还会来问、继续被挡，代价只是一次比较）。次数是摩托车自己的状态，
  谁来撞、撞几辆都该由它说了算——这才是对的层。上下车和召唤时和 `TrafficImpactCount` 一起清零。
  **还没修的相邻问题**：`CarVelocity = Travel / DeltaTime`（每帧位移 ÷ 帧时间），
  所以 `OnRouteLost` 把车瞬移回出生点那一帧会算出**假的巨大速度**（实测 3053 cm/s，
  而车的 `MaxSpeed` 随机范围只有 600~1500），抛射力度被顶成 100%。
  想修就得在位移明显超过 `MaxSpeed × DeltaTime` 时跳过那一帧的撞击扫掠；
  **注意这条路同时供撞角色的伤害计算使用**（按相对速度算伤害），改之前先读那一段的注释。
  每撞一下：按来车方向给击退速度、按左右分量给撞歪的角速度和车身倾斜（`KnockTilt` 直接叠在转弯侧倾上，**不再插值**，插了会把撞击那一下的尖峰抹平）、镜头两轴不同频率抖动。四个量统一按 `KnockDecay` 指数衰减。
  撞满 `ImpactsToDismount`（默认 2）次 → `ExitVehicle()`（和按 F 同一条路）→ 再 `KnockDownDriver()`：走既有伤害 GE 把血清零（回血 GE 可能抵消，所以跟着补一次 `SetNumericAttributeBase`，和撞人那边一样），ASC 自己会切 Stunned/Limp，复用现成的倒地表现，不另写一套。**顺序不能反**：`ExitVehicle` 内部的 `StartRagdoll` 会重建刚体，先加的冲量会被冲掉。
  被掀下车时那记抛射的力度**按来车速度缩放**（`KnockDownSpeedReference` 1200 cm/s 到满、`KnockDownLaunchMinScale` 0.35 保底）：交通车的 `MaxSpeed` 本来就是 600~1500 随机的，一律同一个飞法就看不出"被快车撞"和"被慢车蹭"的区别；保底不能给 0，否则慢车蹭一下人是原地瘫软，看着像自己躺下的。
- **晕倒的人不能按 F 上车。** 判据是 `ADeliveryCharacter::IsIncapacitated()`——读**复制过来的 `State.Stunned` Tag**，不是 ASC 上那个 `bStunned`（它只在服务器维护，客户端恒 false，本机提示照样会弹），血量 ≤0 兜一刀防 Tag 还没同步。两处都要判：探测组件里清掉 `Focused`（同时挡住提示浮窗和按键，`DoInteract`/`DoPickup` 拿的都是它，不用每个调用点各写一遍），`TryEnter` 里再判一次（按键走 Server RPC，客户端状态不可信）。
- **"按 F 下车"只在上车后显示 `ExitPromptDuration`（2 秒）。** 它吊在车顶上方、一直挂着挡视野，而这条信息玩家看一次就记住了；"按 F 驾驶"那条本来就只在 `InteractRadius` 内出现（`FindBest` → `CanInteract` 逐个比距离），不需要额外做什么。
- **骑车时骑手的材质换成驾驶员自己那套**（`bUseDriverMaterials`，默认开）：骑手那 9 个槽共用一个 `tripo_mat_c9b1ab96`，**里面一张贴图都没有**，进游戏是一块灰白。
  **映射按槽位名一一对应。** 玩家网格 `Content/Characters/A/renwu` 和车模型自带的骑手**是同一个基础角色**（都是 Tripo 出的），槽位名完全一致：`tripo_mat_c9b1ab96` / `材质` / `材质_001`…`材质_006` / `Eyes_Black`；区别只在玩家那边给这 9 个槽配了 `character_hat`（绿帽）/`cloth`（黄衣）/`pants`（浅蓝裤）/`shoe1`/`shoe2`（深蓝鞋）/`socks`/`skin`/`eyes`/`prime`。同一个基础角色也意味着 UV 是同一套，贴过去就是玩家本人的样子。`RiderMaterialOverrides` 按下标手填可覆盖；没人骑时 `EmptyOverrideMaterials()` 还原。
  **踩过的坑**：第一版写成"名字里带 eye 的配眼睛，其余所有槽都用第一个非眼睛材质"，结果整个人被涂成衣服那一种黄色。起因是先用 `grep`/ASCII 扫 `.uasset` 判断槽位数，**`材质_00x` 是以 UTF-16 存在名字表里的，ASCII 扫描一条都看不见**，于是误判骑手只有 2 个槽。查 `.uasset` 里的中文名要按 UTF-16 扫，别用 `strings`。
- **骑车时的镜头有两套，局内按 `P` 随时切**（`CameraToggleKey`，默认 `EKeys::P`；`bFreeLookCamera` 是初始模式，默认开 = 自由视角）。两套设置都完整写在 `ApplyCameraMode()` 里，没有哪一套是被删掉的。
  - 切视角仍走 `BindKey` **直接绑键**，物品栏 1~5、召唤 R 也是旧的直接绑定；这些功能没有对应 IA，不能只改 IMC。`IMC_Default` 现已提交，角色和摩托车共用其中的移动、视角、攻击与交互 IA。查 `.uasset` 按键时只能按 FName 名字表扫，按 ASCII 正则乱扫单字母会把一堆无关字符串认成按键。
  - 切换那一帧要接控制旋转，否则视角会跳：切到固定视角调 `SyncControlRotation()`，切到自由视角保留当前朝向当起点。
  - **自由视角**（开）：弹簧臂 `bUsePawnControlRotation=true`，`bInheritPitch/Yaw` 都必须设成 true——`USpringArmComponent::GetTargetRotation()` 会拿组件的相对角度把没继承的那一轴顶掉，只开一个就只剩一个轴能转。鼠标/右摇杆复用角色身上同两个 IA（`IA_Look` / `IA_MouseLook`），俯仰上下限走 `PlayerCameraManager` 的 `ViewPitchMin/Max`。**Tick 里必须停掉 `SyncControlRotation()`**：自由视角下控制旋转就是镜头，每帧同步成车头朝向等于把鼠标抹掉。
  - **固定车尾视角**（关）：`bUsePawnControlRotation=false`、只继承 Yaw、俯仰吃 `CameraPitch` 的相对角度，鼠标完全不参与，"W 永远是往屏幕里开"。
  - **自由视角下 WASD 改成按镜头的前后左右**（`bCameraRelativeMoveInFreeLook`，默认开）。
    固定视角下这个开关自动不生效——那套镜头的 yaw 本来就跟着车头，两种解释等价，
    多绕一层换算只会引入误差。关掉开关就回到原来的"W 永远是车头方向"。
    实现是把二维输入在镜头 yaw 下解释成"想去的世界方向"，再换算成这套车**原有的**
    油门 + 转向量，不是另写一套移动，所以爬坡、撞墙掉速、压弯、轮子自转全部照旧。
    - **换算必须每帧在 Tick 里做**（`ApplyCameraRelativeInput`，排在 `UpdateSpeed`/
      `UpdateSteering` 之前），不能放在按键事件里：镜头每帧都在动，按一次算一次的话
      松手之前方向就不再更新。原始输入存在 `MoveInputRaw`，三处清输入的地方要一起清。
    - 取**控制旋转**的 yaw 当镜头朝向，不取 `FollowCamera` 的世界朝向——后者带弹簧臂的
      迟滞（`CameraLagSpeed`），用它会让行进方向跟着镜头晃、手感发糊。
    - `atan2(X, Y)` 不是 `atan2(Y, X)`：UE 的 yaw 以 +X 为 0 度，而这里的"前"对应
      输入的 Y 分量，两者正好差一次参数对调。
    - **油门下限 `CameraRelativeMinThrottle`（0.4）不能给 0。** 转向量乘的是
      `|车速| / TurnSpeedReference`，车停下来完全不能转（刻意的，停着打把原地转圈很出戏）。
      想掉头时若把油门收到 0 就是 停住 → 不能转 → 永远掉不了头 的死局。
    - **S 是后退，不参与方向换算**（在 `ApplyCameraRelativeInput` 里单独一条分支拦掉）。
      纯按方向解释的话 S = 往屏幕下方开 = 掉头，但按 S 想后退（以及先刹停）
      是所有驾驶游戏的肌肉记忆；**掉头应该是"把镜头转过去再按 W"**。
      实现是把负的 Y 直接当油门交给 `UpdateSpeed`：还在前进时它按
      `BrakeDeceleration`（2200，比加速的 950 猛得多）刹车，停下后继续按就往
      `MaxReverseSpeed`（400）倒。不需要额外状态机，也没有新参数。
      转向用 `MoveInputRaw.X` 原值（车头相对），`UpdateSteering` 里 `Direction`
      已按车速取负，所以**倒车时左右不会反**。
      **A/D 保持"往屏幕左右开"不动**——按驾驶惯例改成纯转向没有意义，
      这套车的转向量本来就乘 `|车速|/TurnSpeedReference`，停着打把转不动是刻意的。
    - **刹车（`CameraRelativeBrakeAngle` 120°）针对的是"高速行驶中把镜头转过头再按 W"**：
      不先把速度卸掉就会以当前车速画一个巨大的弧，看着像失控漂出去。和 S 没有关系。
    - **大角度转向分两段，而且"刹车还是掉头"必须锁住**
      （`CameraRelativeStageSideAngle` 90°、`CameraRelativeBrakeExitSpeed` 25 cm/s）。
      症状：**大角度下车原地颤抖**，要抖到某个角度才突然顺畅，而 A/D 正常；
      停住按 S 最明显，但**不止 S，角度一大就会**。两个独立来源叠在一起，缺一个都还抖：
      1. `Error` 用 `NormalizeAxis` 归到 `(-180, 180]`，**180 是奇点**——鼠标或车身一丝抖动
         就让它在 +180 和 −180 之间翻号，`SteerInput` 被 clamp 成 ±1、满舵方向每帧左右互换。
         **停住按 S 必然踩中**：自由视角的镜头通常就在车尾正后方，`CameraYaw ≈ ActorYaw`，
         `Error` 恰好 180。A/D 没事正是因为那边 `Error ≈ ±90`，离断点最远。
      2. **刹车分支和掉头分支在同一个速度阈值上来回弹**（这条才是主因）：
         `bMovingForward` 的判据是 `CurrentSpeed > TurnSpeedReference * 0.25`（112.5），
         车一加速越过就进刹车（油门 −1、**舵角归零**），刹到阈值以下又回掉头
         （油门 +0.4、**满舵**）——**进出同一个阈值、没有滞环**，就是个极限循环，
         油门和舵角每几帧翻一次。**这条和角度是不是 180 无关**，所以任何大角度都中，
         而且要一直抖到误差小于 `CameraRelativeBrakeAngle` 才跳出来。
      对策都是**锁定，只在动作开始那一帧判一次**：转向侧 `TurnAroundSign`（只读一次
      `Error` 的符号）、刹车与否 `bBrakingForTurn`（只读一次速度和角度）。
      超过 90° 时第一段只朝锁定那侧的**侧面**打舵，转过侧面解锁、第二段由真实误差收尾。
      刹车一旦开始就刹到 `CameraRelativeBrakeExitSpeed`（远小于进入阈值 112.5）才松，
      于是是"一次干净的刹停 + 一道连续的 U 形弧"，不再往复。
      油门的 `cos` 衰减也要用分段后的误差，否则第一段按最终方向算会把油门压到下限、
      掉头全程只剩最低速。两个锁定值在松手、上下车、召唤、切视角时都要和 `MoveInputRaw` 一起清掉。
    - 切视角那一帧要按原始输入重算一次：两套模式下 `SteerInput` 语义不同
      （镜头相对是"到目标方向的误差"，车头相对是"转向量"），不接这一下按着 W 切过去会有一帧按旧语义开。
  - 代价写在这里免得以后重新纠结：**关掉**镜头相对开关时，自由视角下
    W 按的是车头方向、不是屏幕里的方向，镜头转到侧面时按 W 车还是往自己车头那边开。
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
[DeliveryVehicleSummonComponent](Source/Delivery/Vehicle/DeliveryVehicleSummonComponent.h) —— 挂在玩家 Pawn 上，按 R 把最近一辆没人骑的摩托车挪到身前，冷却 10 秒。地图没有摩托车时，服务器通过 `DefaultVehicleClass`（默认 `/Game/Vehicle/Motorbike/BP_Motorbike`）生成首辆车；已有车被占用或超出搜索范围时不额外生成。生成后仍走 `SummonTo` 探地，失败销毁临时车且不计冷却。回归检查：`Delivery.Vehicle.Summon.EmptyMap`。
- **单独一个组件**而不是塞进 `ADeliveryCharacter`：它需要**按固定频率 Tick**来刷左下角提示，而角色的 Tick 是 `bStartWithTickEnabled=false`、只在被车撞的镜头拉远期间才临时打开的，借它推 HUD 会把那套按需开关搅乱。
- 冷却是**服务器权威**的：`ReadyServerTime` 服务器写、`COND_OwnerOnly` 只复制给拥有者，客户端只拿来显示，不做预测（召唤本来就要等一个来回）。**探不到地面的召唤不计冷却**——玩家什么都没得到，不该被罚等 10 秒。
- 落位交给 `ADeliveryMotorbike::SummonTo()`，车自己往下打地面射线，并**清空 `CurrentSpeed` / `VerticalVelocity` / 油门转向 / 被撞状态**：残余车速会让车刚出现就从脚边滑走，残余被撞状态会让它一边抖一边歪着出现。车上有人时拒绝，不能把别人正骑的车抽走。
- `PlaceDistance` 默认 230cm，要大于车长（256cm）的一半再留余量，否则车会压在玩家身上把布娃娃顶翻。

### 屏幕左下角常驻提示
`UDeliveryPromptSubsystem::PushCornerHint()` 和世界浮窗 `PushPrompt()` 是**两个独立槽位**，各有各的时间戳，同一个 SConstraintCanvas 上两个锚点。分开是必须的：合在一起的话走到车边时"按 R 召唤"会被"按 F 驾驶"顶掉。同样是"每帧推一次、停推 0.25 秒自动消失"的约定，所以不需要成对写 Show/Hide。
- 步行时由 `UDeliveryVehicleSummonComponent` 推召唤提示（冷却中压暗成灰色 + 读秒）；骑行时由 `ADeliveryMotorbike::PushCameraHint()` 推"按 P 切换视角（当前：自由/固定）"。
- **两者天然不会打架**：上车后控制器去 Possess 摩托车，被丢在车上的那具身体 `IsLocallyControlled()` 变成 false，召唤组件自动停推。
- 提示里的键名一律从**实际绑定的 FKey** 取 `GetDisplayName()`，改了 `SummonVehicleKey` / `CameraToggleKey` 提示会跟着变，不会说一套做一套。
- 配置了默认车型时，即使地图没有摩托车也显示召唤提示；仅在场景无车且 `DefaultVehicleClass` 留空时隐藏。

### 摩托车资产接入（`Content/Python/setup_motorbike.py`）
菜单 **Delivery → Setup Motorbike**，或控制台 `py setup_motorbike.py`。四步幂等、可单独重跑：
**摩托车现在是自配置的，`setup_motorbike.py` 从"每次改完必须跑"降级成可选。** 这是被"又要重跑一次脚本"逼出来的结论，别再往回走：
- **凡是脚本算得出来的几何，C++ 自己也能算。** `AutoConfigureFromMeshes()` 在 `ApplyBodyMeshes()` 开头跑，用和脚本**一模一样**的判据（轮子=正圆且窄、车把=X 最宽、前轮前叉=中心落在前 1/4、转向轴=车把与最前部件的水平中点）从 `UStaticMesh::GetBoundingBox()` 现场量。顶点本来就在 FBX 场景绝对坐标里（导入时 `transform_vertex_to_absolute=True`），包围盒即整车里的位置，不用再拼相对变换。
- **只填空的**：已经有值的一律不碰，脚本量过的和在 BP 上手调过的都算数。
- **组件上的资产引用要有软引用兜底。** `RiderMesh` 的网格引用存在 BP CDO 里，**组件类型一变就会丢**（`USkeletalMeshComponent` → `UPoseableMeshComponent` 那次就丢了，表现是"车上突然没人"），只能靠重跑脚本修。现在 `RiderMeshAsset`（`TSoftObjectPtr`）在组件为空时自动加载。和 `InteractAction` 用软引用是同一个理由：构造函数只在模块加载时跑一次，软引用才能在运行时解析。
- **不要改成"编辑器启动时自动跑脚本"**：`place_in_level()` 会挪动/重放关卡里的车，每次开编辑器都重来一遍；而且打包后根本没有 Python，自配置在打包版里一样有效，自动跑脚本没有。
- 脚本保留的价值：导入 FBX、建 BP 和 IA、摆进关卡、以及在报告里把量出来的数字打给人看。**改 C++ 默认值（手感、镜头、爬坡参数等）从来都不需要跑它。**

导入 FBX → 建 `IA_Interact` 并在 IMC_Default 上映射 F → 建 `/Game/Vehicle/Motorbike/BP_Motorbike` → 在当前关卡出生点前方放一辆。报告写到 `Saved/setup_motorbike.txt`。

**车体网格的位置（2026-09-24 改过）**：正式位置是 `/Game/model/vehicles/motor`，不再是 `/Game/Vehicle/Motorbike/Parts`。队友在 `673f3cb 调整地图` 里把 9 个网格移了过去，老目录只剩 1.5KB 的 `ObjectRedirector`（真网格 20KB~280KB，看文件大小就能分辨）。脚本因此炸过一次，症状是**报告里前后自相矛盾**：第 1 步"车体资产已存在，跳过导入"、第 3 步"没有车体网格"——因为跳过判据问的是"目录非空"（重定向器和材质都算），而收集判据问的是 `isinstance(asset, unreal.StaticMesh)`（重定向器不是）。现在两处统一走 `_collect_body_meshes()`，并按 `BODY_DIRS`（新位置优先、老位置兜底）搜索，遇到重定向器会在报告里点名。**重定向器跟不过去**：`UObjectRedirector` 是 intrinsic 类，`DestinationObject` 连 `UPROPERTY` 都不是，Python 读不到目标路径——所以靠的是显式目录列表，不是解析重定向。

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

### 人物电子语音系统

[DeliveryDialogueVoiceComponent](Source/Delivery/Audio/DeliveryDialogueVoiceComponent.h) 默认挂在
`ADeliveryPlayerController` 上，用 `USynthComponent` 在运行时合成独游常见的 text blip 电子语音，
不依赖音频资产。只在本机 Controller 启动、不复制；多人只同步台词状态，每台客户端自行发声。
文字按字符计时，普通字符从固定小音阶选音，中文/英文标点只产生停顿，问句末尾上扬、普通句尾下降；
单个字内部还有快速滑音、失谐副振荡器、短噪声音头、弱颤音和四组 Hash 选择的弱共振峰，
让它有近似“口型”的腔调但不形成可识别语音。同一句用稳定 Hash 选音、音色和节奏，重复播放不会
每次随机变调。五种内置声线是
`Normal / Low / High / Robot / Angry`。电话/NPC UI 调 `SpeakText*`，绑定 `OnGlyphRevealed`
逐字显示，不能各自再建一套 Audio Component。电话任务已经由本机 PlayerController 自动监听
`OnPhoneStateChanged`：进入 `InCall` 播放任务资产的 `Dialogue`，离开通话立即停止；服务器的
`DurationSeconds` 仍是结束通话的唯一权威，不能拿本机 `OnLineFinished` 推进队列。逐字事件由
PlayerController 自动累积到 `DeliveryPromptSubsystem` 的屏幕下方字幕槽，带来电人姓名、自动换行，
挂断时清空，不要求 WBP 额外绑定。非 Shipping 验收命令：
`Delivery.Voice.Test [normal|low|high|robot|angry]`、`Delivery.Voice.Say <文字>`、
`Delivery.Voice.Stop`。完整接口和扩展边界见 [Document/AudioSystem.md](Document/AudioSystem.md)。

游戏事件音效不经过一套全局管理器，统一以 `USoundBase` 插槽放在真正拥有事件的类上：
`BP_DeliveryMan` 填成功取件/交付，`RagdollCombat` 填左右拳，`GrabComponent` 填抓取/松手，
`BP_DeliveryManPC` 填手机切换/来电/接听/挂断，`BP_Motorbike` 填发动机循环、上下车、交通撞击、
召唤和切视角。取件/交付只在服务器判定成功后由所属客户端播放；车辆权威事件多播空间音；
左右拳和抓取走本机预测保证反馈及时。发动机音频资产自身必须循环，C++ 按车速调 Pitch/Volume。
字段表和填写位置见 [Document/AudioSystem.md](Document/AudioSystem.md#游戏事件音效槽)。
电话与任务已有六个程序生成的占位 SoundWave（`/Game/Sound/SFX`），并已绑定到
`BP_DeliveryManPC` / `BP_DeliveryMan`；原始 WAV 在 `Design/Audio/GeneratedSFX`，可用
`Content/Python/generate_gameplay_sfx.py` 重生成和重导入，不含第三方采样。当前第二版为 48kHz
立体声：电话音使用动态 FM，任务提示使用非整数泛音钟体，并带短反射、低通和轻微软削波。

**脚步与出拳音效**（2026-10-22）。`UDeliveryActiveRagdollComponent::OnFootPlanted`
（`bLeft` + 落点）是落脚的公开委托，脚印、扬尘以后都接这里。

- **广播点是 `SetControlEnabled(Foot.Control, false, ...)` 那一行，不是 `Alpha >= 1`。**
  `UpdateFootTarget` 里 `Alpha` 到 1 之后还有一段
  `if (Gap > 18 && ...) { Alpha = 0.999f; return; }` 在**等物理脚追上目标**——
  那时候脚还悬在半空，按 Alpha 判会让声音比画面早小半步。
  只响一次由函数开头的 `if (Alpha >= 1.0f) return;` 保证，不需要额外的 bool。
- **门槛按"这一脚走了多远"（`FootstepMinStride` 18cm），不按身体速度。**
  站着不动时平衡恢复会不停踏小碎步，按速度判的话人站着也在响、一秒好几下。
  音量再按迈步距离从 0.35 缩放到 1.0（`FootstepFullStride` 70cm），碎步轻、大步重。
- 左右脚**故意给不同音高**（0.94 / 1.06）：吸盘那种卡通音一高一低交替才听得出
  "左右左右"的节奏，同音会糊成一串。
- `FootstepSounds` 是**数组不是 Sound Cue**：C++ 里挑变体时顺手避开上一次刚放过的那个，
  而 Cue 的 Random 节点要手勾 "no repeats" 才有这个行为，漏勾的表现是
  "偶尔连着两下一样"、极难复现。
- 用 `PlaySoundAtLocation` 而不是 `PlaySound2D`：取件/交付那两个是"给你本人的反馈"所以 2D，
  脚步是世界里真实发生的事，别人的脚步要能从方位上听出来。**不复制**——
  布娃娃在每台客户端各自模拟，每台机器自己为看到的每个角色触发，远端脚步天然就有。
- **出拳分挥空和命中两个音。** 挥空音（`LeftPunchSound`/`RightPunchSound`）早就有、
  时机本来就对：在 `FirePunchDrive()` 放拳那一刻、本机预测、音源在手骨上。
  缺的是命中音 `PunchHitSound`，走 `MulticastPlayPunchHit`——命中判定只在服务器做
  （`OnHitWindow` 开头那句 `IsNetAuthority()`），客户端不知道打中没打中，
  预测的结果是挥空也响。为此 `UDeliveryRagdollCombatComponent` 构造时
  `SetIsReplicated(true)`（它没有要复制的属性，开这个只为了组件 RPC 能发出去）。
  一拳扫到两个人**只响一次**，`bAnyHit` 标记放在循环末尾、所有 `continue` 之后。
- 音效由 `Content/Python/generate_physical_sfx.py` 纯合成（8 个脚步变体 + 左右挥拳 + 命中），
  WAV 在 `Design/Audio/GeneratedSFX`，跑一次会导入 `/Game/Sound/SFX` 并写进
  `BP_DeliveryMan` 两个组件的 CDO。**吸盘质感的关键是音高自己在动**
  （一个短促的下滑再上滑），不是音色复杂——真实录音的"踩地"都是宽频噪声、没有这个滑音，
  所以找素材反而更难对上。滑音**必须累加相位**，用 `sin(2*pi*f(t)*t)` 会在频率变化处
  相位跳变、夹出噼啪声，而且只在变化快的地方出现，听着像"素材本身有爆音"。
- **变体"听着一样"在报告里完全看不出来，所以脚本带两两相关度自检**（阈值 0.85）。
  第一版基频用 `rng.uniform` 取，8 个里撞出两对相关度 0.95+，而脚本照样打印
  "已写 8 个文件"、峰值和 RMS 全正常。改成**按变体序号均匀铺开**才有保证；
  音色参数用**另一个顺序**（乘 5 取模，5 与 8 互质）铺，音高接近的两个在音色上一定隔得远。
  **真正的主因一度被猜错**：以为是音高不够散，把跨度从 ±1.2 提到 ±1.6 个半音，
  相似度反而从 0.854 升到 0.872——这一下否掉了那个猜测，才找到真凶：
  低频"咚"那一层在所有变体里是同一组参数（92→62Hz、同音量同衰减），
  而它是低频段最响的东西，于是任意两个变体都因为这一层强相关。让它也跟着变体变之后，
  28 对的最高相似度降到 0.76。

**背景音乐**：[DeliveryMusicComponent](Source/Delivery/Audio/DeliveryMusicComponent.h) 挂
`ADeliveryPlayerController`（和语音组件同位置——骑车时控制器 Possess 了车，挂角色会在上车瞬间断）。
纯本机、不复制。三条轨道插槽填在 `BP_DeliveryManPC` 上：`ExploreTrack` / `TaskActiveTrack` /
`OverdueTrack`，**逐级回退，只填探索轨就能用**；回退后若和当前同一资产则不重起，
否则会"任务一进行中音乐就跳回开头"。电话响铃/通话期间自动压到 `CallDuckVolumeScale`（默认 0.25）——
**这是必须的**，电子人声是窄频段细碎方波，不压就盖住台词，而台词是任务信息的载体。
手动闪避 `DuckFor()` 与电话闪避**取更低值而非相乘**，相乘会压得过头。
**没有引入 SoundClass/Submix**：闪避直接乘在音频组件音量上；那套东西是给"音乐/音效分开调"
的设置项用的，等有设置菜单再加。占位音乐用 `Content/Python/generate_music.py` 纯合成，
**无缝循环靠把频率吸附成"一个循环整数个周期"而不是首尾淡化**，导入时必须设 `looping=True`
（否则表现为"开局有音乐、过一会儿安静了"）。验收：`Delivery.Music.Dump` / `.Volume` / `.Duck` / `.Toggle`。

## 开发环境与编译

- **引擎：UE 5.8。** 编译走 `Build.bat`，目标是 **`DeliveryEditor`**（不是 `Delivery`——只编后者的话
  新的 C++ 组件不会在编辑器里注册，`DeliveryTrafficCarComponent` 就因为这个"在编辑器里找不到"过一次）：

  ```
  "<引擎目录>/Engine/Build/BatchFiles/Build.bat" DeliveryEditor Win64 Development -Project="<工程目录>/Delivery.uproject" -WaitMutex
  ```

  引擎目录**每台机器不一样**，Epic 默认装在 `C:\Program Files\Epic Games\UE_5.8`。

- **Mac 上的运行时日志在 `~/Library/Logs/Unreal Engine/DeliveryEditor/Delivery.log`**，
  **不在**工程的 `Saved/Logs`（那个目录在 Mac 上根本不存在，别去那里找然后得出"没存日志"的结论）。
  排查运行时问题先读它：`UE_LOG` 自带时间戳和帧号，比在 PIE 里轮询属性靠谱得多。
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

- **`Content/Input/IMC_Default.uasset` 已提交。** 手柄布局：左摇杆移动、右摇杆视角、A/× 跳跃、
  LT/L2 左拳、RT/R2 右拳（双扳机沿用双键抓取）、X/□ 拾取、Y/△ 交互、十字键上打开电话；
  键鼠映射并存。用 `Content/Python/setup_gamepad_input.py` 可幂等补回这些手柄映射。
  物品栏 1~5、切视角 P、召唤 R 仍是 `BindKey` 直接绑定，不属于 IMC/IA，后续适配对应手柄功能时要单独处理。

## 代码结构速览

```
Source/Delivery/
├── DeliveryCharacter.{h,cpp}        角色 Pawn，输入、组件组装
├── DeliveryGameMode.{h,cpp}         最小 GameMode，具体类在蓝图里配
├── DeliveryGameState.{h,cpp}        承载全局共享状态（任务管理器 + 电话队列）
├── DeliveryPlayerController.{h,cpp}
├── Audio/                            本机程序化人物语音、预设与控制台验收命令
├── Combat/                          战斗类型、姿势定义、战斗接口
├── GAS/                             AbilitySystemComponent、AttributeSet、PlayerState、GameplayTags、Abilities/
├── Grab/                            双键抓取：抓取组件、可抓取组件、可抓道具
├── Interaction/                     走近按 F：可交互组件、探测组件、Slate 浮窗子系统（含左下角常驻提示）
├── Inventory/                       五格物品栏、手持道具
├── Ragdoll/                         主动布娃娃、布娃娃战斗组件
├── Traffic/                         交通车辆辅助组件（卡住重置循环、前车避让减速）
└── Vehicle/                         可骑载具（摩托车，运动学街机式 + 车尾货架）+ 按 R 召唤载具组件
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

任务 HUD 编译依赖：`DeliveryTaskHudComponent` 调用 `DeliveryPromptSubsystem::PushObjective`，提示子系统的声明、实现和顶部任务栏必须一并保留。HUD 的重试常量使用 `TaskHudBindRetrySeconds`，避免 Unity Build 将多个 cpp 合并后与钱包的匿名命名空间常量重名。


### 脚边箭头与圆形小地图
- `Task/DeliveryGroundArrowComponent` 默认挂 PlayerController，本机在人物周围（95 cm）/摩托车周围（180 cm）显示高饱和橙黄双 V 箭头。目的地统一取 `GetTrackedTaskDestination`，随取件/交付阶段切换，无目标隐藏。只用 XY 方向；目标位于镜头前方时沿目标方向额外前移最多 50 cm（不越过目的地），避免角色挡住；地面探测后抬高 60 cm。大小、半径、前移量和高度可调；上下车使用当前受控 Pawn。
- `/Game/UI/Minimap/WBP_Minimap` 继承 `UI/DeliveryMinimapWidget`，仍由 `WBP_Main` 引用。圆形裁切、深浅双边框、中心红色方向尖头由原生绘制完成。默认直径 320、右上角边距 24；拍摄宽度从 24000 cm 拉近到 18000 cm。地图朝向由**项目设置**里的 `UDeliveryMapSettings::MapNorthWorldYaw` 决定（项目设置 → 游戏 → Delivery 地图，0 = 世界 +X 朝北），标记按身体朝向旋转（步行取 `GetBodyForward()`，骑车取 Pawn 朝向），不跟随自由镜头。**小地图的朝上方向和罗盘的 N/E/S/W 读的是同一个值**——"这张地图的北在哪"是一个事实，原来抄在两处（小地图的 `MapUpWorldYaw` + 罗盘节点的 `NorthWorldYaw` 引脚），改一处忘另一处的表现是"小地图正的、罗盘偏 90°"，而两个数单看都像是对的。现在那两个字段都删了，`GetTrackedTaskCompass` 也**不再有 `NorthWorldYaw` 参数**（删参数会让 `WBP_Compass` 的节点少一个引脚，重编译一次即可）。放 `UDeveloperSettings` 的理由：不用每张图记得摆 Actor（漏摆会静默退回 0）；存 `Config/DefaultGame.ini` 是**文本**，而本项目被 `git pull` 的 autostash 冲掉过两次，二进制 `.uasset` 冲突只能整份取舍、文本能合并。代价是整个项目一份、不是每关一份。相机是 `UDeliveryMinimapWidget` 运行时 `NewObject` 出来的 `USceneCaptureComponent2D`、挂在 PlayerController 上，**关卡 Outliner 里找不到它**；旋转每帧重设，所以改设置立刻生效。**任务目的地也标在小地图上**：在拍摄范围内画橙色圆点（真实位置），超出就夹到圆边并换成**朝外的三角**——圆点停在边上看不出"目标还在更远处"，三角的尖端天然表达"继续往这边走"。描边用 `#020c1e`，和罗盘图钉、人物标记同一套。**比例尺是 `2*MapRadius / MapWorldWidth`**：`Disc()` 把整张 RT 的 UV 0~1 铺在**直径**上，而 RT 宽度就是 `OrthoWidth = MapWorldWidth`；用半径去除会小一半，表现是"目标点总比真实位置离中心近"，而且近得很均匀，看着像比例没调好、不像算错。目的地在 **Tick 里就换算成地图空间**（X=图上右、Y=图上上）存成 `TargetMapOffsetCm`，Paint 里只剩乘比例尺和夹圆——Paint 是 const 且每帧可能被调多次，不在那里查任务系统；用的 up/right 和相机 `FRotator(-90, N, 0)` 推出来的是同一组，地图转了标记跟着转。没有目标时（`GetTrackedTaskDestination` 返回 false）整个标记不画，和罗盘图钉、脚边箭头同一个判据。每名本机玩家有独立 512×512 RT 和摄像头，以 30Hz 跟随当前 Pawn；暂停旧蓝图写共享 `RT_Minimap` 的摄像头，控件销毁时恢复。绘制忽略 RT 的 Alpha，避免场景颜色被透明通道遮掉。尺寸、拍摄范围、刷新率、边框可在蓝图默认值调整。
- 新父类须先完整编译，再用 `Content/Python/upgrade_minimap.py` 切换小地图父类、编译保存，并验证主 HUD。无需更改关卡。

箭头可见性修正：逻辑组件留在 PlayerController，世界空间 Widget 的 Owner 必须是当前受控 Pawn。`OnlyOwnerSee` 按镜头 ViewActor 与 Owner 链判断，视觉挂 Controller 会被普通 Pawn 镜头过滤；上下摩托车时销毁旧视觉并以新 Pawn 为 Owner 重建。箭头原生 Widget 显式提供 128×128 绘制区域并启用动态重绘。


### 通话结束后自动收起
在用的 `/Game/UI/Task/WBP_TaskHUD.RefreshPhone` 在电话状态回到 `Idle` 后调用 `SetPhoneOpen(false)`，接通进入 `InCall` 时保持展开，整段通话结束（或手动挂断）才收起；不影响任务计时。`SetPhoneOpen` 统一调用 `DeliveryPlayerController.ReportPhoneUIOpen`，同步 `bPhoneUIOpen` 和 `OpenUIPanels`；已移除该函数内直接修改光标/输入模式的四个旧节点，统一由 C++ 按所有面板状态管理。因此来电自动展开、通话结束收起与 J 手动开关都同步，自动收起后下一次 J 直接打开。`Content/Python/phone_autoclose.py` 可幂等修补并编译保存该蓝图；原资产备份在 Saved，不提交。
