// Copyright Epic Games, Inc. All Rights Reserved.

#include "Audio/DeliveryDialogueVoiceComponent.h"

#include "Delivery.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"

namespace
{
constexpr float TwoPi = 2.f * PI;

class FDeliveryVoiceGenerator final : public ISoundGenerator
{
	struct FBandPass
	{
		// 根据中心频率和带宽设置滤波系数，保留需要的音色频段
		void Configure(float InFrequency, float Q, float InSampleRate)
		{
			const float Omega = TwoPi * FMath::Clamp(InFrequency, 80.f, InSampleRate * 0.42f) / InSampleRate;
			const float SinOmega = FMath::Sin(Omega);
			const float Alpha = SinOmega / (2.f * FMath::Max(Q, 0.1f));
			const float A0 = 1.f + Alpha;
			B0 = Alpha / A0;
			B1 = 0.f;
			B2 = -Alpha / A0;
			A1 = -2.f * FMath::Cos(Omega) / A0;
			A2 = (1.f - Alpha) / A0;
			Reset();
		}

		// 处理一个音频采样，并保留滤波状态供下一个采样使用
		float Process(float Input)
		{
			const float Output = B0 * Input + Z1;
			Z1 = B1 * Input - A1 * Output + Z2;
			Z2 = B2 * Input - A2 * Output;
			return Output;
		}

		// 清空上一段声音留下的滤波状态
		void Reset()
		{
			Z1 = 0.f;
			Z2 = 0.f;
		}

		// B 系数控制输入，A 系数控制反馈，Z1 和 Z2 保存前一次计算结果
		float B0 = 1.f;
		float B1 = 0.f;
		float B2 = 0.f;
		float A1 = 0.f;
		float A2 = 0.f;
		float Z1 = 0.f;
		float Z2 = 0.f;
	};

  public:
	FDeliveryVoiceGenerator(float InSampleRate, int32 InNumChannels)
	    : SampleRate(FMath::Max(InSampleRate, 8000.f)), NumChannels(FMath::Max(InNumChannels, 1))
	{
	}

	virtual int32 GetNumChannels() const override
	{
		return NumChannels;
	}

	// 把发声参数送到音频线程，开始播放当前字符对应的短音
	void Trigger(float InFrequency, float InDuration, float InVolume, float InBrightness, float InTriangleMix,
	             float InSquareMix, float InHarmonicMix, float InDetuneCents, float InNoiseAmount,
	             float InPitchAttackSemitones, float InVibratoDepthSemitones, float InVibratoRate,
	             float InFormantAmount, uint32 InGlyphHash)
	{
		SynthCommand(
		    [this, InFrequency, InDuration, InVolume, InBrightness, InTriangleMix, InSquareMix, InHarmonicMix,
			 InDetuneCents, InNoiseAmount, InPitchAttackSemitones, InVibratoDepthSemitones, InVibratoRate,
			 InFormantAmount, InGlyphHash]()
		    {
			    Frequency = FMath::Clamp(InFrequency, 40.f, SampleRate * 0.4f);
			    Volume = FMath::Clamp(InVolume, 0.f, 0.8f);
			    Brightness = FMath::Clamp(InBrightness, 100.f, SampleRate * 0.45f);
			    TriangleMix = FMath::Clamp(InTriangleMix, 0.f, 1.f);
			    SquareMix = FMath::Clamp(InSquareMix, 0.f, 1.f - TriangleMix);
			    HarmonicMix = FMath::Clamp(InHarmonicMix, 0.f, 0.6f);
			    DetuneRatio = FMath::Pow(2.f, FMath::Clamp(InDetuneCents, 0.f, 40.f) / 1200.f);
			    NoiseAmount = FMath::Clamp(InNoiseAmount, 0.f, 0.3f);
			    const float GlideDirection = (InGlyphHash & 1u) == 0u ? 1.f : -1.f;
			    PitchAttackSemitones = FMath::Clamp(InPitchAttackSemitones, 0.f, 8.f) * GlideDirection;
			    VibratoDepthSemitones = FMath::Clamp(InVibratoDepthSemitones, 0.f, 2.f);
			    VibratoRate = FMath::Clamp(InVibratoRate, 0.1f, 20.f);
			    FormantAmount = FMath::Clamp(InFormantAmount, 0.f, 0.8f);
			    TotalFrames = FMath::Max(FMath::RoundToInt(InDuration * SampleRate), 1);
			    FramesRemaining = TotalFrames;
			    Phase = 0.f;
			    HarmonicPhase = (InGlyphHash & 2u) == 0u ? 0.f : PI * 0.5f;
			    VibratoPhase = static_cast<float>((InGlyphHash >> 2u) & 255u) / 255.f * TwoPi;
			    NoiseState = InGlyphHash != 0u ? InGlyphHash : 0x6d2b79f5u;
			    FilteredSample = 0.f;

			    // 四种很弱的共振组合只提供“口型色彩”，不会形成可识别的元音或语音
			    static constexpr float Formants[][2] = {
			        {650.f, 1300.f}, {430.f, 2050.f}, {780.f, 2450.f}, {360.f, 1650.f}};
			    const int32 Variant = static_cast<int32>((InGlyphHash >> 10u) % UE_ARRAY_COUNT(Formants));
			    FormantLow.Configure(Formants[Variant][0], 2.2f, SampleRate);
			    FormantHigh.Configure(Formants[Variant][1], 2.8f, SampleRate);
		    });
	}

