// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SynthComponent.h"
#include "DeliveryDialogueVoiceComponent.generated.h"

/** 第一版内置的角色声线；以后可以由 DataAsset 扩展，不会影响 SpeakText 接口 */
UENUM(BlueprintType)
enum class EDeliveryVoicePreset : uint8
{
	Normal UMETA(DisplayName = "普通"),
	Low UMETA(DisplayName = "低沉"),
	High UMETA(DisplayName = "高亢"),
	Robot UMETA(DisplayName = "机械"),
	Angry UMETA(DisplayName = "生气")
};

/**
 * 一条电子语音的音色和节奏参数
 * 数值刻意保持纯数据，电话、NPC 和任务对白以后可以各自保存一份配置
 */
USTRUCT(BlueprintType)

struct FDeliveryVoiceSettings
{
	GENERATED_BODY()

	// 音调与发声节奏
	/** 声音中心频率（Hz）；成年角色通常 180~320，儿童可到 450 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice",
	          meta = (ClampMin = "60", ClampMax = "1200"))
	float BaseFrequency = 280.f;

	/** 字符之间从小音阶中取音时允许的最大半音跨度 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice",
	          meta = (ClampMin = "0", ClampMax = "12"))
	float PitchVariationSemitones = 5.f;

	/** 单个电子音长度；应略短于 CharacterInterval，避免连成持续蜂鸣 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice",
	          meta = (ClampMin = "0.015", ClampMax = "0.25"))
	float BlipDuration = 0.06f;

	/** 普通字符之间的间隔 */
	/** 普通字符之间的间隔；这个值按"一个字 = 一个音节"定，对应汉字 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice",
	          meta = (ClampMin = "0.025", ClampMax = "0.4"))
	float CharacterInterval = 0.075f;

	/**
	 * 窄字（拉丁字母、数字、西里尔…）的间隔倍率；汉字/假名/韩文不受影响
	 * 一个英文字母承载的信息远少于一个汉字，两者同速的话中文是风格化的快、
	 * 英文只有自然朗读速度（实测 2.6 单词/秒），听着像卡住。
	 * 0.45 让英文到约 5.7 单词/秒，相对自然语速的倍数和中文那边对得上
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice",
	          meta = (ClampMin = "0.15", ClampMax = "1"))
	float NarrowGlyphScale = 0.45f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice",
	          meta = (ClampMin = "0.03", ClampMax = "1"))
	float CommaPause = 0.14f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice",
	          meta = (ClampMin = "0.05", ClampMax = "2"))
	float SentencePause = 0.26f;

	// 音量与音色
	/** 合成器内部线性音量；最终仍会经过组件/SoundClass 音量 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice",
	          meta = (ClampMin = "0", ClampMax = "0.6"))
	float Volume = 0.18f;

	/** 一阶低通截止频率；越低越柔和，越高越接近尖锐电子音 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice",
	          meta = (ClampMin = "300", ClampMax = "12000"))
	float Brightness = 2600.f;

	/** 三角波比例；它负责主体的卡通电子质感 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice",
	          meta = (ClampMin = "0", ClampMax = "1"))
	float TriangleMix = 0.42f;

	/** 方波比例；太高会刺耳，机械角色可以提高 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice",
	          meta = (ClampMin = "0", ClampMax = "0.8"))
	float SquareMix = 0.08f;

	/** 高八度副振荡器比例；少量可让声音更厚，过高会像蜂鸣器 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice",
	          meta = (ClampMin = "0", ClampMax = "0.6"))
	float HarmonicMix = 0.16f;

	/** 副振荡器与主音的失谐量（音分），负责轻微的颤动和厚度 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice",
	          meta = (ClampMin = "0", ClampMax = "40"))
	float DetuneCents = 7.f;

	/** 每个字音头的短噪声比例，只在开头约 12ms 出现 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice",
	          meta = (ClampMin = "0", ClampMax = "0.3"))
	float NoiseAmount = 0.035f;

	/** 每个字从偏离目标的音高快速滑入，形成说话般的音头 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice",
	          meta = (ClampMin = "0", ClampMax = "8"))
	float PitchAttackSemitones = 1.8f;

	/** 字内微颤音深度（半音）；默认很弱，只用来打破固定蜂鸣感 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice",
	          meta = (ClampMin = "0", ClampMax = "2"))
	float VibratoDepthSemitones = 0.16f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice",
	          meta = (ClampMin = "0.1", ClampMax = "20"))
	float VibratoRate = 6.2f;

	/** 两组弱共振峰的混合比例；它只制造不同“口型”色彩，不合成可识别音节 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice",
	          meta = (ClampMin = "0", ClampMax = "0.8"))
	float FormantAmount = 0.24f;

	/** 问号前最后三个字逐步上扬的总半音数 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice",
	          meta = (ClampMin = "0", ClampMax = "12"))
	float QuestionRiseSemitones = 5.f;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDeliveryVoiceGlyphRevealed, const FString&, Glyph, int32,
                                             GlyphIndex);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnDeliveryVoiceLineFinished);

/**
 * 本机程序化人物语音：按文本逐字调度短音，同时广播逐字显示事件
 * 
 * 组件挂在 DeliveryPlayerController 上，属于纯本地表现，不复制也不发服务器 RPC
 * 对话系统只同步“说哪一句”，每台客户端自行产生相同的文字节奏和声音
 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryDialogueVoiceComponent : public USynthComponent
{
	GENERATED_BODY()

  public:
	UDeliveryDialogueVoiceComponent(const FObjectInitializer& ObjectInitializer);

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** 从 Controller 或 Pawn 找到语音组件 */
	UFUNCTION(BlueprintPure, Category = "Delivery|Voice",
	          meta = (DisplayName = "Get Delivery Dialogue Voice"))
	static UDeliveryDialogueVoiceComponent* FindVoice(AActor* Actor);

