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
- UI、背包/交互、金钱结算、存档都还没做，对接点见文档第七节；文档第八节列了我在实现时替规则做的假设，需要确认。

### 5. 地图 / 交通场景
- 已导入地图并接入 **PS2DEMImporter**（`Plugins/PS2DEMImporter`，PS2 地形/道路生成插件）用于把地形转换为 landscape spline 道路。
- 交通路口、红绿灯、道路与门的破碎效果正在搭建：`Content/PS2DEM`（`BP_Intersection`、`BP_TrafficLine*`）、`Content/trafficlight`。
- 测试地图：`Content/Level/TestForCharacter.umap`（角色/互殴）、`Content/Level/testfortraffic.umap`（交通路口）。

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
└── Task/                             任务系统：配置资产、解锁条件、全局管理器、电话队列、追踪、快递/收件人组件

Plugins/PS2DEMImporter/               地形转 landscape spline 道路插件
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
