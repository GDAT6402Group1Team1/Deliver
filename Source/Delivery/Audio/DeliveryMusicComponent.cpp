// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryMusicComponent.h"

#include "Components/AudioComponent.h"
#include "Delivery.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "Task/DeliveryPhoneCallQueueComponent.h"
#include "Task/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/DeliveryTaskTrackerComponent.h"

namespace DeliveryMusic
{
	/**
	 * 10Hz 足够。音乐决策只看"任务阶段变了没""电话响了没"，都是秒级事件；
	 * 每帧跑一遍要做好几次组件查找，纯浪费。
	 * 闪避的插值用的是真实 DeltaTime，不受这个间隔影响。
	 */
	constexpr float TickInterval = 0.1f;
}

UDeliveryMusicComponent::UDeliveryMusicComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = DeliveryMusic::TickInterval;

	// 纯本机表现。音乐是每个玩家自己听的，服务器不参与
	SetIsReplicatedByDefault(false);
}

void UDeliveryMusicComponent::BeginPlay()
{
	Super::BeginPlay();

	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	if (!Controller || !Controller->IsLocalController())
	{
		// 远端 Controller 不该发声。关掉 Tick 省得每 0.1 秒白跑一次
		SetComponentTickEnabled(false);

		return;
	}

	RefreshTrack();
}

void UDeliveryMusicComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 音频组件是 bAutoDestroy=false 建的，必须自己收——否则关卡切换时
	// 音乐会继续放，下一张图再起一条，叠成两轨
	for (UAudioComponent* Audio : { ActiveAudio.Get(), FadingAudio.Get() })
	{
		if (Audio)
		{
			Audio->Stop();
			Audio->DestroyComponent();
		}
	}
	ActiveAudio = nullptr;
	FadingAudio = nullptr;

	Super::EndPlay(EndPlayReason);
}

bool UDeliveryMusicComponent::IsPhoneBusy() const
{
	const UDeliveryPhoneCallQueueComponent* Phone = UDeliveryPhoneCallQueueComponent::Get(this);
	if (!Phone)
	{
		return false;
	}

	const EDeliveryPhoneCallState State = Phone->GetCallState();

	// 响铃也要压：铃声本身就该盖过音乐，而且玩家这时要做"接不接"的决定
	return State == EDeliveryPhoneCallState::Ringing || State == EDeliveryPhoneCallState::InCall;
}

EDeliveryMusicTrack UDeliveryMusicComponent::EvaluateTrack() const
{
	if (!bMusicEnabled)
	{
		return EDeliveryMusicTrack::None;
	}

	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	const APlayerState* State = Controller ? Controller->PlayerState : nullptr;
	const UDeliveryTaskTrackerComponent* Tracker =
		State ? State->FindComponentByClass<UDeliveryTaskTrackerComponent>() : nullptr;
	const UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);

	if (!Tracker || !Manager)
	{
		return EDeliveryMusicTrack::Explore;
	}

	const UDeliveryTaskDefinition* Task = Tracker->GetTrackedTask();
	if (!Task || Manager->GetTaskStatus(Task) != EDeliveryTaskStatus::InProgress)
	{
		// 待取件也算探索：那段路和平时跑图没区别，没必要换气氛
		return EDeliveryMusicTrack::Explore;
	}

	const FDeliveryTaskTimeSnapshot Time = Manager->GetTimeSnapshot(Task);

	return Time.bOverdue ? EDeliveryMusicTrack::Overdue : EDeliveryMusicTrack::TaskActive;
}

USoundBase* UDeliveryMusicComponent::ResolveTrackSound(EDeliveryMusicTrack Track) const
{
	// 逐级回退：只填了探索轨也能用，不会出现"切到任务轨结果没声音"。
	// 这比"没填就静音"好——静音在游戏里和"音乐坏了"分不出来
	switch (Track)
	{
	case EDeliveryMusicTrack::Overdue:
		if (OverdueTrack) return OverdueTrack;
		// fallthrough

	case EDeliveryMusicTrack::TaskActive:
		if (TaskActiveTrack) return TaskActiveTrack;
		// fallthrough

	case EDeliveryMusicTrack::Explore:
		return ExploreTrack;

	default:
		return nullptr;
	}
}

