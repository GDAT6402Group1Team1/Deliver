// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliveryMusicComponent.generated.h"

class UAudioComponent;
class USoundBase;

/** 当前该放哪一轨。顺序即优先级，排后面的盖前面的。 */
UENUM(BlueprintType)
enum class EDeliveryMusicTrack : uint8
{
	/** 什么都不放（比如整套音乐被关掉）。 */
	None UMETA(DisplayName="静音"),

	/** 平时跑图。没接任务、或者任务还没进行中。 */
	Explore UMETA(DisplayName="探索"),

	/** 有任务在计时。留空时自动退回探索轨。 */
	TaskActive UMETA(DisplayName="任务中"),

	/** 已超时。留空时退回任务轨、再退回探索轨。 */
	Overdue UMETA(DisplayName="超时")
};

/**
 * 背景音乐。挂在 PlayerController 上。
 *
 * **为什么挂控制器而不是角色**：骑摩托车时控制器去 Possess 了车，角色那具身体
 * 的 `IsLocallyControlled()` 变成 false——挂角色上的话音乐会在上车瞬间断掉。
 * 和对白语音组件、顶部任务条是同一个理由，放在同一个位置。
 *
 * **纯本机表现，不复制。** 音乐是每个玩家自己听的，多人下各客户端按各自的
 * 任务状态独立选轨；服务器不参与。
 *
 * —— 和项目里其他音频的关系 ——
 *
 * 游戏事件音效是"`USoundBase` 插槽放在真正拥有事件的类上、不做全局管理器"
 * （见 Document/AudioSystem.md）。音乐沿用同一个约定：轨道也是插槽，填在
 * `BP_DeliveryManPC` 上，C++ 只管什么时候放、怎么过渡。
 *
 * **唯一的例外是闪避（ducking）**：电话响铃/通话期间必须把音乐压下去，
 * 否则那套程序合成的电子人声（细碎方波、频段窄、响度低）会被整个盖住，
 * 而台词是任务信息的载体。这件事没法让各个插槽自己管，所以放在这里。
 *
 * 闪避是直接乘在音频组件的音量上做的，**没有引入 SoundClass/Submix 资产**：
 * 那套东西真正的用处是给玩家做"音乐/音效分开调"的设置项，等有设置菜单了再加，
 * 现在加只是多两个没人引用的资产。
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryMusicComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UDeliveryMusicComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	/** —— 轨道插槽，填在 BP_DeliveryManPC 上 —— */

	/** 平时跑图。只填这一条就能用，下面两条留空会自动退回这里。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Music|轨道")
	TObjectPtr<USoundBase> ExploreTrack;

	/** 有任务在计时时切到这条。留空 = 继续放探索轨。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Music|轨道")
	TObjectPtr<USoundBase> TaskActiveTrack;

	/** 任务超时后切到这条。留空 = 退回任务轨。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Music|轨道")
	TObjectPtr<USoundBase> OverdueTrack;

	/** —— 调音 —— */

	/**
	 * 音乐总音量。玩家设置项接进来之前，改这里就是改音乐大小。
	 *
	 * 默认 0.45 是刻意的：BGM 要坐在音效和人声底下。**所以进游戏听着一定比
	 * Content Browser 里的预览小**（预览是 1.0），这不是 bug。嫌小就往上调。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Music|调音",
		meta=(ClampMin="0.0", ClampMax="2.0"))
	float MusicVolume = 0.45f;

	/**
	 * 电话期间音乐压到原音量的几成。
	 *
	 * 0.25 大约是 −12 dB。别调太高：电子人声是窄频段的细碎方波，
	 * 压得不够的话台词听不清，而台词里有任务信息。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Music|调音",
		meta=(ClampMin="0.0", ClampMax="1.0"))
	float CallDuckVolumeScale = 0.25f;

	/** 闪避淡入淡出速度（每秒变化的比例）。太快会"啪"一下，太慢压不住开头那句。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Music|调音",
		meta=(ClampMin="0.1"))
	float DuckInterpSpeed = 4.f;

	/** 换轨时的交叉淡入淡出时长。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Music|调音",
		meta=(ClampMin="0.0", Units="s"))
	float CrossfadeSeconds = 2.f;

	/**
	 * 游戏暂停时音乐是否继续放。
	 *
	 * 默认开：暂停菜单里音乐断掉会显得像卡死了。实现上是把音频组件标成 UI 音，
	 * 这样它不受时间膨胀和暂停影响。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Music|调音")
	bool bKeepPlayingWhilePaused = true;

	/** 整套音乐开关。关掉会淡出并停掉，不是静音——持续解码一条没人听的音轨没有意义。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Music")
	bool bMusicEnabled = true;

	/** —— 运行时接口 —— */

	/** 当前在放哪一轨。调试和 UI 用。 */
	UFUNCTION(BlueprintPure, Category="Delivery|Music")
	EDeliveryMusicTrack GetCurrentTrack() const { return CurrentTrack; }

	/** 当前是否处于电话闪避状态。 */
	UFUNCTION(BlueprintPure, Category="Delivery|Music")
	bool IsDucking() const { return bDucking; }

	/**
	 * 临时压低音乐，`Seconds` 秒后自动恢复。过场、演出、重要提示用。
	 * 电话闪避走的是自动判断，不需要调这个。
	 */
	UFUNCTION(BlueprintCallable, Category="Delivery|Music")
	void DuckFor(float Seconds, float VolumeScale = 0.25f);

	/** 立刻重新评估该放哪一轨。换了轨道插槽之后调一次就能当场听到。 */
	UFUNCTION(BlueprintCallable, Category="Delivery|Music")
	void RefreshTrack();

protected:

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:

	/** 按任务/电话状态算出此刻该放哪一轨。 */
	EDeliveryMusicTrack EvaluateTrack() const;

	/** 轨道枚举 → 实际资产。填空的轨会按 超时→任务→探索 依次回退。 */
	USoundBase* ResolveTrackSound(EDeliveryMusicTrack Track) const;

	/** 切到目标轨：新起一个音频组件淡入，旧的淡出。 */
	void StartTrack(EDeliveryMusicTrack Track);

	/** 新建一个播音乐用的音频组件。失败返回 nullptr。 */
	UAudioComponent* SpawnMusicAudio(USoundBase* Sound);

	/** 电话是不是正在响或通话中。 */
	bool IsPhoneBusy() const;

	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> ActiveAudio;

	/** 正在淡出的上一轨。交叉淡入淡出期间两个同时存在。 */
	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> FadingAudio;

	EDeliveryMusicTrack CurrentTrack = EDeliveryMusicTrack::None;

	/** 闪避系数的当前值和目标值，每帧向目标插值。 */
	float DuckScale = 1.f;
	float TargetDuckScale = 1.f;
	bool bDucking = false;

	/** DuckFor 的到期时间（世界时间）。<= 0 表示没有手动闪避。 */
	double ManualDuckUntil = 0.0;
	float ManualDuckScale = 1.f;
};
