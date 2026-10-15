# 人物电子语音系统

## 当前目标

第一版实现独游常见的 text blip / gibberish voice：它不朗读真实语音，而是在文字逐字出现时
生成短促电子音，用小音阶、句尾升降和标点停顿产生抑扬顿挫。实现不依赖 SoundWave 或
MetaSound 资产，克隆工程后编译即可使用；后续可把发声后端换成 MetaSound，调用接口保持不变。

## 结构

- `UDeliveryDialogueVoiceComponent`：继承 `USynthComponent`，默认挂在
  `ADeliveryPlayerController` 上。只在本机 Controller 启动，不复制。
- `FDeliveryVoiceGenerator`：运行在音频生成线程。主振荡器混合正弦/三角/少量方波，另有轻微失谐的
  高八度副振荡器；每个字带 24ms 快速滑音、短噪声音头和弱颤音。字符 Hash 会选择四组弱共振峰之一，
  形成不同“口型”色彩，但不会合成可识别音节。最后经过低通与轻微软削波输出双声道 UI 音频。
- 游戏线程按字符设置计时器：广播逐字事件，普通字符触发一个短音，空格和标点只负责停顿。
- `FDeliveryVoiceSettings`：保存基础频率、音高跨度、字间隔、标点停顿、音量、亮度、波形比例、
  副振荡器、音头、颤音和共振峰强度。所有设置都可在蓝图里调，不需要改合成器代码。
- `EDeliveryVoicePreset`：当前内置 `Normal / Low / High / Robot / Angry` 五种验收声线。

## 蓝图接口

从 PlayerController 调 `GetDialogueVoice()`，或用静态节点 `Get Delivery Dialogue Voice` 从
Controller/Pawn 查找组件。

| 接口 | 用途 |
|---|---|
| `SpeakText(Text)` | 用组件默认配置播放 |
| `SpeakTextWithPreset(Text, Preset)` | 用内置声线播放 |
| `SpeakTextWithSettings(Text, Settings)` | 用完整自定义参数播放；以后 Voice Profile DataAsset 接这里 |
| `StopSpeaking()` | 立即停止调度和当前短音 |
| `IsSpeaking()` | 查询是否还在逐字播放 |
| `OnGlyphRevealed(Glyph, Index)` | Widget 绑定后逐字追加显示 |
| `OnLineFinished` | 整句停顿结束后广播 |

电话和 NPC 不要各自再创建 Audio Component。它们把文本交给 PlayerController 上这一份组件，
并绑定 `OnGlyphRevealed` 更新文字即可。多人时只同步“当前台词/对话状态”，声音由每台客户端本地生成。

## 电话任务接入

电话任务已经自动接入，不需要在 `WBP_TaskHUD` 里手动调用播放：

1. `DeliveryPhoneCallQueueComponent::OnPhoneStateChanged` 进入 `InCall`。
2. 本机 `ADeliveryPlayerController` 用 `GetCallContent(Call)` 取得任务资产里的 `UnlockCall.Dialogue`
   或 `OverdueCall.Dialogue`。
3. Controller 调自己的 `DialogueVoice->SpeakText(Dialogue)`；转回 `Ringing / Idle` 或挂断时立即停止。
4. PlayerController 同时监听 `OnGlyphRevealed`，把字符累积到 `DeliveryPromptSubsystem` 的独立字幕槽；
   字幕显示在屏幕下方中央，带来电人姓名并自动换行。挂断或通话结束时自动清空，Widget 不用再绑事件。

通话结束仍由服务器的 `DurationSeconds` 决定，不由某个客户端的语音播放进度决定。任务资产里的
`DurationSeconds` 应比实际逐字播放时间多留约 0.5 秒，否则服务器先结束时本机声音会被正确截断。
当前一通电话就是一段单向台词，因此不需要对话树；等出现玩家选项、多角色轮流说话、条件分支时，
再增加独立的 Conversation DataAsset，电话队列只引用它，不把分支状态塞进任务状态机。

## 游戏事件音效槽

第一版不设全局音频管理器。声音跟着真正拥有事件和网络判定的对象走，蓝图只需要填写资产；
所有字段都是 `USoundBase`，因此可以放 Sound Wave、Sound Cue 或 MetaSound Source。

| 在哪里填写 | 字段 | 触发时机 |
|---|---|---|
| `BP_DeliveryMan` Class Defaults → `Audio / Interaction` | `PickupSuccessSound` | E 键取普通物品或任务快递，并且服务器确认放进背包成功 |
| 同上 | `DeliverySuccessSound` | E 键交付，并且 `TryDeliver` 真正完成任务 |
| `BP_DeliveryMan` 的 `RagdollCombat` 组件 → `Punch / Audio` | `LeftPunchSound` / `RightPunchSound` | 左/右拳蓄力结束、拳头真正前送的瞬间 |
| `BP_DeliveryMan` 的 `GrabComponent` → `Grab / Audio` | `GrabStartSound` / `GrabReleaseSound` | 双键找到可抓目标并开始本机预测 / 松手 |
| `BP_DeliveryManPC` Class Defaults → `Delivery / Audio / Phone` | `PhoneToggleSound` | J 键切换手机界面 |
| 同上 | `IncomingCallSound` | 电话进入 `Ringing`；资产应自行循环，离开响铃状态时程序停止 |
| 同上 | `AnswerCallSound` / `HangUpCallSound` | 电话状态切到 `InCall` / 从响铃或通话回到 `Idle` |
| `BP_Motorbike` Class Defaults → `Motorbike / Audio` | `EngineLoopSound` | 有驾驶员时循环，离车停止；按车速在 Idle/Max Pitch 与 Volume 之间插值 |
| 同上 | `EnterVehicleSound` / `ExitVehicleSound` | 服务器确认上下车后，以车的位置向所有客户端播放 |
| 同上 | `TrafficImpactSound` | 交通车撞击被服务器记账时，以车的位置向所有客户端播放 |
| 同上 | `SummonSound` | R 召唤成功落位后，以车的位置向所有客户端播放 |
| 同上 | `CameraToggleSound` | 本机按 P 切换固定/自由视角 |