	// 在音频线程停止发声并清空滤波残留
	void Silence()
	{
		SynthCommand(
		    [this]()
		    {
			    FramesRemaining = 0;
			    FilteredSample = 0.f;
			    FormantLow.Reset();
			    FormantHigh.Reset();
		    });
	}

	// 逐个生成声音采样，再写入所有输出声道
	virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override
	{
		const int32 NumFrames = NumSamples / NumChannels;
		int32 SampleIndex = 0;

		for (int32 Frame = 0; Frame < NumFrames; ++Frame)
		{
			float Output = 0.f;
			if (FramesRemaining > 0)
			{
				const int32 ElapsedFrames = TotalFrames - FramesRemaining;
				// 先快速增大音量，再逐渐减小，避免声音突然开始或结束产生杂音
				const int32 AttackFrames = FMath::Max(FMath::RoundToInt(0.004f * SampleRate), 1);
				float Envelope = 1.f;
				if (ElapsedFrames < AttackFrames)
				{
					Envelope = static_cast<float>(ElapsedFrames) / AttackFrames;
				}
				else
				{
					const float ReleaseProgress =
					    static_cast<float>(FramesRemaining) / FMath::Max(TotalFrames - AttackFrames, 1);
					Envelope = ReleaseProgress * ReleaseProgress;
				}

				const float GlideFrames = FMath::Max(0.024f * SampleRate, 1.f);
				const float GlideProgress = FMath::Clamp(ElapsedFrames / GlideFrames, 0.f, 1.f);
				const float GlideSemitones =
				    PitchAttackSemitones * (1.f - FMath::SmoothStep(0.f, 1.f, GlideProgress));
				const float VibratoFade =
				    FMath::Clamp(static_cast<float>(ElapsedFrames) / FMath::Max(TotalFrames, 1), 0.f, 1.f);
				const float VibratoSemitones = FMath::Sin(VibratoPhase) * VibratoDepthSemitones * VibratoFade;
				// 将滑音和颤音的半音偏移换算成频率，每升高十二个半音频率翻倍
				const float InstantFrequency =
				    Frequency * FMath::Pow(2.f, (GlideSemitones + VibratoSemitones) / 12.f);

				// 混合不同波形和高音成分，形成角色的基础音色
				const float Sine = FMath::Sin(Phase);
				const float Triangle = (2.f / PI) * FMath::Asin(Sine);
				const float Square = Sine >= 0.f ? 1.f : -1.f;
				const float SineMix = FMath::Max(1.f - TriangleMix - SquareMix, 0.f);
				const float Fundamental = Sine * SineMix + Triangle * TriangleMix + Square * SquareMix;
				const float Harmonic = FMath::Sin(HarmonicPhase);
				float Raw = Fundamental * (1.f - HarmonicMix) + Harmonic * HarmonicMix;

				// 只在发声开头加入短暂噪声，增加起音变化
				const int32 NoiseFrames = FMath::Max(FMath::RoundToInt(0.012f * SampleRate), 1);
				if (ElapsedFrames < NoiseFrames && NoiseAmount > 0.f)
				{
					NoiseState = NoiseState * 1664525u + 1013904223u;
					const float Noise = static_cast<float>((NoiseState >> 8u) & 0xffffu) / 32767.5f - 1.f;
					const float NoiseEnvelope = 1.f - static_cast<float>(ElapsedFrames) / NoiseFrames;
					Raw += Noise * NoiseAmount * NoiseEnvelope * NoiseEnvelope;
				}

				const float Formant = FMath::Clamp(
				    (FormantLow.Process(Raw) + FormantHigh.Process(Raw) * 0.65f) * 1.6f, -1.5f, 1.5f);
				Raw = FMath::Lerp(Raw, Formant, FormantAmount);

				// 削弱过亮的高频，并平滑限制峰值，避免音量突变
				const float FilterAlpha = 1.f - FMath::Exp(-TwoPi * Brightness / SampleRate);
				FilteredSample += FilterAlpha * (Raw - FilteredSample);
				Output = FMath::Tanh(FilteredSample * 1.25f) * Volume * Envelope;

				Phase = FMath::Fmod(Phase + TwoPi * InstantFrequency / SampleRate, TwoPi);
				HarmonicPhase = FMath::Fmod(
				    HarmonicPhase + TwoPi * InstantFrequency * 2.f * DetuneRatio / SampleRate, TwoPi);
				VibratoPhase = FMath::Fmod(VibratoPhase + TwoPi * VibratoRate / SampleRate, TwoPi);
				--FramesRemaining;
			}

			for (int32 Channel = 0; Channel < NumChannels; ++Channel)
			{
				OutAudio[SampleIndex++] = Output;
			}
		}

		while (SampleIndex < NumSamples)
		{
			OutAudio[SampleIndex++] = 0.f;
		}
		return NumSamples;
	}

