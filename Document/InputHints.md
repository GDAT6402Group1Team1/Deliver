# 输入与操作提示

按键配置放在 `/Game/Input/IMC_Default`，动作资源放在 `/Game/Input/Actions`。IA 表示玩家要做的操作，IMC 决定用哪个键触发。改键时修改映射，不再修改角色里的 FKey 字段

## 类和资源

| 类 | 职责 |
|---|---|
| DeliveryPlayerController | 添加输入映射，绑定手机动作，将键盘或手柄的变化告诉提示组件 |
| DeliveryInputComponent | 绑定步行、攻击、交互、召唤和物品栏动作，判断单键出拳与双键抓取 |
| DeliveryMotorbike | 绑定驾驶输入和切换视角动作，切换后发布 CameraChanged |
| DeliveryVehicleSummonComponent | 向服务器请求召唤，检查车辆和落点，记录召唤冷却结束时间 |
| DeliveryHintComponent | 监听控制对象、视角、召唤冷却和输入映射变化，决定左下角显示什么 |
| DeliveryPromptSubsystem | 保存提示内容并交给 Slate 显示，按来源清除左下角提示 |

源码分别位于 `Source/Delivery/Player`、`Input`、`Vehicle`、`UI` 和 `Interaction` 目录，类名与文件名一致

| 动作资源 | 键盘 | 手柄 |
|---|---|---|
| IA_SummonVehicle | R | 左摇杆按下 |
| IA_ToggleCamera | P | 右摇杆按下 |
| IA_TogglePhone | J | 方向键上 |
| IA_InventorySlot1 | 1，丢弃手中物品 | 方向键下 |
| IA_InventorySlot2 | 2，交换第一个背包槽 | 方向键左 |
| IA_InventorySlot3 | 3，交换第二个背包槽 | 方向键右 |
| IA_InventorySlot4 | 4，交换第三个背包槽 | 左肩键 |
| IA_InventorySlot5 | 5，交换第四个背包槽 | 右肩键 |

移动、跳跃、攻击和交互继续使用原有 IA 和映射。输入组件里的动作引用、车辆里的 CameraToggleAction、控制器里的 TogglePhoneAction 必须与 IMC 中的动作一致

## 从按下 R 到显示冷却

R 经 IMC_Default 找到 IA_SummonVehicle，输入组件在 Bind 中将动作的 Started 事件绑定到 SummonVehiclePressed。这个函数仍然检查角色能否行动，然后调用召唤组件，不负责生成车辆

召唤组件把请求送到服务器。服务器检查冷却、可用车辆和落点，成功后更新 ReadyServerTime。找不到落点或无法召唤时，不消耗冷却

```cpp
// 来源：Source/Delivery/Vehicle/DeliveryVehicleSummonComponent.cpp
ReadyServerTime = Now + Cooldown;
// 房主不经过属性复制回调，因此服务器修改后也要发送通知
OnCooldownChanged.Broadcast();
Owner->ForceNetUpdate();
```

Cooldown 是等待时长，默认 10 秒；ReadyServerTime 是下一次允许召唤的服务器时间。结束时间只复制给操作玩家，客户端在 OnRep_ReadyServerTime 中广播 OnCooldownChanged。房主直接收到服务器上的广播

提示组件订阅这个通知后调用 RefreshHint，读取 GetCooldownRemaining，并显示向上取整的剩余秒数。它不决定能否召唤，也不修改冷却

## 事件怎样接到提示

事件由修改状态的代码发送，不需要提示组件每帧检查全部状态

| 变化 | 通知来源 | 提示组件的处理 |
|---|---|---|
| 上车、下车或重新获得角色控制权 | PlayerController.OnPossessedPawnChanged | HandlePawnChanged 解绑旧对象，订阅新对象，再刷新提示 |
| 切换驾驶视角 | 车辆 Events 发布 CameraChanged | HandleVehicleEvent 调用 RefreshHint |
| 召唤成功 | 服务器广播 OnCooldownChanged，客户端在复制回调中广播 | RefreshHint 显示剩余秒数 |
| 输入映射重建 | ControlMappingsRebuiltDelegate | 重新查询当前动作对应的按键 |
| 键盘、鼠标或手柄输入 | PlayerController.InputKey | SetInputDevice 在设备类型变化时刷新 |
| 场景中的车辆生成或销毁 | 世界的 Actor 生成、销毁通知 | 下一帧重新检查是否可以提供召唤提示 |

角色变成车辆后，HandlePawnChanged 先取消订阅角色的召唤通知，再订阅车辆事件。RefreshHint 优先检查当前 Pawn 是否为摩托车，因此驾驶时显示切换视角提示，下车后恢复召唤提示

## 为什么倒计时仍需要计时器

开始冷却只发生一次，但界面要把 10 秒改成 9 秒、8 秒。提示组件在冷却尚未结束时设置一次计时器，到时重新读取服务器时间，并决定是否再设置下一次

```cpp
// 来源：Source/Delivery/UI/DeliveryHintComponent.cpp
// 每次用服务器结束时间重算，卡顿时不会把倒计时越拖越长
GetWorld()->GetTimerManager().SetTimer(
    CountdownTimer, this, &UDeliveryHintComponent::RefreshHint,
    FMath::Max(0.01f, FMath::Min(Remaining, 1.0f)), false);
```

冷却结束后不再设置计时器。切换到车辆时先清除已有计时器，组件销毁时也会清除计时器和事件订阅。提示组件和召唤组件都不使用 Tick 更新提示

## 提示怎样显示到界面

RefreshHint 调用 ShowHint，ShowHint 把文本交给 PromptSubsystem.SetCornerHint。子系统保存文本、灰显状态和来源对象，Slate 从这些数据生成左下角显示内容

没有可显示内容时调用 ClearCornerHint，并传入当前提示组件作为来源。子系统只有在来源匹配时才清除，避免旧对象退出时误删新对象的提示。来源失效时也不显示旧文本

左下角提示不再依靠每帧推送维持显示。场景交互浮窗、任务目标和字幕仍沿用各自原来的更新规则，它们与左下角操作提示是不同的显示位置

## 修改和排查

改按键先打开 IMC_Default，检查动作、键盘键和手柄键。提示通过 QueryKeysMappedToAction 查询当前生效的映射，不保存一份独立的键名。当前输入设备没有对应映射时隐藏该操作提示

按键没有反应时，先看动作是否映射，再看当前控制的是角色还是车辆，最后看对应组件是否接受操作。IA 只负责传入操作，晕倒、收拳、抓取和服务器权限等条件仍由原玩法代码检查

上车后提示不对时，检查 HandlePawnChanged 是否接到新的车辆；按 P 后文字没有变化时，检查车辆是否发出 CameraChanged；召唤后没有倒计时时，检查服务器的 ReadyServerTime 和拥有者客户端的复制回调

输入缓冲未在本次增加。鼠标的 0.2 秒等待用于区分单键出拳与双键抓取，不会把收拳期间被拒绝的攻击保存为下一拳

## 验证范围

DeliveryEditor 完整编译通过，26 项自动化检查全部通过，其中原套件有 6 项带警告
双人 PIE 在 TestForCharacter 验证键盘召唤、上下车、驾驶视角、手机开关和模拟手柄切换视角，提示组件全程未开启 Tick
五格物品栏的键盘与手柄映射、动作绑定及控制权切换由自动化检查覆盖，尚未逐一实机验证每格携带物交换
原有输入映射全部保留，新增文档已按 390 像素手机宽度渲染检查

手机输入现在转交 PhoneController，HUD 统一保存面板状态；具体实现见 [PhoneMVC.md](PhoneMVC.md)