取件和交付音放在服务器成功返回之后，再通过 owning-client RPC 播放，所以失败的按键不会误响。
左右拳和抓取属于即时操作反馈，跟本机预测播放，避免多等一次网络往返。车辆发动机是挂在车上的
3D `AudioComponent`；`EngineLoopSound` 必须在 Sound Wave、Sound Cue 或 MetaSound 内开启循环，
C++ 不会把一个非循环资产强行循环。通用 F 交互暂时没有统一声音：上车已经走车辆音效，而门、NPC、
机关需要不同声音，等它们各自有明确行为后放到各自执行成功的位置更合适。

### 当前生成的占位资产

`/Game/Sound/SFX` 已有六个纯程序生成的占位 SoundWave，不使用外部采样，因此没有第三方授权问题。
第二版不再只叠基础波形：电话音使用带动态音色变化的双算子 FM，任务提示使用非整数泛音钟体；
统一经过 10.5kHz 低通、轻微软削波、短立体声反射和微弱合唱延迟，保留电子感但减少尖锐、单薄的蜂鸣感：

- `SFX_Phone_Toggle`：手机开关的短促双向滑音。
- `SFX_Phone_Ring`：两秒循环的三音电子铃声，导入脚本会自动开启 Looping。
- `SFX_Phone_Answer` / `SFX_Phone_HangUp`：上行接通与下行挂断。
- `SFX_Task_Pickup`：双音取件确认。
- `SFX_Task_Delivered`：四音上行交付成功提示。

前四个已绑定到 `BP_DeliveryManPC`，后两个已绑定到 `BP_DeliveryMan`。原始 48kHz 立体声 WAV
保存在 `Design/Audio/GeneratedSFX`，合成与导入脚本是 `Content/Python/generate_gameplay_sfx.py`。
调整脚本里的音高、时长或波形后，在编辑器 Python 控制台运行：

```python
import importlib, generate_gameplay_sfx as sfx; importlib.reload(sfx); sfx.run()
```

它会覆盖更新这六个 SoundWave、重新开启铃声循环并恢复蓝图默认绑定。

PIE 控制台可直接试听，不必走一遍任务流程：

```text
Delivery.Audio.Test toggle
Delivery.Audio.Test ring
Delivery.Audio.Test answer
Delivery.Audio.Test hangup
Delivery.Audio.Test pickup
Delivery.Audio.Test delivered
Delivery.Audio.Stop
```

## 节奏规则

- 普通字符发声；空格、换行、中文/英文标点不发声。
- `，、；：` 使用 `CommaPause`；`。！？……` 使用 `SentencePause`。
- 字符和位置经过稳定 Hash 后从 `[-3, 0, 2, 4, 7]` 小音阶选音，同一句重复播放不会随机变调。
- 问号前最后三个发声字符逐步上扬；普通句尾最后两个字符轻微下降。
- 单个短音应短于字间隔，否则会变成连续蜂鸣。默认 `0.06s < 0.075s`。
- 每个字的滑音方向、噪声和共振组合都来自字符 Hash；同一句可重复验收，不会每次随机换声线。

## 调音顺序

先只调 `BaseFrequency / CharacterInterval / BlipDuration`，确定角色的高低和语速；再调
`Brightness / TriangleMix / SquareMix` 确定材质。感觉仍太薄时增加 `HarmonicMix`，太干净时增加
`NoiseAmount`，太像固定蜂鸣时增加 `PitchAttackSemitones`。`FormantAmount` 默认只需 0.15~0.35；
调得过高会变成明显的滤波哇音。`VibratoDepthSemitones` 建议保持在 0.3 以下。

## PIE 控制台验收

```text
Delivery.Voice.Test
Delivery.Voice.Test low
Delivery.Voice.Test high
Delivery.Voice.Test robot
Delivery.Voice.Test angry
Delivery.Voice.Say 这是一条自定义测试台词，请注意句号和问号？
Delivery.Voice.Stop
```

命令找不到组件时，检查当前 GameMode 的 PlayerController 是否继承
`ADeliveryPlayerController`（项目正常使用的 `BP_DeliveryManPC` 已满足）。命令只存在于非 Shipping 构建。

## 后续扩展

1. 新建 `UDeliveryVoiceProfile` DataAsset，让角色/NPC 引用可复用声线，而不是复制 struct 参数。
2. 将 MetaSound 作为可选发声后端，用 Preset 做更复杂的角色音色；文本调度和 Widget 接口不变。
3. 创建 `SC_Voice` SoundClass，接设置菜单里的独立语音音量。
4. 电话通话阶段加入带通、失真和窄带噪声，普通 NPC 世界对白再增加 3D 空间化。
5. 快进整句时限制短音数量，避免一次触发几十个字符产生爆音。