  private:
	float SampleRate = 48000.f;
	int32 NumChannels = 2;
	float Frequency = 280.f;
	float Volume = 0.18f;
	float Brightness = 2600.f;
	float TriangleMix = 0.42f;
	float SquareMix = 0.08f;
	float HarmonicMix = 0.16f;
	float DetuneRatio = 1.f;
	float NoiseAmount = 0.035f;
	float PitchAttackSemitones = 1.8f;
	float VibratoDepthSemitones = 0.16f;
	float VibratoRate = 6.2f;
	float FormantAmount = 0.24f;
	// 记录各波形播放到的位置，让相邻采样连续
	float Phase = 0.f;
	float HarmonicPhase = 0.f;
	float VibratoPhase = 0.f;
	float FilteredSample = 0.f;
	uint32 NoiseState = 0x6d2b79f5u;
	FBandPass FormantLow;
	FBandPass FormantHigh;
	int32 TotalFrames = 0;
	// 当前字符还要生成多少帧声音，减到零后输出静音
	int32 FramesRemaining = 0;
};

bool IsComma(TCHAR Glyph)
{
	return Glyph == TEXT(',') || Glyph == TEXT('，') || Glyph == TEXT('、') || Glyph == TEXT(';') ||
	       Glyph == TEXT('；') || Glyph == TEXT(':') || Glyph == TEXT('：');
}

bool IsQuestion(TCHAR Glyph)
{
	return Glyph == TEXT('?') || Glyph == TEXT('？');
}

bool IsSentenceEnd(TCHAR Glyph)
{
	return Glyph == TEXT('.') || Glyph == TEXT('。') || Glyph == TEXT('!') || Glyph == TEXT('！') ||
	       IsQuestion(Glyph) || Glyph == TEXT('…');
}

bool IsPunctuation(TCHAR Glyph)
{
	return IsComma(Glyph) || IsSentenceEnd(Glyph) || Glyph == TEXT('(') || Glyph == TEXT(')') ||
	       Glyph == TEXT('（') || Glyph == TEXT('）') || Glyph == TEXT('"') || Glyph == TEXT('\'') ||
	       Glyph == TEXT('“') || Glyph == TEXT('”') || Glyph == TEXT('《') || Glyph == TEXT('》');
}

// 一个字符是不是"宽字"：汉字、假名、韩文、全角。
// 这类字一个就是一个音节，按 CharacterInterval 原速念；
// 拉丁字母这些窄字一个只是音节的一部分，要按 NarrowGlyphScale 提速。
// 判据按 Unicode 区段而不是"是不是 ASCII"：西里尔、希腊也是拼音文字，该跟拉丁一样快
bool IsWideGlyph(TCHAR Glyph)
{
	const uint32 Code = static_cast<uint32>(Glyph);

	return (Code >= 0x3040 && Code <= 0x30FF)    // 平假名、片假名
	       || (Code >= 0x3400 && Code <= 0x4DBF) // 扩展 A 区汉字
	       || (Code >= 0x4E00 && Code <= 0x9FFF) // 常用汉字
	       || (Code >= 0xAC00 && Code <= 0xD7AF) // 韩文音节
	       || (Code >= 0xF900 && Code <= 0xFAFF) // 兼容汉字
	       || (Code >= 0xFF00 && Code <= 0xFF60); // 全角字母数字
}
}

