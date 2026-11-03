# 手机界面与玩家控制器

手机会在步行和驾驶时使用，因此相关对象不能跟着当前 Pawn 一起切换。玩家控制器保留输入和服务器请求，手机界面逻辑放在 `UI/Controllers/DeliveryPhoneController`，由 HUD 创建和销毁。这与已有任务、快捷栏和交互处理器使用 UObject 的方式一致

## 各个对象负责什么

| 对象 | 职责 | 主要状态 |
|---|---|---|
| DeliveryPhoneCallQueueComponent | 服务器管理来电队列、接听、挂断和通话时长，向客户端复制 | 当前电话、通话阶段、开始时间 |
| DeliveryPhoneController | 处理手机操作，读取通话内容，协调对白并更新字幕 | 当前对白编号、来电者名字、已显示的字幕 |
| DeliveryHUD | 创建手机处理器，管理多个面板的开关和鼠标模式 | OpenPanels 面板集合 |
| 手机 Widget 蓝图 | 生成手机布局，显示内容，响应按钮 | 控件和显示效果 |
| DeliveryPhoneAudioComponent | 监听电话状态，播放铃声、接听、挂断和手机操作音效 | 铃声音频实例、上次电话状态 |
| DeliveryDialogueVoiceComponent | 将台词变成电子语音，并通知当前读到哪个字 | 语音播放进度 |
| DeliveryPlayerController | 接收 IA_TogglePhone，转交手机操作，提供接听和挂断的服务器入口 | 输入配置、跨角色使用的组件引用 |

电话队列就是这里的 Model。处理器读取这份已有数据，不重新建立一份电话队列。HUD 的面板集合是本机界面状态，不参与网络复制

## 按 J 打开手机

`SetupInputComponent` 绑定 IA_TogglePhone。动作触发后，PC 的 HandleTogglePhonePressed 只取得手机处理器并调用 Toggle

```cpp
// 来源：Source/Delivery/Player/DeliveryPlayerController.cpp
// 输入只转交操作，手机开关规则由处理器决定
void ADeliveryPlayerController::HandleTogglePhonePressed()
{
    if (auto* Phone = GetPhoneController()) Phone->Toggle();
}
```

Toggle 读取 HUD 中的手机面板状态，改变状态后让原手机蓝图更新显示。开关提示音交给 PhoneAudio 播放。这里保留 PC 的 TogglePhoneUI 蓝图事件作为现有 Widget 的连接入口，事件本身不保存界面状态，也不决定如何播放声音

```cpp
// 来源：Source/Delivery/UI/Controllers/DeliveryPhoneController.cpp
void UDeliveryPhoneController::Toggle()
{
    if (!Player.IsValid() || !HUD.IsValid()) return;
    if (auto* Audio = Player->FindComponentByClass<UDeliveryPhoneAudioComponent>()) Audio->PlayToggle();
    SetOpen(!IsOpen());
    // 现有手机蓝图通过这个事件显示或移除控件
    Player->TogglePhoneUI();
}
```

手机自动展开、接听后收起或 Widget 自行关闭时，原蓝图仍可以调用 ReportPhoneUIOpen。这个兼容入口现在把状态转交给处理器，再由 HUD 更新面板集合。新蓝图可通过 GetPhoneController 直接调用 SetOpen、Answer 和 HangUp

## 面板关闭后，为什么鼠标有时还在

手机和其他界面可能同时打开。每个界面只报告自己的开关状态，HUD 判断是否还有其他面板存在

```cpp
// 来源：Source/Delivery/UI/DeliveryHUD.cpp
void ADeliveryHUD::SetPanelOpen(FName PanelId, bool bOpen)
{
    if (PanelId.IsNone()) return;
    const bool bWasOpen = IsAnyPanelOpen();
    if (bOpen) OpenPanels.Add(PanelId);
    else OpenPanels.Remove(PanelId);
    // 只有首个面板打开或最后一个面板关闭时才改变输入模式，避免反复抢焦点
    if (bWasOpen != IsAnyPanelOpen()) ApplyInputMode();
}
```

