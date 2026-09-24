# 任务系统（电话接任务）结构与接口

对应功能说明里的十条规则。本文档描述**已经写出来的 C++ 结构和对外接口**，
以及还没做、需要别的系统来接的部分。UI、背包、交互、金钱都不在本次范围内。

---

## 一、设计要点

三件事决定了整套结构的样子：

**1. 任务状态是全局的，追踪是各人各自的。**
多人模式下解锁、来电、接取、计时、完成对所有玩家是同一份数据，所以这些状态挂在
**GameState** 上（`UDeliveryTaskManagerComponent`）。只有"当前追踪哪个任务"是每个玩家自己选的，
挂在 **PlayerState** 上（`UDeliveryTaskTrackerComponent`）。

**2. 计时不用 Tick，用时间戳。**
取件那一刻记下服务器时间，之后谁都不再碰它，倒计时是"当前服务器时间 − 起始时间戳"算出来的。
这样快递掉落、被别人抢走、塞进车后备箱、持有者晕倒，**计时根本碰不到**，
不需要为每种情况写一遍"计时不停"的逻辑，也不会有客户端各算各的问题。
超时后这个差值继续变大，UI 直接把负的剩余时间显示成 `+00:01`。

**3.「同时只有一个进行中任务」不是一个状态，是一条取件规则。**
它落在 `CanAcquireItem()` 里：只要世界上存在进行中的任务，其他任务的快递就拿不起来。
所以不需要额外的互斥标记，也不可能出现两个任务同时计时。

---

## 二、状态机

```
                解锁条件满足                有人取到快递              交付完成
   Locked ──────────────────► AwaitingPickup ──────────► InProgress ──────────► Completed
（未解锁）      ＋解锁来电入队   （待取件）      ＋开始全局计时  （进行中）    ＋结算奖励
                                不计时、不过期                 ＋超时定时器    ＋重新评估解锁
```

- **没有失败态**。超时只影响奖励倍率，任务继续，颜色保持红色，倒计时转正计时。
- 未取件的任务可以永远挂着，不计时也不消失。
- 首次超时打一次催促电话（`bOverdueCallPlayed` 置位），同一任务不会打第二次。

---

## 三、文件与职责

所有新文件在 `Source/Delivery/Task/`，另有 GameState 在 `Source/Delivery/`。

| 文件 | 类 | 挂在哪 | 职责 |
|---|---|---|---|
| `Task/DeliveryTaskTypes.h` | 枚举与结构体 | — | 状态枚举、计时快照、奖励明细、电话项等纯数据 |
| `Task/DeliveryTaskDefinition.h/.cpp` | `UDeliveryTaskDefinition` | 资产 | 一个任务的静态配置（一个任务一份资产） |
| `Task/DeliveryTaskUnlockCondition.h/.cpp` | `UDeliveryTaskUnlockCondition` | 内联在资产里 | 解锁条件基类，可继承 C++/蓝图扩展 |
| `Task/DeliveryTaskManagerComponent.h/.cpp` | `UDeliveryTaskManagerComponent` | **GameState** | 全局任务状态唯一权威：解锁、接取、计时、完成、奖励结算 |
| `Task/DeliveryPhoneCallQueueComponent.h/.cpp` | `UDeliveryPhoneCallQueueComponent` | **GameState** | 全局电话队列，解锁来电和催促来电按序播放 |
| `Task/DeliveryTaskTrackerComponent.h/.cpp` | `UDeliveryTaskTrackerComponent` | **PlayerState** | 每个玩家自己的当前追踪任务 |
| `Task/DeliveryItemComponent.h/.cpp` | `UDeliveryItemComponent` | 快递 Actor | 声明这件快递属于哪个任务；回答能不能捡、捡了要不要接取 |
| `Task/DeliveryTargetComponent.h/.cpp` | `UDeliveryTargetComponent` | 收件人 Actor | 声明这里收哪个任务；执行交付判定与完成 |
| `DeliveryGameState.h/.cpp` | `ADeliveryGameState` | — | 承载上面两个全局组件 |

改动到的既有文件：
- `DeliveryGameMode.cpp`：`GameStateClass = ADeliveryGameState::StaticClass()`
- `GAS/DeliverPlayerState.h/.cpp`：创建 `TaskTracker` 子组件

---

## 四、接口清单

### 4.1 `UDeliveryTaskManagerComponent`（全局权威，GameState 上）