UDeliveryDialogueVoiceComponent::UDeliveryDialogueVoiceComponent(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
	NumChannels = 2;
	bAllowSpatialization = false;
	bIsUISound = true;
	bAutoDestroy = false;
	bStopWhenOwnerDestroyed = true;
	PrimaryComponentTick.bCanEverTick = false;
}

// 只为本机玩家启动语音生成器
void UDeliveryDialogueVoiceComponent::BeginPlay()
{
	Super::BeginPlay();

	if (IsOwnedByLocalController())
	{
		Start();
	}
}

// 停止对白并清理逐字播放的计时器
void UDeliveryDialogueVoiceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopSpeaking();
	Stop();
	Super::EndPlay(EndPlayReason);
}

// 从角色或控制器查找本机对白语音组件
UDeliveryDialogueVoiceComponent* UDeliveryDialogueVoiceComponent::FindVoice(AActor* Actor)
{
	if (!Actor)
	{
		return nullptr;
	}

	if (const APawn* Pawn = Cast<APawn>(Actor))
	{
		Actor = Pawn->GetController();
	}

	return Actor ? Actor->FindComponentByClass<UDeliveryDialogueVoiceComponent>() : nullptr;
}

// 对白播放入口

void UDeliveryDialogueVoiceComponent::SpeakText(const FText& Text)
{
	SpeakTextWithSettings(Text, DefaultSettings);
}

void UDeliveryDialogueVoiceComponent::SpeakTextWithPreset(const FText& Text, EDeliveryVoicePreset Preset)
{
	SpeakTextWithSettings(Text, MakePresetSettings(Preset));
}

// 替换当前对白，按指定音色开始逐字播放
void UDeliveryDialogueVoiceComponent::SpeakTextWithSettings(const FText& Text,
                                                            const FDeliveryVoiceSettings& Settings)
{
	if (!IsOwnedByLocalController())
	{
		return;
	}

	StopSpeaking();
	ActiveText = Text.ToString();
	ActiveSettings = Settings;
	NextGlyphIndex = 0;
	bSpeaking = !ActiveText.IsEmpty();

	if (!IsPlaying())
	{
		Start();
	}

	if (bSpeaking)
	{
		RevealNextGlyph();
	}
}

// 停止后续字符发声，并结束当前对白
void UDeliveryDialogueVoiceComponent::StopSpeaking()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(GlyphTimer);
	}

	bSpeaking = false;
	ActiveText.Reset();
	NextGlyphIndex = 0;

	if (VoiceGenerator.IsValid())
	{
		static_cast<FDeliveryVoiceGenerator*>(VoiceGenerator.Get())->Silence();
	}
}

// 人物声音预设