HUD 最终仍调用 PlayerController 的 SetInputMode 和 bShowMouseCursor，因为这些是引擎提供的玩家输入接口。面板判断已经从 PC 移出，PC 不再保存 OpenUIPanels 或 bPhoneUIOpen

## 来电、接听和字幕

PhoneController 初始化后订阅电话队列的 OnPhoneStateChanged。客户端的 GameState 可能尚未复制到位，此时用 0.5 秒的一次性计时器重试；连接成功就清除计时器，并读取当前电话。晚加入的玩家因此也能获得正在进行的通话内容

进入 InCall 后，处理器按 CallId 判断这通对白是否已经开始。同一状态可能分别由队列和状态字段的复制回调通知，不能因此把语音从头播放两遍。确认是新对白后，处理器读取来电者与台词，请求 DialogueVoice 播放

语音组件每读出一个字就发出 OnGlyphRevealed。处理器把这个字加入字幕，再交给 PromptSubsystem 显示。离开 InCall 时停止语音并清除字幕。处理器保存的是显示进度，实际电话状态仍以队列为准

接听和挂断由处理器的 Answer、HangUp 发起，经 PC 的原有 Server RPC 到达服务器。RPC 留在玩家拥有的 PC 上，电话队列仍在 GameState 上，原来的多人共享规则没有改变

## 手机关闭时铃声也会响

PhoneAudio 是 PC 上的独立组件，它自己订阅电话队列，不依赖 Widget 是否存在，也不依赖手机是否打开。进入 Ringing 开始循环铃声，离开 Ringing 停止并销毁铃声音频实例

它记录上一次已处理的通话状态，重复通知不会重复播放音效。第一次收到状态时只恢复当前铃声，不补播已经发生的接听或挂断声音

原 PC 蓝图的 PhoneToggleSound、IncomingCallSound、AnswerCallSound、HangUpCallSound 已迁移到同一蓝图的 PhoneAudio 组件中。以后在 `/Game/Blueprint/PlayerController/BP_DeliveryManPC` 选择 PhoneAudio 配置这四个资源

## 对象销毁与控制权切换

上下车只改变 Pawn，HUD、手机处理器和电话音效组件继续存在，不重新订阅电话。HUD 结束时调用处理器的 Shutdown，清除等待计时器，解除电话和语音订阅，停止对白并清空字幕

电话音效组件在自身 EndPlay 中解除电话订阅、清除重试计时器并停止铃声。UI 处理器使用弱引用访问玩家和 Model，由 HUD 的 UPROPERTY 保存处理器，既防止处理器被提前回收，也避免处理器反过来延长旧玩家的生命周期

## 代码位置

- `Source/Delivery/UI/Controllers/DeliveryPhoneController.h/.cpp`：手机操作、对白和字幕
- `Source/Delivery/UI/DeliveryHUD.h/.cpp`：处理器生命周期、面板集合、鼠标模式
- `Source/Delivery/Audio/DeliveryPhoneAudioComponent.h/.cpp`：电话音效
- `Source/Delivery/Player/DeliveryPlayerController.h/.cpp`：输入、网络请求和旧蓝图转发入口
- `Source/Delivery/Phone/DeliveryPhoneCallQueueComponent.h/.cpp`：原有电话队列与网络状态
- `Source/Delivery/UI/Tests/DeliveryPhoneControllerTests.cpp`：多个面板、重复通知和处理器解绑检查

任务、快捷栏、交互的已有处理器继续使用原来的数据来源。操作提示继续由独立 HintComponent 监听控制对象、视角和冷却，本次不重复实现这些系统


手机 MVC 验证：完整编译通过，27 项自动化检查通过，其中原套件有 6 项带警告。双人 PIE 验证了手动手机开关隔离、共享来电展开、客户端接听后两边播放对白、挂断停声并收起，以及召唤、上下车和视角切换。手机状态、处理器查询与鼠标显示一致。四个电话音效引用已迁移并在重启后核对，HTML 已按手机宽度渲染检查。