UAudioComponent* UDeliveryMusicComponent::SpawnMusicAudio(USoundBase* Sound)
{
	if (!Sound || !GetWorld())
	{
		return nullptr;
	}

	// bAutoDestroy=false：要留着句柄做淡出和换轨，自动销毁就抓不住了。
	// 代价是 EndPlay 必须自己收，上面做了。
	// 起始音量直接给到位，不要给 0 等 Tick 补——中间那一帧会是静音
	UAudioComponent* Audio = UGameplayStatics::SpawnSound2D(
		this, Sound, /*VolumeMultiplier=*/MusicVolume * DuckScale, /*PitchMultiplier=*/1.f,
		/*StartTime=*/0.f, /*ConcurrencySettings=*/nullptr,
		/*bPersistAcrossLevelTransition=*/false, /*bAutoDestroy=*/false);

	if (!Audio)
	{
		return nullptr;
	}

	// 标成 UI 音：不受时间膨胀和暂停影响。暂停菜单里音乐断掉会显得像卡死
	Audio->bIsUISound = bKeepPlayingWhilePaused;

	return Audio;
}

void UDeliveryMusicComponent::StartTrack(EDeliveryMusicTrack Track)
{
	USoundBase* Sound = ResolveTrackSound(Track);

	// 回退之后可能和现在放的是同一个资产。这时候**不要重起**——
	// 换轨会把音乐从头播，而玩家听到的是"任务一进行中音乐就跳回开头"
	if (ActiveAudio && ActiveAudio->GetSound() == Sound)
	{
		CurrentTrack = Track;

		return;
	}

	// 上一条还在淡出就直接收掉：同时存在三条音轨没有意义，只会糊成一片
	if (FadingAudio)
	{
		FadingAudio->Stop();
		FadingAudio->DestroyComponent();
		FadingAudio = nullptr;
	}

	if (ActiveAudio)
	{
		FadingAudio = ActiveAudio;
		FadingAudio->FadeOut(CrossfadeSeconds, 0.f);
		ActiveAudio = nullptr;
	}

	CurrentTrack = Sound ? Track : EDeliveryMusicTrack::None;

	if (!Sound)
	{
		return;
	}

	ActiveAudio = SpawnMusicAudio(Sound);
	if (ActiveAudio)
	{
		// **FadeIn 的第二个参数是"占组件已计算音量的百分比"，不是绝对音量**
		// （引擎注释：The percentage of the AudioComponent's calculated volume to fade to）。
		// 这里必须传 1：实际音量由每帧的 SetVolumeMultiplier 决定，
		// 淡入淡出只负责 0→1 的包络。两边都传 MusicVolume 的话会乘两次——
		// 0.45 × 0.45 ≈ 0.20，听上去就是"进游戏比 Content Browser 里小一半"。
		ActiveAudio->FadeIn(CrossfadeSeconds, 1.f);
	}
}

void UDeliveryMusicComponent::RefreshTrack()
{
	StartTrack(EvaluateTrack());
}

void UDeliveryMusicComponent::DuckFor(float Seconds, float VolumeScale)
{
	if (Seconds <= 0.f)
	{
		return;
	}

	ManualDuckScale = FMath::Clamp(VolumeScale, 0.f, 1.f);
	ManualDuckUntil = GetWorld() ? GetWorld()->GetTimeSeconds() + Seconds : 0.0;
}

void UDeliveryMusicComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// --- 该放哪一轨 ---
	const EDeliveryMusicTrack Desired = EvaluateTrack();
	if (Desired != CurrentTrack)
	{
		StartTrack(Desired);
	}

	// --- 闪避 ---
	const bool bManualDucking = ManualDuckUntil > World->GetTimeSeconds();
	bDucking = IsPhoneBusy() || bManualDucking;

	TargetDuckScale = 1.f;
	if (IsPhoneBusy())
	{
		TargetDuckScale = FMath::Min(TargetDuckScale, CallDuckVolumeScale);
	}
	if (bManualDucking)
	{
		// 两种闪避同时存在时取更低的那个，而不是相乘——
		// 相乘会压得过头，过场 + 来电能叠到几乎听不见
		TargetDuckScale = FMath::Min(TargetDuckScale, ManualDuckScale);
	}

	DuckScale = FMath::FInterpTo(DuckScale, TargetDuckScale, DeltaTime, DuckInterpSpeed);

	// --- 应用音量 ---
	if (ActiveAudio && ActiveAudio->IsPlaying())
	{
		ActiveAudio->SetVolumeMultiplier(MusicVolume * DuckScale);
	}

	// 淡出完的组件自己收掉，不然会攒着一堆已停止的组件
	if (FadingAudio && !FadingAudio->IsPlaying())
	{
		FadingAudio->DestroyComponent();
		FadingAudio = nullptr;
	}
}