拿到它：`UDeliveryTaskManagerComponent::Get(WorldContextObject)`（蓝图可调）。

**服务器接口**（客户端调用直接返回 false，不会有副作用）：

| 接口 | 说明 |
|---|---|
| `bool TryAcquireItem(Task, APlayerState* Player)` | 玩家拿到快递。首次取件才会接取任务并开始计时，返回 true；换手/捡回返回 false |
| `bool TryCompleteDelivery(Task, APlayerState* Deliverer)` | 完成交付。结算奖励、结束任务、重新评估后续任务解锁 |
| `void ReportSpecialEvent(Task, FGameplayTag EventTag)` | 上报影响奖励的特殊事件。同一 Tag 只记一次 |
| `void ReevaluateUnlocks()` | 重新评估所有未解锁任务。完成任务后自动调用；外部条件变化时需手动调 |

**双端查询接口**：

| 接口 | 说明 |
|---|---|
| `bool CanAcquireItem(Task) const` | 这件快递现在能不能被拿起来（交互系统在拾取前问这句） |
| `EDeliveryTaskStatus GetTaskStatus(Task) const` | 任务状态 |
| `UDeliveryTaskDefinition* GetActiveTask() const` | 当前进行中的任务，全世界最多一个 |
| `void GetTasksByStatus(Status, TArray<...>& Out) const` | 按状态取任务列表（手机列表用 `AwaitingPickup`） |
| `FDeliveryTaskTimeSnapshot GetTimeSnapshot(Task) const` | 倒计时快照：已用时、剩余（超时为负）、是否超时、绿/黄/红 |
| `FDeliveryRewardBreakdown EvaluateReward(Task, Elapsed, Events) const` | 纯计算，结算和预览都走它 |
| `FDeliveryRewardBreakdown PreviewReward(Task) const` | 按当前用时预览"现在送到能拿多少" |

**事件**（服务器直接广播，客户端经复制差分后广播）：

| 委托 | 参数 |
|---|---|
| `OnTaskStatusChanged` | `(Task, NewStatus)` |
| `OnTaskCompleted` | `(Task, const FDeliveryRewardBreakdown&, APlayerState* Deliverer)` |
| `OnTaskOverdue` | `(Task)` 首次超时 |

三者语义不同，用法见第七节。

### 4.2 `UDeliveryPhoneCallQueueComponent`（全局电话队列，GameState 上）

状态机：

```
Idle ──(有电话入队)──► Ringing ──(有人接听)──► InCall ──(播完 / 挂断)──┐
                         └──(响铃超时没人接，记未接)──────────────────┤
                                                                     ▼
                                                    出队 → 下一通 Ringing，没有则 Idle
```

| 接口 | 说明 |
|---|---|
| `EnqueueCall(Task, CallType)` | 服务器：来电入队。同任务同类型不会重复入队 |
| `AnswerCurrentCall()` | 服务器：接听，只在 Ringing 时有效 |
| `HangUpCurrentCall()` | 服务器：挂断。Ringing 时算拒接（记未接），InCall 时算提前结束 |
| `EDeliveryPhoneCallState GetCallState()` | `Idle` / `Ringing` / `InCall`，UI 靠它切界面 |
| `bool GetCurrentCall(FDeliveryPhoneCall& Out)` | 当前这通电话，Idle 时返回 false |
| `float GetStateRemainingSeconds()` | 当前阶段剩余秒数：响铃时是还能接多久，通话时是台词还有多久播完 |
| `int32 GetPendingCallCount()` | 队列长度 |
| `static GetCallContent(Call)` | 取出台词 / 语音 / 时长 |
| `OnPhoneStateChanged(State, Call)` | 状态变化。三个界面的切换接这一个就够 |
| `OnCallMissed(Call)` | 响铃超时没人接，UI 可显示"未接来电" |

**客户端不要直接调 `AnswerCurrentCall`**。电话队列挂在 GameState 上，GameState 不属于任何客户端，
客户端对它发 RPC 是无效的。玩家的接听/挂断意图走 PlayerController 中转：

| PlayerController 接口 | 说明 |
|---|---|
| `RequestAnswerCall()` | 客户端调用自动转 Server RPC |
| `RequestHangUpCall()` | 同上 |

