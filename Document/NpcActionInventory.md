# NPC 动作库与行为库盘点（第一版）

2026-11-15 由 Claude 通过编辑器 MCP 读回 `/Game/NPC` 下全部 `AnimSequence`，并对照 `Design/AI/NpcBehaviorCatalog.json`。这一版只做了资产和执行入口两层的盘点，下面每一处"已接"都是依据资产命名和主文档的描述，还没有逐项打开动画蓝图、人物表 `BehaviorPresentation` 和行为树核对，也没有任何一项由我实机验证。行为树、子树、任务节点和 `AnimMontage` 尚未读回，放在文末"未盘点"。

## 数量

| 项 | 数量 | 说明 |
| --- | --- | --- |
| `/Game/NPC` 下 `AnimSequence` 资产 | 229 | 含源版、对齐版、候选版 |
| 独特片段（同一动画的源版、对齐版、重定向版、候选版算一组） | 约 126 | 正式片段 37 组，Quaternius 库一 46 组、库二 43 组；其中约 12 组正式片段本身就是库片段的重定向，按一组计后约 114 |
| 行为表执行入口 | 34 | `NpcBehaviorCatalog.json` version 1 |
| 已有正式触发的独特片段 | 约 37 | 即下表"正式片段"，依据命名推断 |
| 由我实机试玩通过 | 0 | |

## 行为表 34 个执行入口

| 分类 | 入口 | 这几轮试玩日志里实际出现过 |
| --- | --- | --- |
| 注意 | Wait、LookAt | 是 |
| 移动 | MoveTo、Pace、MoveAside | MoveAside 作为候选出现 |
| 姿态 | LieDown、GetUp | LieDown 作为候选出现 |
| 战斗 | StrikeOnce、Fight、Flee、JoinAssist、StopAssist | Fight、Flee 实际执行过 |
| 车辆 | HoldVehicle、ReleaseVehicle、SoundFollowHorn | 未见 |
| 同伴 | Follow、Stay、CancelCompanionTask、ContinueCurrent | Follow 实际执行过 |
| 配送 | FetchPackage、TakePackage、DeliverPackage、ReturnPackage、DropPackage、StorePackageInBackpack | TakePackage、ReturnPackage、StorePackageInBackpack 实际执行过 |
| 普通道具 | FetchItem、SearchNearOwner、ReturnItem、TakeItem | FetchItem、TakeItem、ReturnItem 被选中过；FetchItem 的拿取在 2026-11-15 两次修复前一直失败 |
| 毛毛虫 | OfferCaterpillar、CollectCaterpillar、InsertCaterpillar | InsertCaterpillar 被选中，拿取失败（同上原因） |
| 家居 | GoHome、HomeAction | HomeAction 在房屋解锁后才给候选 |

行为表里没有"表演"入口。跳舞、点头、摇头、招手、叉手、原地跳这些社交表演目前没有任何正式触发，是计划第 1 批要加的可中断表演入口。

## 正式片段（已在正式目录，按命名推断已接入）

| 类别 | 角色 | 片段 | 推断的触发入口 | 验证 |
| --- | --- | --- | --- | --- |
| 移动 | 居民 | A_ResidentIdle、A_ResidentWalk、A_NpcWalk、A_NpcJog | 动画蓝图移动状态；Pace、MoveAside 可经人物表覆盖 | 未验收 |
| 移动 | 居民 | Jump/A_ResidentJumpStart、Loop、Land | 遇障自动起跳 | 未验收 |
| 移动 | 二狗 | Jump/A_ErgouJumpStart、Loop、Land | 遇障自动起跳；2026-11-15 新增的"跟随受阻跳到主人身边"也会进入腾空状态 | 日志确认触发过，观感未验收 |
| 移动 | 二狗 | A_ErgouRide | 专属车骑乘 | 未验收 |
| 社交 | 居民 | A_ResidentTalk | 说话 | 未验收 |
| 社交 | 爷爷 | A_GrandpaIdle、IdleFoldArms、Look、Talk、Surprised、Startled、Walk、Receive | 爷爷状态机：敲门、收货、搬离；《汤喝完再搬》的两句催促台词用 Talk | 未验收 |
| 观察寻找 | 二狗 | A_ErgouSearchCrouchIdle | LookAt 低处道具、找物低身查看 | 未验收 |
| 物品 | 二狗 | A_ErgouGroundPickup、A_ErgouPickUp、A_ErgouCarry、A_ErgouAxeIdle | 地面拾取、家居拿取、搬运、持斧 | 未验收 |
| 物品 | 居民 | A_ResidentHold | HoldVehicle 拽车 | 未验收 |
| 家居 | 二狗 | A_ErgouSitDown、A_ErgouSit、A_ErgouStandUp、Home/A_ErgouJumpSeat、A_ErgouClimbSeat、A_ErgouLeaveSeat | HomeAction 的 Sit | 未验收 |
| 战斗 | 居民 | A_ResidentPunch、A_ResidentHit、A_NpcLightHit、A_ResidentDown、A_ResidentGetUp | StrikeOnce、Fight、受击、击倒、GetUp | 还手触发在日志确认过，观感未验收 |
| 剧情 | 面馆 | Encounter/A_NoodleOwnerEject、A_NoodleBossEject | 二狗初遇演出 | 未验收 |