// 为不同角色生成音高、音色和语速配置
FDeliveryVoiceSettings UDeliveryDialogueVoiceComponent::MakePresetSettings(EDeliveryVoicePreset Preset)
{
	FDeliveryVoiceSettings Settings;
	switch (Preset)
	{
	case EDeliveryVoicePreset::Low:
		Settings.BaseFrequency = 165.f;
		Settings.PitchVariationSemitones = 3.5f;
		Settings.BlipDuration = 0.075f;
		Settings.CharacterInterval = 0.095f;
		Settings.Brightness = 1900.f;
		Settings.HarmonicMix = 0.22f;
		Settings.PitchAttackSemitones = 1.2f;
		Settings.FormantAmount = 0.32f;
		break;

	case EDeliveryVoicePreset::High:
		Settings.BaseFrequency = 430.f;
		Settings.PitchVariationSemitones = 7.f;
		Settings.BlipDuration = 0.045f;
		Settings.CharacterInterval = 0.06f;
		Settings.Brightness = 3900.f;
		Settings.NoiseAmount = 0.025f;
		Settings.PitchAttackSemitones = 2.5f;
		Settings.VibratoDepthSemitones = 0.22f;
		break;

	case EDeliveryVoicePreset::Robot:
		Settings.BaseFrequency = 220.f;
		Settings.PitchVariationSemitones = 1.f;
		Settings.BlipDuration = 0.055f;
		Settings.CharacterInterval = 0.07f;
		Settings.Brightness = 5200.f;
		Settings.TriangleMix = 0.2f;
		Settings.SquareMix = 0.55f;
		Settings.HarmonicMix = 0.08f;
		Settings.DetuneCents = 0.f;
		Settings.NoiseAmount = 0.06f;
		Settings.PitchAttackSemitones = 0.4f;
		Settings.VibratoDepthSemitones = 0.f;
		Settings.FormantAmount = 0.08f;
		Settings.QuestionRiseSemitones = 2.f;
		break;

	case EDeliveryVoicePreset::Angry:
		Settings.BaseFrequency = 235.f;
		Settings.PitchVariationSemitones = 3.f;
		Settings.BlipDuration = 0.04f;
		Settings.CharacterInterval = 0.052f;
		Settings.Volume = 0.24f;
		Settings.Brightness = 4300.f;
		Settings.TriangleMix = 0.3f;
		Settings.SquareMix = 0.18f;
		Settings.HarmonicMix = 0.24f;
		Settings.DetuneCents = 11.f;
		Settings.NoiseAmount = 0.09f;
		Settings.PitchAttackSemitones = 3.2f;
		Settings.VibratoDepthSemitones = 0.08f;
		Settings.FormantAmount = 0.3f;
		break;

	case EDeliveryVoicePreset::Normal:
	default:
		break;
	}
	return Settings;
}

// 创建实际输出音频采样的声音生成器
ISoundGeneratorPtr UDeliveryDialogueVoiceComponent::CreateSoundGenerator(
    const FSoundGeneratorInitParams& InParams)
{
	return VoiceGenerator = MakeShared<FDeliveryVoiceGenerator, ESPMode::ThreadSafe>(InParams.SampleRate,
	                                                                                 InParams.NumChannels);
}

// 逐字显示与发声

// 显示下一个字符，发声后安排下一个字符的时间
void UDeliveryDialogueVoiceComponent::RevealNextGlyph()
{
	UWorld* World = GetWorld();

	// 延时为零的字符必须在**同一次调用里**继续往下念，不能交给计时器：
	// FTimerManager::SetTimer 收到小于等于 0 的间隔时，会先清掉旧计时器、然后什么都不设
	// （TimerManager.cpp 里那句 `if (InRate > 0.f)`），于是整条对白停在那儿不动。
	// 回车换行那一对里的回车符正是零延时的那一个，而它就在第一行末尾，
	// 所以表现是"念完第一行就愣住"。
	// 写成循环而不是只特判回车符：以后再加零延时的规则也不会把播放链悄悄掐断
	for (;;)
	{
		if (!bSpeaking || !ActiveText.IsValidIndex(NextGlyphIndex))
		{
			bSpeaking = false;
			OnLineFinished.Broadcast();
			return;
		}

		const int32 GlyphIndex = NextGlyphIndex;
		const TCHAR Glyph = ActiveText[NextGlyphIndex++];
		OnGlyphRevealed.Broadcast(FString::Chr(Glyph), GlyphIndex);

		if (ShouldSoundGlyph(Glyph))
		{
			TriggerGlyphSound(Glyph, GlyphIndex);
		}

		const float Delay = DelayForGlyph();
		if (Delay <= 0.f)
		{
			// 这个字符不占时间，直接接着念下一个。回到循环开头会重查 bSpeaking，
			// 因为上面那两个广播有可能在回调里把这句话停掉
			continue;
		}

		if (World)
		{
			World->GetTimerManager().SetTimer(GlyphTimer, this, &UDeliveryDialogueVoiceComponent::RevealNextGlyph,
			                                  Delay, false);
		}

		return;
	}
}

