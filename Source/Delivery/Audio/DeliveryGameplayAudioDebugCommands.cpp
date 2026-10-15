// Copyright Epic Games, Inc. All Rights Reserved.

#include "Delivery.h"
#include "Components/AudioComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"

#if !UE_BUILD_SHIPPING

namespace DeliveryGameplayAudioDebug
{
	TWeakObjectPtr<UAudioComponent> PreviewComponent;

	UWorld* ResolveWorld(UWorld* World)
	{
		return World ? World : (GEngine ? GEngine->GetCurrentPlayWorld() : nullptr);
	}

	void StopPreview()
	{
		if (UAudioComponent* Component = PreviewComponent.Get())
		{
			Component->Stop();
			Component->DestroyComponent();
		}
		PreviewComponent.Reset();
	}

	const TCHAR* ResolveAssetPath(const FString& Name)
	{
		if (Name.Equals(TEXT("toggle"), ESearchCase::IgnoreCase))
			return TEXT("/Game/Sound/SFX/SFX_Phone_Toggle.SFX_Phone_Toggle");
		if (Name.Equals(TEXT("ring"), ESearchCase::IgnoreCase))
			return TEXT("/Game/Sound/SFX/SFX_Phone_Ring.SFX_Phone_Ring");
		if (Name.Equals(TEXT("answer"), ESearchCase::IgnoreCase))
			return TEXT("/Game/Sound/SFX/SFX_Phone_Answer.SFX_Phone_Answer");
		if (Name.Equals(TEXT("hangup"), ESearchCase::IgnoreCase))
			return TEXT("/Game/Sound/SFX/SFX_Phone_HangUp.SFX_Phone_HangUp");
		if (Name.Equals(TEXT("pickup"), ESearchCase::IgnoreCase))
			return TEXT("/Game/Sound/SFX/SFX_Task_Pickup.SFX_Task_Pickup");
		if (Name.Equals(TEXT("delivered"), ESearchCase::IgnoreCase))
			return TEXT("/Game/Sound/SFX/SFX_Task_Delivered.SFX_Task_Delivered");
		return nullptr;
	}

	void Test(const TArray<FString>& Args, UWorld* InWorld)
	{
		UWorld* World = ResolveWorld(InWorld);
		const FString Name = Args.IsEmpty() ? TEXT("delivered") : Args[0];
		const TCHAR* AssetPath = ResolveAssetPath(Name);
		if (!World || !AssetPath)
		{
			UE_LOG(LogDelivery, Warning,
				TEXT("[Audio] 用法：Delivery.Audio.Test toggle|ring|answer|hangup|pickup|delivered"));
			return;
		}

		USoundBase* Sound = LoadObject<USoundBase>(nullptr, AssetPath);
		if (!Sound)
		{
			UE_LOG(LogDelivery, Warning, TEXT("[Audio] 找不到 %s"), AssetPath);
			return;
		}

		StopPreview();
		PreviewComponent = UGameplayStatics::SpawnSound2D(
			World, Sound, 1.0f, 1.0f, 0.0f, nullptr, false, false);
		UE_LOG(LogDelivery, Log, TEXT("[Audio] 正在试听 %s"), *Name);
	}

	void Stop(const TArray<FString>&, UWorld*)
	{
		StopPreview();
		UE_LOG(LogDelivery, Log, TEXT("[Audio] 已停止试听"));
	}
}

static FAutoConsoleCommandWithWorldAndArgs GDeliveryAudioTest(
	TEXT("Delivery.Audio.Test"),
	TEXT("试听游戏音效：Delivery.Audio.Test toggle|ring|answer|hangup|pickup|delivered"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DeliveryGameplayAudioDebug::Test));

static FAutoConsoleCommandWithWorldAndArgs GDeliveryAudioStop(
	TEXT("Delivery.Audio.Stop"),
	TEXT("停止当前试听音效（尤其是循环来电铃声）。"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DeliveryGameplayAudioDebug::Stop));

#endif
