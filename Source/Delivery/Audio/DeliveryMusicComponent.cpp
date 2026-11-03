// Copyright Epic Games, Inc. All Rights Reserved.

#include "Audio/DeliveryMusicComponent.h"

#include "Components/AudioComponent.h"
#include "Delivery.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "Phone/DeliveryPhoneCallQueueComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/Tracking/DeliveryTaskTrackerComponent.h"

namespace DeliveryMusic
{
/**
 * 10Hz 足够；音乐决策只看"任务阶段变了没""电话响了没"，都是秒级事件
 * 每帧跑一遍要做好几次组件查找，纯浪费
 * 闪避的插值用的是真实 DeltaTime，不受这个间隔影响
 */
constexpr float TickInterval = 0.1f;
}

UDeliveryMusicComponent::UDeliveryMusicComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = DeliveryMusic::TickInterval;

	// 纯本机表现；音乐是每个玩家自己听的，服务器不参与
	SetIsReplicatedByDefault(false);
}

// 只为本机玩家启用音乐，并选择初始曲目
void UDeliveryMusicComponent::BeginPlay()
{
	Super::BeginPlay();

	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	if (!Controller || !Controller->IsLocalController())
	{
		// 远端 Controller 不该发声；关掉 Tick 省得每 0.1 秒白跑一次
		SetComponentTickEnabled(false);

		return;
	}

	RefreshTrack();
}

// 停止并销毁当前和正在淡出的音乐组件
void UDeliveryMusicComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 音频组件是 bAutoDestroy=假 建的，必须自己收——否则关卡切换时
	// 音乐会继续放，下一张图再起一条，叠成两轨
	for (UAudioComponent* Audio : {ActiveAudio.Get(), FadingAudio.Get()})
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

// 判断是否正在响铃或通话，用于降低背景音乐音量
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

// 根据任务选择音乐

// 根据追踪任务的状态选择探索、配送或超时曲目
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

// 取得曲目资源，未配置时尝试使用较基础的曲目
USoundBase* UDeliveryMusicComponent::ResolveTrackSound(EDeliveryMusicTrack Track) const
{
	// 超时曲目未配置时尝试配送曲目，再尝试探索曲目
	switch (Track)
	{
	case EDeliveryMusicTrack::Overdue:
		if (OverdueTrack)
			return OverdueTrack;
		// 继续尝试下一种可用曲目

	case EDeliveryMusicTrack::TaskActive:
		if (TaskActiveTrack)
			return TaskActiveTrack;
		// 继续尝试下一种可用曲目

	case EDeliveryMusicTrack::Explore:
		return ExploreTrack;

	default:
		return nullptr;
	}
}

// 音乐播放与切换

// 创建由本组件管理生命周期的二维音乐播放组件
UAudioComponent* UDeliveryMusicComponent::SpawnMusicAudio(USoundBase* Sound)
{
	if (!Sound || !GetWorld())
	{
		return nullptr;
	}

	// bAutoDestroy=假：要留着句柄做淡出和换轨，自动销毁就抓不住了
	// 代价是 EndPlay 必须自己收，上面做了
	// 起始音量直接给到位，不要给 0 等 Tick 补——中间那一帧会是静音
	UAudioComponent* Audio = UGameplayStatics::SpawnSound2D(
	    this, Sound, /* 音量= */MusicVolume * DuckScale, /* 音高= */1.f,
	    /* 起播时间= */0.f, /* 并发设置= */nullptr,
	    /* 跨关卡保留= */false, /* 自动销毁= */false);

	if (!Audio)
	{
		return nullptr;
	}

	// 标成 UI 音：不受时间膨胀和暂停影响；暂停菜单里音乐断掉会显得像卡死
	Audio->bIsUISound = bKeepPlayingWhilePaused;

	return Audio;
}

// 切换曲目，让旧曲目淡出、新曲目淡入
void UDeliveryMusicComponent::StartTrack(EDeliveryMusicTrack Track)
{
	USoundBase* Sound = ResolveTrackSound(Track);

	// 不同任务状态可能使用同一首音乐，此时保留播放进度
	if (ActiveAudio && ActiveAudio->GetSound() == Sound)
	{
		CurrentTrack = Track;

		return;
	}

	// 清理上一次淡出的音乐，避免连续切换时多首音乐叠加
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
		// 淡入目标设为完整音量，实际音量由 SetVolumeMultiplier 控制
		// 不要再次乘 MusicVolume，否则音量会重复降低
		ActiveAudio->FadeIn(CrossfadeSeconds, 1.f);
	}
}

// 根据当前任务状态重新选择曲目
void UDeliveryMusicComponent::RefreshTrack()
{
	StartTrack(EvaluateTrack());
}

// 对白期间降低音乐音量

// 在指定时间内降低音乐音量
void UDeliveryMusicComponent::DuckFor(float Seconds, float VolumeScale)
{
	if (Seconds <= 0.f)
	{
		return;
	}

	ManualDuckScale = FMath::Clamp(VolumeScale, 0.f, 1.f);
	ManualDuckUntil = GetWorld() ? GetWorld()->GetTimeSeconds() + Seconds : 0.0;
}

// 更新曲目和音量，并清理已经淡出结束的音乐
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