**所有计时都在服务器**（响铃时长 `RingDurationSeconds`、通话时长 `DurationSeconds`），
不等客户端播完回报——否则某一个人的播放进度就决定了所有人什么时候进下一通。
客户端只负责把当前状态表现出来。

**接听是全局的**：任意一个玩家接听，所有人一起进入通话；挂断同理。
这和"任意玩家取件则全体任务进入进行中"是同一套逻辑。

### 4.3 `UDeliveryTaskTrackerComponent`（每玩家，PlayerState 上）

| 接口 | 说明 |
|---|---|
| `static FindTracker(AActor*)` | 从 Pawn / Controller / PlayerState 任意一个拿到 |
| `UDeliveryTaskDefinition* GetTrackedTask() const` | 当前追踪任务（只有它显示地图引导） |
| `void RequestTrackTask(Task)` | UI 选择追踪任务，客户端调用自动转 Server RPC |
| `bool CanTrackTask(Task) const` | 该任务当前能否被选中 |
| `OnTrackedTaskChanged(Task)` | 追踪目标变化 |

自动选择规则（服务器统一处理，避免两端各算各的）：
1. 存在进行中任务 → 所有玩家的追踪都切到它（此时别的任务的快递也拿不起来）
2. 原追踪任务已完成/失效 → 清空
3. 没有追踪目标且只剩一个待取件任务 → 默认选中它

### 4.4 `UDeliveryItemComponent`（快递 Actor 上）

| 接口 | 说明 |
|---|---|
| `OwningTask` | 配置：这件快递属于哪个任务 |
| `bool CanBeAcquired() const` | 交互系统在允许拾取前问这句 |
| `bool NotifyAcquired(APlayerState* Player)` | 服务器：玩家拿到了它。返回是否因此接取了任务（仅首次 true） |

本组件**不管**拿在手里、掉在地上、塞进后备箱——那是交互/背包系统的事，
而且这些都不影响任务状态和计时。

### 4.5 `UDeliveryTargetComponent`（收件人 Actor 上）

| 接口 | 说明 |
|---|---|
| `ExpectedTask` / `DeliveryRadius` | 配置：收哪个任务、判定距离 |
| `bool CanAcceptDelivery(ItemActor, Player) const` | UI 用它决定要不要显示交付提示 |
| `bool TryDeliver(ItemActor, Player)` | 服务器：完成交付，成功后销毁快递 Actor |

---

### 4.6 `UDeliveryWalletComponent`（每玩家，PlayerState 上）

挂在 `ADeliverPlayerState` 上，和 ASC 同理——钱要在角色死亡/重生/上下载具之后还在，
而 Character 在这些事件里会被销毁重建。

| 成员 | 说明 |
|---|---|
| `FindWallet(Actor)` | 静态。从 Pawn / Controller / PlayerState 任意一个找到钱包 |
| `GetBalance()` | 余额 |
| `AddBalance(Amount)` | 加钱，负数为扣款。**只在服务器有效**，余额夹到 0 以上 |
| `TrySpend(Amount)` | 余额不足时不扣并返回 false |
| `OnBalanceChanged(NewBalance, Delta)` | 余额变化 |
| `OnTaskPaid(Task, Reward)` | 送达入账，带完整奖励明细，UI 做结算飘字直接接这个 |

**入账是自动的，任务系统那边一行都不用改。** 组件自己订阅 GameState 上的
`OnTaskCompleted`，那个委托自带 `APlayerState* Deliverer`，多人抢单时钱记给谁天然就对。
`Balance` 用 `COND_OwnerOnly` 复制——别人的余额不该出现在你的客户端上。

不做客户端预测：钱预测错了要回滚，而送达本来就要等服务器确认，没有延迟收益。

**PlayerState 可能比 GameState 先 BeginPlay**，这时订阅不到任务管理器。组件会每 0.5 秒
重试一次直到成功——不重试的话这个玩家整局收不到钱，而且完全没有报错。

服务器上 `OnRep` 不触发，所以 `AddBalance` 里手动广播一次 `OnBalanceChanged`；
漏了的话就是"只有客户端有反应"那类很难查的 bug。

### 4.7 `UDeliveryLocationRegistry` + `UDeliveryLocationComponent`（地点 ID 解析）

把任务定义里的地点编号解析成关卡里的实际位置。没有这一层的话，"这个任务要送到哪"
在代码里是答不出来的，地图指引、方向箭头、距离提示全都无从做起。