// 把当前字符转换成音高和发声参数
void UDeliveryDialogueVoiceComponent::TriggerGlyphSound(TCHAR Glyph, int32 GlyphIndex)
{
	if (!VoiceGenerator.IsValid())
	{
		UE_LOG(LogDelivery, Warning, TEXT("[Voice] 音频生成器尚未就绪，跳过字符 %d"), GlyphIndex);
		return;
	}

	const float Semitones = ProsodySemitoneOffset(GlyphIndex, Glyph);
	const float Frequency = ActiveSettings.BaseFrequency * FMath::Pow(2.f, Semitones / 12.f);
	const uint32 GlyphHash = HashCombine(GetTypeHash(static_cast<uint32>(Glyph)), GetTypeHash(GlyphIndex));
	static_cast<FDeliveryVoiceGenerator*>(VoiceGenerator.Get())
	    ->Trigger(Frequency, ActiveSettings.BlipDuration, ActiveSettings.Volume, ActiveSettings.Brightness,
		          ActiveSettings.TriangleMix, ActiveSettings.SquareMix, ActiveSettings.HarmonicMix,
		          ActiveSettings.DetuneCents, ActiveSettings.NoiseAmount, ActiveSettings.PitchAttackSemitones,
		          ActiveSettings.VibratoDepthSemitones, ActiveSettings.VibratoRate,
		          ActiveSettings.FormantAmount, GlyphHash);
}

// 文字间隔与语调

// 根据字符和标点决定停顿时长
// 单个字符念完之后要等多久。**这是全项目唯一一份逐字计时规则**，
// 真实播放（DelayForGlyph）和时长估算（EstimateSpeechSeconds）都调它——
// 抄成两份的话会出现"估算说够、实际被截断"，而那种不一致极难想到。
//
// 收整串文本 + 下标而不是单个字符：换行要判 \r\n 是不是一对，
// 只看一个字符做不到（见下面那段注释）
float UDeliveryDialogueVoiceComponent::GlyphDelaySeconds(const FString& Text, int32 Index,
                                                         const FDeliveryVoiceSettings& Settings)
{
	if (!Text.IsValidIndex(Index))
	{
		return 0.f;
	}

	const TCHAR Glyph = Text[Index];

	// 标点是**替换**整个字符间隔而不是叠加在它上面；空格只算半个
	if (Glyph == TEXT('\n') || Glyph == TEXT('\r'))
	{
		// Windows 上写的文本是 \r\n，两个字符各停一次 SentencePause 的话，
		// 换行处会停成别处的两倍——而停顿长短不该取决于文本是用什么换行符存的。
		// 把 \r\n 当成一个换行：\r 不停，停顿记在 \n 上
		const bool bIsCarriageReturnOfPair =
		    Glyph == TEXT('\r') && Text.IsValidIndex(Index + 1) && Text[Index + 1] == TEXT('\n');

		return bIsCarriageReturnOfPair ? 0.f : Settings.SentencePause;
	}
	if (IsComma(Glyph))
	{
		return Settings.CommaPause;
	}
	if (IsSentenceEnd(Glyph))
	{
		return Settings.SentencePause;
	}

	// 一个英文字母承载的信息远少于一个汉字，两者用同一个间隔的话，
	// 中文是风格化的快、英文只有自然朗读速度，听着像卡住。
	// 窄字（拉丁、西里尔、数字…）按倍率提速，汉字/假名/韩文保持原速
	const float Interval =
	    Settings.CharacterInterval * (IsWideGlyph(Glyph) ? 1.f : Settings.NarrowGlyphScale);

	if (FChar::IsWhitespace(Glyph))
	{
		return Interval * 0.5f;
	}

	// 固定文本会得到固定节奏，不用全局随机数，重复播放不会每次都换一种口吃方式。
	// 抖动幅度跟着间隔缩放，否则英文提速之后 ±12ms 会占到间隔的三分之一
	const uint32 Hash = HashCombine(GetTypeHash(static_cast<uint32>(Glyph)), GetTypeHash(Index));
	const float JitterScale = Settings.CharacterInterval > 0.f ? Interval / Settings.CharacterInterval : 1.f;
	const float Jitter = (static_cast<float>(Hash % 2001) / 1000.f - 1.f) * 0.012f * JitterScale;

	return FMath::Max(Interval + Jitter, 0.02f);
}

