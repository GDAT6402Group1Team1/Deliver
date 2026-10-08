// Copyright Epic Games, Inc. All Rights Reserved.

#include "Audio/DeliveryDialogueVoiceComponent.h"

#include "Delivery.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

#if !UE_BUILD_SHIPPING

namespace DeliveryVoiceDebug
{
	UWorld* ResolveWorld(UWorld* World)
	{
		return World ? World : (GEngine ? GEngine->GetCurrentPlayWorld() : nullptr);
	}

	UDeliveryDialogueVoiceComponent* Voice(UWorld* InWorld)
	{
		UWorld* World = ResolveWorld(InWorld);
		APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
		return Controller ? Controller->FindComponentByClass<UDeliveryDialogueVoiceComponent>() : nullptr;
	}

	EDeliveryVoicePreset ParsePreset(const FString& Name)
	{
		if (Name.Equals(TEXT("low"), ESearchCase::IgnoreCase)) return EDeliveryVoicePreset::Low;
		if (Name.Equals(TEXT("high"), ESearchCase::IgnoreCase)) return EDeliveryVoicePreset::High;
		if (Name.Equals(TEXT("robot"), ESearchCase::IgnoreCase)) return EDeliveryVoicePreset::Robot;
		if (Name.Equals(TEXT("angry"), ESearchCase::IgnoreCase)) return EDeliveryVoicePreset::Angry;
		return EDeliveryVoicePreset::Normal;
	}

	void Test(const TArray<FString>& Args, UWorld* World)
	{
		UDeliveryDialogueVoiceComponent* Component = Voice(World);
		if (!Component)
		{
			UE_LOG(LogDelivery, Warning,
				TEXT("[Voice] 找不到本地 DeliveryDialogueVoiceComponent。确认 GameMode 使用 DeliveryPlayerController。"));
			return;
		}

		const EDeliveryVoicePreset Preset = ParsePreset(Args.IsEmpty() ? TEXT("normal") : Args[0]);
		Component->SpeakTextWithPreset(
			FText::FromString(TEXT("你好，这里是闪送快递。你的包裹到了，请问有人在家吗？太好了！")), Preset);
		UE_LOG(LogDelivery, Log, TEXT("[Voice] 开始测试，Preset=%s"),
			*UEnum::GetValueAsString(Preset));
	}

	void Say(const TArray<FString>& Args, UWorld* World)
	{
		UDeliveryDialogueVoiceComponent* Component = Voice(World);
		if (!Component)
		{
			UE_LOG(LogDelivery, Warning, TEXT("[Voice] 找不到本地语音组件"));
			return;
		}

		const FString Text = Args.IsEmpty()
			? TEXT("这是一条自定义电子语音。")
			: FString::Join(Args, TEXT(" "));
		Component->SpeakText(FText::FromString(Text));
		UE_LOG(LogDelivery, Log, TEXT("[Voice] %s"), *Text);
	}

	void Stop(const TArray<FString>&, UWorld* World)
	{
		if (UDeliveryDialogueVoiceComponent* Component = Voice(World))
		{
			Component->StopSpeaking();
			UE_LOG(LogDelivery, Log, TEXT("[Voice] 已停止"));
		}
	}
}

static FAutoConsoleCommandWithWorldAndArgs GDeliveryVoiceTest(
	TEXT("Delivery.Voice.Test"),
	TEXT("播放内置测试句：Delivery.Voice.Test [normal|low|high|robot|angry]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DeliveryVoiceDebug::Test));

static FAutoConsoleCommandWithWorldAndArgs GDeliveryVoiceSay(
	TEXT("Delivery.Voice.Say"),
	TEXT("用普通声线播放任意文本：Delivery.Voice.Say <文字>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DeliveryVoiceDebug::Say));

static FAutoConsoleCommandWithWorldAndArgs GDeliveryVoiceStop(
	TEXT("Delivery.Voice.Stop"),
	TEXT("停止当前电子语音。"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DeliveryVoiceDebug::Stop));

#endif