- **`UDeliveryLocationComponent`**（SceneComponent）：挂在取件点/收件点/收件人 NPC 上，
  填 `LocationId`，`BeginPlay` 时自己注册。三种点共用一个组件，因为在数据上它们是同一种
  东西：一个需要被解析成世界坐标的外部编号。继承 SceneComponent 是为了能带相对偏移——
  收件点挂在整栋楼上时，楼的原点可能在中心甚至地下，指引箭头该指门口。
- **`UDeliveryLocationRegistry`**（WorldSubsystem）：`ResolveActor` / `ResolveLocation` /
  `ResolveAllActors` / `GetRegisteredIds`。

**查找走注册表而不是遍历关卡**，和交互系统同一个理由：这个项目已经被"碰撞查询静默失效"
坑过一次（交通组件的前车探测通道配错、恒为 false，肉眼完全看不出来）。注册表没有这个
失败模式——没注册就是查不到。

追踪组件上新增了两个取目的地的接口：

| 成员 | 说明 |
|---|---|
| `GetTrackedTaskDestination(OutLocation, OutLocationId)` | 当前追踪任务该去哪 |
| `GetTaskDestination(Task, ...)` | 指定任务的目的地，任务列表显示距离时用 |

**目的地按任务状态自动切换**，调用方不要自己判：待取件 → `PickupLocationId`，
进行中 → `DeliveryLocationId`，其他状态返回 false。判错了的表现是"箭头指向已经拿过的
地方"，而且四个界面会各错各的。

策划表里填了 ID、关卡里却没有对应的点时返回 false **并在日志里点名**。静默失败的话
表现只是"箭头不显示"，根因几乎查不到。

### 4.8 快递丢失的恢复

`UDeliveryTaskManagerComponent::NotifyItemLost(Task)` —— 把进行中的任务退回待取件。

不处理的话任务会永远卡在进行中，而且因为"同时只有一个进行中任务"是取件规则，
**整局再也接不了任何别的任务**，一次意外就把这局玩废了。

三个决定和理由：

- **退回而不是判失败** —— 和"四个状态无失败态"一致。退回不是惩罚，是重来一次。
- **不在取件点重新生成一份** —— 那需要把 `DeliveryItemId` 解析成可生成的类，那一步还没做。
  退回之后关卡里原本那份快递还在原地（手摆的话），玩家按指引回取件点就能重新拿。
  如果快递是运行时生成的，生成方要自己在退回后再生成一份。
- **计时重置、特殊事件清空** —— 退回等于这次取件没发生过。保留旧计时的话，玩家要为一次
  不是自己造成的意外承担时间损失；事件不清的话重新取件后再触发一次会把倍率叠上去。

检测在 `UDeliveryItemComponent::EndPlay`：Actor 被销毁（`EEndPlayReason::Destroyed`，
掉出世界会走到这条）且任务仍在进行中时上报。关卡切换/退出游戏不触发——那时整张图都在拆，
改任务状态没有意义，GameState 还可能已经先没了。

**"正常交付"和"意外丢失"靠执行顺序区分，不需要额外的标志位**：`TryDeliver` 是先把任务
标成已完成、再销毁快递的，所以走到 `EndPlay` 时状态已经不是进行中，`NotifyItemLost`
会自己返回 false。**改动这两处的先后顺序会让每次成功交付都被误判成丢件。**

组件上有 `bReportLostOnDestroy` 开关（默认开），给"用完就换一份"那类流程关掉用。

### 4.9 `UDeliveryGuidanceLibrary`（指引换算）

把"该去哪"换算成 UI 直接能用的量，返回 `FDeliveryGuidance`：

| 字段 | 说明 |
|---|---|
| `bValid` | 有没有可指引的目的地。false 时其余字段不要用 |
| `WorldLocation` / `LocationId` | 目的地坐标和解析用的 ID |
| `Distance` | 直线距离（厘米） |
| `RelativeYaw` | 相对镜头的水平夹角，−180~180。0 正前、正数在右、负数在左 |
| `bInFront` | 在不在镜头前方。决定箭头画屏幕边缘还是目标上方 |
| `HeightOffset` | 高度差，正数说明在上方，可以提示"在楼上" |

三个入口：`GetTrackedTaskGuidance`、`GetTaskGuidance(Task)`、`MakeGuidanceToLocation`。
另有 `FormatDistance` 把厘米格式化成"12 米" / "1.5 公里"。

