// Copyright Epic Games, Inc. All Rights Reserved.

#include "Delivery.h"
#include "DeliveryMusicComponent.h"
#include "DeliveryPlayerController.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Sound/SoundBase.h"

#if !UE_BUILD_SHIPPING

/**
 * 音乐管线的验收命令。
 *
 * 音乐的问题大多是"该换轨没换""电话来了没压下去"这类**时序**问题，
 * 而它们在游戏里只表现为"听着怪"，靠耳朵说不清。这几条命令把内部状态打出来，
 * 并且能脱离任务流程直接验证换轨和闪避。
 */

namespace DeliveryMusicDebug
{
	UDeliveryMusicComponent* GetMusic(UWorld* InWorld)
	{
		UWorld* World = InWorld ? InWorld : (GEngine ? GEngine->GetCurrentPlayWorld() : nullptr);
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		ADeliveryPlayerController* DeliveryPC = Cast<ADeliveryPlayerController>(PC);

		if (!DeliveryPC)
		{
			UE_LOG(LogDelivery, Warning,
				TEXT("[音乐] 拿不到 ADeliveryPlayerController。PlayerController 类是不是没设成 BP_DeliveryManPC？"));

			return nullptr;
		}

		return DeliveryPC->GetMusic();
	}

	const TCHAR* TrackName(EDeliveryMusicTrack Track)
	{
		switch (Track)
		{
		case EDeliveryMusicTrack::Explore:    return TEXT("探索");
		case EDeliveryMusicTrack::TaskActive: return TEXT("任务中");
		case EDeliveryMusicTrack::Overdue:    return TEXT("超时");
		default:                              return TEXT("静音");
		}
	}
}

static FAutoConsoleCommandWithWorld GDeliveryMusicDump(
	TEXT("Delivery.Music.Dump"),
	TEXT("打印当前音乐状态：在放哪一轨、三个插槽填了没、是否正在闪避。"),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		UDeliveryMusicComponent* Music = DeliveryMusicDebug::GetMusic(World);
		if (!Music)
		{
			return;
		}

		auto SlotName = [](USoundBase* Sound)
		{
			return Sound ? *Sound->GetName() : TEXT("<空>");
		};

		UE_LOG(LogDelivery, Log, TEXT("================ 音乐状态 ================"));
		UE_LOG(LogDelivery, Log, TEXT("  总开关      : %s"), Music->bMusicEnabled ? TEXT("开") : TEXT("关"));
		UE_LOG(LogDelivery, Log, TEXT("  当前轨      : %s"),
			DeliveryMusicDebug::TrackName(Music->GetCurrentTrack()));
		UE_LOG(LogDelivery, Log, TEXT("  正在闪避    : %s"), Music->IsDucking() ? TEXT("是") : TEXT("否"));
		UE_LOG(LogDelivery, Log, TEXT("  音量/闪避比 : %.2f / %.2f"),
			Music->MusicVolume, Music->CallDuckVolumeScale);
		UE_LOG(LogDelivery, Log, TEXT("  探索轨      : %s"), SlotName(Music->ExploreTrack));
		UE_LOG(LogDelivery, Log, TEXT("  任务轨      : %s"), SlotName(Music->TaskActiveTrack));
		UE_LOG(LogDelivery, Log, TEXT("  超时轨      : %s"), SlotName(Music->OverdueTrack));

		if (!Music->ExploreTrack)
		{
			UE_LOG(LogDelivery, Warning,
				TEXT("[音乐] 探索轨是空的，整套音乐不会出声。"
					 "在 BP_DeliveryManPC 的 Music 组件上填一条——"
					 "另外两条留空会自动退回它，所以只填这一条就能用。"));
		}
		UE_LOG(LogDelivery, Log, TEXT("=========================================="));
	}));

static FAutoConsoleCommandWithWorldAndArgs GDeliveryMusicVolume(
	TEXT("Delivery.Music.Volume"),
	TEXT("设置音乐音量：Delivery.Music.Volume <0~2>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			UDeliveryMusicComponent* Music = DeliveryMusicDebug::GetMusic(World);
			if (!Music || Args.Num() == 0)
			{
				UE_LOG(LogDelivery, Log, TEXT("[音乐] 用法：Delivery.Music.Volume <0~2>"));

				return;
			}

			Music->MusicVolume = FMath::Clamp(FCString::Atof(*Args[0]), 0.f, 2.f);
			UE_LOG(LogDelivery, Log, TEXT("[音乐] 音量 → %.2f"), Music->MusicVolume);
		}));

static FAutoConsoleCommandWithWorldAndArgs GDeliveryMusicDuck(
	TEXT("Delivery.Music.Duck"),
	TEXT("手动闪避几秒，验证压低和恢复的手感：Delivery.Music.Duck [秒数] [比例]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			UDeliveryMusicComponent* Music = DeliveryMusicDebug::GetMusic(World);
			if (!Music)
			{
				return;
			}

			const float Seconds = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 3.f;
			const float Scale = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 0.25f;
			Music->DuckFor(Seconds, Scale);

			UE_LOG(LogDelivery, Log, TEXT("[音乐] 闪避 %.1f 秒，压到 %.0f%%"), Seconds, Scale * 100.f);
		}));

static FAutoConsoleCommandWithWorld GDeliveryMusicToggle(
	TEXT("Delivery.Music.Toggle"),
	TEXT("开关整套背景音乐。"),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		UDeliveryMusicComponent* Music = DeliveryMusicDebug::GetMusic(World);
		if (!Music)
		{
			return;
		}

		Music->bMusicEnabled = !Music->bMusicEnabled;
		Music->RefreshTrack();

		UE_LOG(LogDelivery, Log, TEXT("[音乐] 总开关 → %s"),
			Music->bMusicEnabled ? TEXT("开") : TEXT("关"));
	}));

#endif // !UE_BUILD_SHIPPING
