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