**角度基于镜头而不是角色**：玩家看的是镜头，而骑车时镜头和车头还可能不一致
（摩托车有自由视角/固定视角两套）。用角色朝向算的话，自由视角下箭头会指错。
夹角只算水平面——带上 Z 的话目标在正下方时角度会乱跳，而边缘箭头本来只需要"往左还是往右"。

### 4.10 奖励结算的单元测试

`Task/DeliveryRewardTests.cpp`。跑法：编辑器 → Tools → Session Frontend → Automation →
筛 `Delivery.Task`，或控制台 `Automation RunTests Delivery.Task`。

`EvaluateReward` 是纯函数——只读任务资产，不碰组件状态、网络和世界，所以能在不开关卡的
情况下把边界跑全。它同时是整个任务系统里**最容易配错又最难在游戏里发现**的一块：
档位顺序配反、超时边界差一秒、特殊事件重复计数，在 PIE 里都只表现为"钱好像不太对"。

覆盖：档位按配置顺序命中第一条、阈值是闭区间、超过最后一档不再恶化、`bOverdue` 的判据是
用时超过时限（**和命中哪一档是两回事**，这两个概念混过一次）、特殊事件相乘且重复只计一次、
没配档位时按底薪原样发、`FinalReward` 是四舍五入不是截断。

测试里的 Tag 借用已注册的原生 Tag。用 `RequestGameplayTag` 现造的话，没注册的会返回空 Tag，
而**两个空 Tag 互相相等，测试会假通过**。

---

## 五、配置方式

### 5.1 建一个任务

新建 `UDeliveryTaskDefinition` 资产（Content Browser → Miscellaneous → Data Asset → DeliveryTaskDefinition），配置：

| 分组 | 字段 | 说明 |
|---|---|---|
| Task | `TaskId` | 稳定 ID，上线后不要改 |
| Task | `DisplayName` | 任务名 |
| Task | `SimpleDescription` / `DetailedDescription` | 列表里的一句话 / 详情页的完整描述 |
| Reference | `DeliveryItemId` / `PickupLocationId` / `DeliveryLocationId` / `ReceiverNpcId` / `SpecialEventId` | 策划表里的外部引用 ID。**目前只存字符串不做解析**，指向的系统还没做（见第八节） |
| Unlock | `UnlockConditions` | 内联条件数组，全部满足才解锁。**留空 = 开局即解锁** |
| Phone | `UnlockCall` / `OverdueCall` | 台词、语音、这通电话占队列多久 |
| Time | `TimeLimitSeconds` | 总限时，例：300（5 分钟） |
| Time | `YellowRemainingSeconds` | 剩余少于它转黄，例：120。**纯 UI 配色，无机制后果** |
| Time | `RedRemainingSeconds` | 剩余少于它转红，例：60。同上 |
| Reward | `BaseReward` | 基础奖励 |
| Reward | `TimeGrades` | 时间评价档位，见下 |
| Reward | `SpecialEventRules` | 事件 Tag → 倍率，命中的可叠乘 |

最终奖励 = `BaseReward × 时间评价倍率 × ∏(命中的特殊事件倍率)`，四舍五入取整。

**时间评价档位用「交付时的剩余秒数」表达，正数是提前、负数是超时，按从大到小配置**，
和策划表 Time Rating 那列（`150,1.2|60,1.1|0,1|-60,0.9|-120,0.8`）一一对应。
结算时取第一条"剩余时间不低于门槛"的档；比最后一档还差就按最后一档算。

这样写有两个好处：填表时能直接抄、不用拿限时去心算；超时档位天然就是负数，
不需要再单独配一个"超时倍率"（早期版本有个 `OvertimeMultiplier`，已经删掉了）。

### 5.2 关卡接线

1. 建一个继承 `ADeliveryGameState` 的蓝图（比如 `BP_DeliverGameState`），
   在里面的 `TaskManager` 组件上把本关所有任务填进 `TaskDefinitions`。
   **数组顺序 = 同时解锁时的来电顺序。**
2. 在 `BP_DeliverGameMode` 里把 `GameStateClass` 指向这个蓝图。
3. 快递 Actor 上加 `DeliveryItemComponent`，填 `OwningTask`。
4. 收件人 Actor 上加 `DeliveryTargetComponent`，填 `ExpectedTask`。

### 5.3 从策划表导入

