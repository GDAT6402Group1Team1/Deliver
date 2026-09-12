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

| 接口 | 说明 |
|---|---|
| `EnqueueCall(Task, CallType)` | 服务器：来电入队。同任务同类型不会重复入队 |
| `bool GetCurrentCall(FDeliveryPhoneCall& Out) const` | 队首（正在播的那通） |
| `int32 GetPendingCallCount() const` | 队列长度 |
| `static FDeliveryPhoneCallContent GetCallContent(Call)` | 取出这通电话的台词 / 语音 / 时长 |
| `OnCallStarted(const FDeliveryPhoneCall&)` | 队首换人时广播，UI 接这个响铃 |
| `OnQueueDrained()` | 队列播空，UI 接这个收线 |

队列推进由**服务器按每通电话配置的 `DurationSeconds` 驱动**，不是等客户端播完回报——
队列是所有人共享的，不能让某一个客户端的播放进度决定下一通什么时候响。

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

## 五、配置方式

### 5.1 建一个任务

新建 `UDeliveryTaskDefinition` 资产（Content Browser → Miscellaneous → Data Asset → DeliveryTaskDefinition），配置：

| 分组 | 字段 | 说明 |
|---|---|---|
| Task | `TaskId` | 稳定 ID，上线后不要改 |
| Task | `DisplayName` / `Description` | 手机里显示 |
| Unlock | `UnlockConditions` | 内联条件数组，全部满足才解锁。**留空 = 开局即解锁** |
| Phone | `UnlockCall` / `OverdueCall` | 台词、语音、这通电话占队列多久 |
| Time | `TimeLimitSeconds` | 总限时，例：300（5 分钟） |
| Time | `YellowRemainingSeconds` | 剩余少于它转黄，例：120 |
| Time | `RedRemainingSeconds` | 剩余少于它转红，例：60 |
| Reward | `BaseReward` | 基础奖励 |
| Reward | `TimeGrades` | 时间评价档位，**按 `WithinSeconds` 从小到大配**，第一条容得下用时的即评价结果 |
| Reward | `OvertimeMultiplier` | 所有档都超了（超时交付）时的倍率 |
| Reward | `SpecialEventRules` | 事件 Tag → 倍率，命中的可叠乘 |

最终奖励 = `BaseReward × 时间评价倍率 × ∏(命中的特殊事件倍率)`，四舍五入取整。

### 5.2 关卡接线

1. 建一个继承 `ADeliveryGameState` 的蓝图（比如 `BP_DeliverGameState`），
   在里面的 `TaskManager` 组件上把本关所有任务填进 `TaskDefinitions`。
   **数组顺序 = 同时解锁时的来电顺序。**
2. 在 `BP_DeliverGameMode` 里把 `GameStateClass` 指向这个蓝图。
3. 快递 Actor 上加 `DeliveryItemComponent`，填 `OwningTask`。
4. 收件人 Actor 上加 `DeliveryTargetComponent`，填 `ExpectedTask`。

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
| 地图引导 | 只暴露了 `GetTrackedTask()`，引导箭头/地图标记怎么画还没定 |
| 金钱结算 | `OnTaskCompleted` 里拿 `FinalReward` 自己加钱，钱包系统还没有 |
| 快递 Actor 本体 | 手持、掉落、放车后备箱由交互/背包系统实现，任务系统只认 `DeliveryItemComponent` |
| 存档 | 目前状态全在内存，重开关卡即重置 |
| 解锁条件 | 内置只有"前置任务已完成"一条，其他条件继承 `UDeliveryTaskUnlockCondition` 扩展 |
| **快递丢失的处理** | 快递掉出世界/被删之后，任务会永远卡在进行中，而且因为"同时只有一个进行中任务"，整局再也接不了别的任务。需要定：重生快递、加放弃接口、还是先不管 |
| 配置校验 | Definition 里黄/红阈值配反、`TimeGrades` 顺序配错都不会报错，可以加 `IsDataValid` 在编辑器里标红 |
| 倒计时文本格式化 | `05:00` / 超时的 `+00:07` 现在要在蓝图里自己拼 |
| 自动化测试 | 奖励结算是纯函数，最值得测，可复用互殴系统 `DeliveryBoxingPoseTests.cpp` 那套框架 |

---

## 九、我做的假设（需要确认）

功能说明里没写死、我按最合理的方式定的地方，验收时重点看这几条：

1. **取件后所有玩家的追踪都切到进行中任务**，不只是取件的那个人。理由：此时其他任务的快递也拿不起来，追踪别的没有意义。
2. **电话队列由服务器按配置时长推进**，所有人同时听到同一通电话；没有做"某个玩家挂断"的接口（挂断会影响所有人）。
3. **换手/掉落再捡不算重新接取，也不重置计时**；`NotifyAcquired` 此时返回 false（表示"没有发生接取"，不表示"拾取失败"）。
4. **同一个特殊事件 Tag 只计一次**，防止沿途反复触发把倍率叠爆。
5. **超时交付用单独的 `OvertimeMultiplier`**。规则只写了"时间评价档位"，没写超时怎么算。
6. **未解锁任务在数据层完全不暴露给 UI**（手机里不显示灰条）。
7. **没有放弃任务的接口**，和"不设失败状态"保持一致。