	// 对白播放
	/** 使用组件上的 DefaultSettings 说一句话 */
	UFUNCTION(BlueprintCallable, Category = "Delivery|Voice")
	void SpeakText(const FText& Text);

	/** 使用内置角色预设说一句话，适合第一版电话/NPC 对接 */
	UFUNCTION(BlueprintCallable, Category = "Delivery|Voice")
	void SpeakTextWithPreset(const FText& Text, EDeliveryVoicePreset Preset);

	/** 使用完整参数说一句话；以后 DataAsset 只需要把配置传进这里 */
	UFUNCTION(BlueprintCallable, Category = "Delivery|Voice")
	void SpeakTextWithSettings(const FText& Text, const FDeliveryVoiceSettings& Settings);

	UFUNCTION(BlueprintCallable, Category = "Delivery|Voice")
	void StopSpeaking();

	UFUNCTION(BlueprintPure, Category = "Delivery|Voice")

	bool IsSpeaking() const
	{
		return bSpeaking;
	}

	UFUNCTION(BlueprintPure, Category = "Delivery|Voice")
	static FDeliveryVoiceSettings MakePresetSettings(EDeliveryVoicePreset Preset);

	// 时长估算
	/**
	 * 估算整段台词念完要几秒，和真实播放共用同一条逐字计时规则
	 * 给"通话时长够不够念完"这类校验用：电话到点就挂断、不等念完，
	 * 配短了的表现是 NPC 话没说完就被切断
	 */
	UFUNCTION(BlueprintPure, Category = "Delivery|Voice")
	static float EstimateSpeechSecondsWithSettings(const FText& Text, const FDeliveryVoiceSettings& Settings);

	/** 同上，用 C++ 默认声线；电话对白走 SpeakText，用的就是这一份 */
	UFUNCTION(BlueprintPure, Category = "Delivery|Voice")
	static float EstimateSpeechSeconds(const FText& Text);

	/**
	 * 给定秒数能念到第几个字符（返回念得出来的前缀长度）
	 * 校验时用来指出"会在哪儿被挂断"；和估算共用同一条计时规则，
	 * 所以报出来的位置和实际听到的一致
	 */
	UFUNCTION(BlueprintPure, Category = "Delivery|Voice")
	static int32 SpokenGlyphCountWithin(const FText& Text, float Seconds);

	// 字幕与结束通知
	/** Widget 绑定它来逐字追加文字；Index 是原始字符串里的 UTF-16 下标 */
	UPROPERTY(BlueprintAssignable, Category = "Delivery|Voice")
	FOnDeliveryVoiceGlyphRevealed OnGlyphRevealed;

	UPROPERTY(BlueprintAssignable, Category = "Delivery|Voice")
	FOnDeliveryVoiceLineFinished OnLineFinished;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Voice")
	FDeliveryVoiceSettings DefaultSettings;

  protected:
	virtual ISoundGeneratorPtr CreateSoundGenerator(const FSoundGeneratorInitParams& InParams) override;

  private:
	// 逐字播放处理
	void RevealNextGlyph();
	void TriggerGlyphSound(TCHAR Glyph, int32 GlyphIndex);
	/** 唯一一份逐字计时规则；DelayForGlyph 和 EstimateSpeechSeconds* 都走它 */
	static float GlyphDelaySeconds(const FString& Text, int32 Index, const FDeliveryVoiceSettings& Settings);
	float DelayForGlyph() const;
	bool ShouldSoundGlyph(TCHAR Glyph) const;
	float ProsodySemitoneOffset(int32 GlyphIndex, TCHAR Glyph) const;
	bool IsOwnedByLocalController() const;

	// 本次播放状态
	ISoundGeneratorPtr VoiceGenerator;
	FString ActiveText;
	FDeliveryVoiceSettings ActiveSettings;
	int32 NextGlyphIndex = 0;
	bool bSpeaking = false;
	FTimerHandle GlyphTimer;
};