任务数值由策划在表格里维护，导出成 `Design/Tasks.csv`（13 列，列顺序见文件本身），
然后在编辑器菜单 **Delivery → Import / Reimport Tasks** 一键同步成 DataAsset。
脚本在 `Content/Python/delivery_task_import.py`。

几条关键行为：

- **按 `TaskId` 增量更新**：已存在的资产只改表里有的字段，不重建。所以来电语音、
  黄红阈值、特殊事件倍率这些表里没有的列，在资产上手填之后不会被导入冲掉。
- **表里删掉的任务不会被自动删除**，只在日志里提示，避免误删。
- **按列名匹配而不是列位置**，策划调整列顺序不会串位。
- 导入后**不会自动加进 GameState 的 `TaskDefinitions`**，那一步仍要手动做。

解锁条件那一列在表里原本是自然语言（"游戏开始15秒后"），没法可靠解析，约定改成：

| 写法 | 含义 |
|---|---|
| 留空 | 开局立刻解锁 |
| `time:15` | 关卡开始 15 秒后 |
| `task:Task_001` | 前置任务完成（整张表导完后统一回填，所以可以引用表里靠后的任务） |
| `time:15\|task:Task_001` | 用 `\|` 连接，全部满足才解锁 |

解析不了的内容会告警并当成"无条件"，不会静默生成一个错的条件。

### 5.4 不用 UI 也能测：控制台命令

交互和背包系统还没有，所以加了四条控制台命令，在 PIE 里按 `` ` `` 敲。
实现在 `Task/DeliveryTaskDebugCommands.cpp`，用 `#if !UE_BUILD_SHIPPING` 包着，不进正式包。

| 命令 | 作用 |
|---|---|
| `Delivery.Task.Dump` | 打印所有任务的状态、倒计时、奖励预览、当前追踪目标、队首来电 |
| `Delivery.Task.Acquire [TaskId]` | 模拟取件。不填 TaskId 就取第一个待取件的 |
| `Delivery.Task.Deliver` | 模拟交付当前进行中的任务，打印奖励算式 |
| `Delivery.Task.Event <Tag>` | 给进行中的任务上报特殊事件，验证奖励倍率 |
| `Delivery.Task.Validate` | **配置对账**：地点 ID 能不能解析、收件点和快递有没有接上。摆点时用 |
| `Delivery.Task.LoseItem` | 模拟快递丢失，任务退回待取件 |
| `Delivery.Wallet.Dump` / `Delivery.Wallet.Add <金额>` | 看余额 / 加钱扣款（只在服务器有效） |
| `Delivery.Locations.Dump` | 列出关卡注册了哪些地点 ID，并验证当前追踪任务能否解析出目的地 |

都是权威操作，要用 Standalone 或 Play As Listen Server 跑。
双人测试时在服务器窗口 Acquire、客户端窗口 Dump，可以验证复制和客户端计时基准。

---

## 六、一次完整流程的调用链

```
关卡开始
  └ TaskManager::BeginPlay → ReevaluateUnlocks()
      └ 条件满足的任务 → AwaitingPickup → PhoneQueue::EnqueueCall(解锁来电)
          └ 客户端 OnCallStarted → UI 响铃播台词

玩家走到快递前
  └ 交互系统 → ItemComponent::CanBeAcquired()   （有别的任务在进行中就会被拦住）
      └ 拾取成功 → ItemComponent::NotifyAcquired(PS)
          └ TaskManager::TryAcquireItem() → InProgress，记 StartServerTime，挂超时定时器
              └ 所有玩家 Tracker 自动切到该任务；UI 靠 GetTimeSnapshot 每帧刷倒计时

（途中：掉落、换手、进后备箱、晕倒 —— 任务系统完全不参与，计时照走）
（途中：恶作剧等事件 → TaskManager::ReportSpecialEvent(Task, Tag)）

限时到点
  └ TaskManager::HandleOverdue → 催促来电入队 + OnTaskOverdue；任务继续，UI 转正计时

玩家到收件人处交互
  └ TargetComponent::TryDeliver(ItemActor, PS)
      └ TaskManager::TryCompleteDelivery() → 结算奖励 → Completed → ReevaluateUnlocks()
          └ 销毁快递 Actor；OnTaskCompleted 带 FinalReward 和 Deliverer 广播给两端
              └ 后续任务若解锁 → 立刻进电话队列
```