候选目录 `Companion/Animation/Candidates` 下的 A_ErgouSearchCrouchWalk、A_ErgouSearchInspectKneeling、两份地面拾取候选和 CMU 预览片段是备选或源片段，没有正式入口。`Home/Retarget/Aligned/Candidates/...` 下五个片段与正式的 Carry、PickUp、Sit、SitDown、StandUp 同组。

## Quaternius 库片段（只导入，没有正式触发）

源版在 `Companion/Home/Source/Quaternius`，对齐版在 `.../QuaterniusAligned`，两份算一组。下面按计划批次归类。标"已有同类"的表示正式目录里已经有从它重定向出来的片段。

| 计划批次 | 片段 | 状态和缺项 |
| --- | --- | --- |
| 1 社交表演 | Dance_Loop、Yes、Idle_No_Loop、Idle_FoldArms_Loop、Idle_Talking_Loop、Idle_TalkingPhone_Loop、Zombie_Scratch | 缺：表演执行入口、表演 Gameplay Tag、人物表片段配置、到居民和二狗骨架的重定向。库里没有招手和犹豫的片段，要另找素材 |
| 2 移动与身体 | Walk_Loop、Walk_Formal_Loop、Jog_Fwd_Loop、Sprint_Loop、Crouch_Idle_Loop、Crouch_Fwd_Loop、Jump_Start/Loop/Land、NinjaJump_Start/Idle_Loop/Land、Roll、Roll_RM、Slide_Start/Loop/Exit、LayToIdle、Death01、Hit_Chest、Hit_Head、Hit_Knockback、Hit_Knockback_RM | 走、跑、跳、低身已有同类。缺：玩家要求"原地跳一下"的入口；翻滚、滑铲没有对应玩法；带 _RM 的片段开着 Root Motion，人物表覆盖会拒绝 |
| 3 实物与家居 | Interact、PickUp_Table、Walk_Carry_Loop、Idle_Torch_Loop、Idle_Lantern_Loop、Push_Loop、Consume、Chest_Open、Sitting_Enter/Idle_Loop/Talking_Loop/Exit、OverhandThrow | 坐、拿、搬已有同类。举物姿势、台面递交、喝汤是《汤喝完再搬》要用的，缺道具挂点和触物时刻标注 |
| 4 工作战斗剧情 | Punch_Jab、Punch_Cross、Punch_Enter、Melee_Hook、Melee_Hook_Rec、Fixing_Kneeling、Farm_Harvest、Farm_PlantSeed、Farm_Watering、TreeChopping_Loop、Driving_Loop、Idle_Rail_Loop、Idle_Rail_Call | 出拳已有同类。种地、修理、砍树没有对应的工作地点和道具 |
| 5 清库 | Swim_Idle_Loop、Swim_Fwd_Loop、ClimbUp_1m_RM、Pistol_*（6 个）、Sword_*（库一 3 个、库二 8 个）、Shield_*（2 个）、Idle_Shield_Loop、Idle_Shield_Break、Spell_Simple_*（4 个）、Zombie_Idle_Loop、Zombie_Walk_Fwd_Loop | 游戏里没有游泳、攀爬、枪、剑、盾、法术和丧尸玩法，全部缺前置功能 |
| 参考 | A_TPose（两库各一） | 重定向姿势参考，不接入 |

## 《汤喝完再搬》素材

`Design/Art/NPC/Grandpa/MoveOutScene` 下的锤子、汤碗、空碗、木凳和三份动画源文件还没有导入 UE。来源和许可见同目录 `Sources.md`。当前游戏里用斧头代替锤子，爷爷只有台词和 A_GrandpaTalk。

## 未盘点

- 行为树、子树和任务节点（只知道 `BT_SearchNearbyItem` 和 `UBTTask_DeliveryExecuteNpcAction`、`UBTTask_DeliveryGrandpaMoveOut` 这几个名字）
- `AnimMontage`、混合空间和动画蓝图里各状态实际引用的片段
- 人物表每个角色的 `BehaviorPresentation` 覆盖
- `/Game/NPC` 以外的动画（玩家、摩托车骑手、行人）
- 每个入口的前置条件、中断清理和远端表现
