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
- 交通路口、红绿灯（`trafficlight`）、道路与门的破碎效果等场景内容持续在搭建中（`Content/trafficlight`、`Content/PS2DEM` 下的 `BP_Intersection`、`BP_TrafficLine*` 等蓝图）。
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
└── Ragdoll/                         主动布娃娃、布娃娃战斗组件
```

蓝图层：`Content/Blueprint/Character/BP_DeliveryMan`、`Content/Blueprint/GameMode/BP_DeliverGameMode`、
`Content/Blueprint/PlayerController/BP_DeliveryManPC` 等，负责把 C++ 组件和具体资产（GE、GA 子类、动画）接起来。

## 约定与注意事项

- **每次新增或修改功能后，同步更新本文件（以及 [AGENTS.md](AGENTS.md)）里对应的模块说明**，不要让文档停留在旧状态。哪怕只是改了一个组件的职责或新增一个系统，也要在"目前做了什么"里补一句，保持文档和代码同步。
- 代码注释以中文为主，且偏"解释为什么"而不是"解释是什么"（尤其是 `FDeliveryArmPoseSettings` 和 Ragdoll 相关代码），修改这些参数前先理解注释里说明的耦合关系（例如 WindupUp/WindupOutward 必须和 PunchReach 按比例对齐，否则出拳轨迹会跑偏）。
- 二进制资源走 Git LFS；`Binaries/` `Intermediate/` `Saved/` `DerivedDataCache/` `.sln` `.vs/` 已被 `.gitignore` 忽略，不要强制添加或提交生成产物。
- GAS 的 AbilitySystemComponent 挂在 PlayerState 而不是 Character 上，涉及网络复制/GetAbilitySystemComponent 相关改动时注意这一点（`OnRep_PlayerState` 里做了处理）。