---

## 七、UI 对接注意

**初始化时要先主动查一次当前状态，不能只依赖委托。** 委托只在"发生变化的那一刻"广播，
而解锁来电、任务解锁都可能发生在 UI 控件创建之前（关卡一开始就会评估解锁并把来电入队）。
所以手机界面打开/创建时应当：

- `GetTasksByStatus(AwaitingPickup, Out)` 拉一次任务列表，再订阅 `OnTaskStatusChanged`
- `GetCurrentCall(OutCall)` 看看现在是不是正有一通电话在响，再订阅 `OnCallStarted`
- `GetTrackedTask()` 取当前追踪目标，再订阅 `OnTrackedTaskChanged`

倒计时不要监听事件，每帧（或每 0.1 秒）调 `GetTimeSnapshot(Task)` 取 `RemainingSeconds` 和 `Urgency` 刷新即可。

**三个任务委托的语义不一样，别混用：**

| 委托 | 语义 | 中途加入的玩家 |
|---|---|---|
| `OnTaskStatusChanged` | "刷新你的视图"。可能重复收到，处理函数要写成幂等的（重建列表而不是累加） | 首次同步时会收到全部任务的当前状态 |
| `OnTaskCompleted` | 一次性表现事件：结算弹窗、音效 | **不补播**别人早就做完的任务 |
| `OnTaskOverdue` | 一次性表现事件：超时提示 | **不补播** |

完成弹窗接 `OnTaskCompleted`，列表刷新接 `OnTaskStatusChanged`。
如果拿 `OnTaskStatusChanged` 里的 `Completed` 状态去弹结算框，中途进来的玩家一进场会看到一串弹窗。

---

## 八、还没做的部分（等其他系统）

| 缺口 | 说明 |
|---|---|
| UI / 手机界面 | 所有数据和事件都已暴露，直接接委托即可，不需要改 C++ |
| 地图引导 | 目的地坐标和方向/距离换算都有了（4.7 / 4.9），**箭头和地图标记的美术表现还没做** |
| ~~金钱结算~~ | **已做**：`UDeliveryWalletComponent` 挂 PlayerState，自己订阅 `OnTaskCompleted` 入账，见 4.6 |
| 快递 Actor 本体 | 手持、掉落、放车后备箱由交互/背包系统实现，任务系统只认 `DeliveryItemComponent` |
| 存档 | 目前状态全在内存，重开关卡即重置 |
| 解锁条件 | 内置只有"前置任务已完成"一条，其他条件继承 `UDeliveryTaskUnlockCondition` 扩展 |
| 外部 ID 的解析 | **地点类已做**：`PickupLocationId` / `DeliveryLocationId` / `ReceiverNpcId` 由 `UDeliveryLocationRegistry` 解析，见 4.7。`DeliveryItemId` / `SpecialEventId` 仍只存字符串 |
| ~~快递丢失的处理~~ | **已做**：`NotifyItemLost` 把任务退回待取件、计时重置；`UDeliveryItemComponent` 在 Actor 被销毁时自动上报。见 4.8 |
| 自动化测试 | 奖励结算是纯函数，最值得测，可复用互殴系统 `DeliveryBoxingPoseTests.cpp` 那套框架 |

---

## 九、我做的假设（需要确认）

功能说明里没写死、我按最合理的方式定的地方，验收时重点看这几条：

1. **取件后所有玩家的追踪都切到进行中任务**，不只是取件的那个人。理由：此时其他任务的快递也拿不起来，追踪别的没有意义。
2. ~~没有做挂断接口~~ —— **已实现接听/挂断**：任意玩家接听即全局接通，挂断同理；响铃超时记为未接来电，自动进下一通。所有计时仍在服务器。
3. **换手/掉落再捡不算重新接取，也不重置计时**；`NotifyAcquired` 此时返回 false（表示"没有发生接取"，不表示"拾取失败"）。
4. **同一个特殊事件 Tag 只计一次**，防止沿途反复触发把倍率叠爆。
5. ~~超时交付用单独的 `OvertimeMultiplier`~~ —— **已确认**：策划表里超时是分档的（负数档位），已改成剩余时间语义，`OvertimeMultiplier` 删除。
6. **未解锁任务在数据层完全不暴露给 UI**（手机里不显示灰条）。
7. **没有放弃任务的接口**，和"不设失败状态"保持一致。