float UDeliveryDialogueVoiceComponent::DelayForGlyph() const
{
	// RevealNextGlyph 取字符时已经 NextGlyphIndex++，所以当前字符的下标是 NextGlyphIndex - 1。
	return GlyphDelaySeconds(ActiveText, NextGlyphIndex - 1, ActiveSettings);
}

float UDeliveryDialogueVoiceComponent::EstimateSpeechSecondsWithSettings(
    const FText& Text, const FDeliveryVoiceSettings& Settings)
{
	const FString Line = Text.ToString();
	float Seconds = 0.f;
	for (int32 Index = 0; Index < Line.Len(); ++Index)
	{
		Seconds += GlyphDelaySeconds(Line, Index, Settings);
	}

	return Seconds;
}

float UDeliveryDialogueVoiceComponent::EstimateSpeechSeconds(const FText& Text)
{
	// 取 CDO 上的 DefaultSettings：电话对白走的是 SpeakText，用的就是这一份。
	// 蓝图在某个 PlayerController 上改过 DefaultSettings 的话这里看不到——
	// 这只是编辑器期的估算，不值得为它去加载某张图里的某个实例
	const UDeliveryDialogueVoiceComponent* Defaults = GetDefault<UDeliveryDialogueVoiceComponent>();

	return EstimateSpeechSecondsWithSettings(
	    Text, Defaults ? Defaults->DefaultSettings : FDeliveryVoiceSettings());
}

int32 UDeliveryDialogueVoiceComponent::SpokenGlyphCountWithin(const FText& Text, float Seconds)
{
	const UDeliveryDialogueVoiceComponent* Defaults = GetDefault<UDeliveryDialogueVoiceComponent>();
	const FDeliveryVoiceSettings Settings =
	    Defaults ? Defaults->DefaultSettings : FDeliveryVoiceSettings();

	const FString Line = Text.ToString();
	float Elapsed = 0.f;
	for (int32 Index = 0; Index < Line.Len(); ++Index)
	{
		// 和真实播放同序：先念这个字，再等它的延时；延时等完才轮到下一个
		Elapsed += GlyphDelaySeconds(Line, Index, Settings);
		if (Elapsed > Seconds)
		{
			return Index;
		}
	}

	return Line.Len();
}

bool UDeliveryDialogueVoiceComponent::ShouldSoundGlyph(TCHAR Glyph) const
{
	return !FChar::IsWhitespace(Glyph) && !IsPunctuation(Glyph);
}

// 根据句子位置调整音高，形成起伏和句尾变化
float UDeliveryDialogueVoiceComponent::ProsodySemitoneOffset(int32 GlyphIndex, TCHAR Glyph) const
{
	static constexpr float Scale[] = {-3.f, 0.f, 2.f, 4.f, 7.f};
	const uint32 Hash = HashCombine(GetTypeHash(static_cast<uint32>(Glyph)), GetTypeHash(GlyphIndex));
	float Offset = Scale[Hash % UE_ARRAY_COUNT(Scale)] * (ActiveSettings.PitchVariationSemitones / 7.f);

	// 看当前短句的句尾；问句最后三个发声字符逐步上扬；陈述句最后两个略微下降
	int32 EndIndex = GlyphIndex + 1;
	while (ActiveText.IsValidIndex(EndIndex) && !IsSentenceEnd(ActiveText[EndIndex]))
	{
		++EndIndex;
	}

	if (ActiveText.IsValidIndex(EndIndex))
	{
		int32 SoundGlyphsUntilEnd = 0;
		for (int32 Index = GlyphIndex + 1; Index < EndIndex; ++Index)
		{
			if (ShouldSoundGlyph(ActiveText[Index]))
			{
				++SoundGlyphsUntilEnd;
			}
		}

		if (IsQuestion(ActiveText[EndIndex]) && SoundGlyphsUntilEnd < 3)
		{
			Offset +=
			    ActiveSettings.QuestionRiseSemitones * (1.f - static_cast<float>(SoundGlyphsUntilEnd) / 3.f);
		}
		else if (!IsQuestion(ActiveText[EndIndex]) && SoundGlyphsUntilEnd < 2)
		{
			Offset -= 1.5f * (2 - SoundGlyphsUntilEnd);
		}
	}

	return Offset;
}

bool UDeliveryDialogueVoiceComponent::IsOwnedByLocalController() const
{
	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	return Controller && Controller->IsLocalController();
}
