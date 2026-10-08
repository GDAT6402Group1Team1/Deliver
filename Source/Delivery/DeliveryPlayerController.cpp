// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryPlayerController.h"
#include "Audio/DeliveryDialogueVoiceComponent.h"
#include "Delivery.h"
#include "Components/PostProcessComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Materials/MaterialInterface.h"
#include "Interaction/DeliveryPromptSubsystem.h"
#include "Task/DeliveryPhoneCallQueueComponent.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

ADeliveryPlayerController::ADeliveryPlayerController()
{
	DialogueVoice = CreateDefaultSubobject<UDeliveryDialogueVoiceComponent>(TEXT("DialogueVoice"));

	HighlightPostProcess = CreateDefaultSubobject<UPostProcessComponent>(TEXT("HighlightPostProcess"));
	HighlightPostProcess->bUnbound = true;
	HighlightPostProcess->bEnabled = false;
	HighlightPostProcess->Priority = 100.0f;
	HighlightPostProcess->BlendWeight = 1.0f;

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> HighlightMaterial(
		TEXT("/Game/Blueprint/Interaction/M_GrabHighlight.M_GrabHighlight"));
	if (HighlightMaterial.Succeeded())
	{
		HighlightPostProcess->Settings.AddBlendable(HighlightMaterial.Object, 1.0f);
	}

	static ConstructorHelpers::FObjectFinder<UInputMappingContext> DefaultIMC(TEXT("/Game/Input/IMC_Default"));
	if (DefaultIMC.Succeeded())
	{
		DefaultMappingContexts.Add(DefaultIMC.Object);
	}

	static ConstructorHelpers::FObjectFinder<UInputMappingContext> MouseLookIMC(TEXT("/Game/Input/IMC_MouseLook"));
	if (MouseLookIMC.Succeeded())
	{
		MobileExcludedMappingContexts.Add(MouseLookIMC.Object);
	}

	// J 开关手机。走 BindKey 直接绑，不进 IMC——理由见头文件里 TogglePhoneUI 的注释
	TogglePhoneKey = EKeys::J;

	// 软引用晚绑：IA_Interact 是脚本生成的资产，构造函数只在模块加载时跑一次，
	// 用 ConstructorHelpers 的话新建出来的资产在同一次会话里永远解析不到。
	InteractAction = TSoftObjectPtr<UInputAction>(FSoftObjectPath(TEXT("/Game/Input/Actions/IA_Interact.IA_Interact")));
	InteractKey = EKeys::F;
}

void ADeliveryPlayerController::BeginPlay()
{
	Super::BeginPlay();

	// PlayerController 在服务器上也会为远端玩家存在；只有本机控制器需要渲染后处理。
	if (HighlightPostProcess)
	{
		HighlightPostProcess->bEnabled = IsLocalController();
	}

	BindPhoneVoice();
	if (IsLocalController() && DialogueVoice)
	{
		DialogueVoice->OnGlyphRevealed.AddUniqueDynamic(this,
			&ADeliveryPlayerController::HandlePhoneVoiceGlyphRevealed);
	}
}

void ADeliveryPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(PhoneVoiceBindRetryTimer);
	}

	if (UDeliveryPhoneCallQueueComponent* Phone = UDeliveryPhoneCallQueueComponent::Get(this))
	{
		Phone->OnPhoneStateChanged.RemoveDynamic(this,
			&ADeliveryPlayerController::HandlePhoneVoiceStateChanged);
	}

	if (DialogueVoice)
	{
		DialogueVoice->OnGlyphRevealed.RemoveDynamic(this,
			&ADeliveryPlayerController::HandlePhoneVoiceGlyphRevealed);
		DialogueVoice->StopSpeaking();
	}
	if (UDeliveryPromptSubsystem* Prompt = UDeliveryPromptSubsystem::Get(this))
	{
		Prompt->ClearSubtitle();
	}

	Super::EndPlay(EndPlayReason);
}

void ADeliveryPlayerController::BindPhoneVoice()
{
	if (!IsLocalController())
	{
		return;
	}

	UDeliveryPhoneCallQueueComponent* Phone = UDeliveryPhoneCallQueueComponent::Get(this);
	if (!Phone)
	{
		// 客户端的 GameState/组件可能还没复制到。只在本机重试，不让远端 Controller 做表现。
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(PhoneVoiceBindRetryTimer, this,
				&ADeliveryPlayerController::BindPhoneVoice, 0.5f, false);
		}
		return;
	}

	Phone->OnPhoneStateChanged.AddUniqueDynamic(this,
		&ADeliveryPlayerController::HandlePhoneVoiceStateChanged);

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(PhoneVoiceBindRetryTimer);
	}

	// 绑定发生时电话可能已经响了或已经接通。主动同步一次，避免晚加入/晚复制的客户端没声音。
	FDeliveryPhoneCall CurrentCall;
	if (Phone->GetCurrentCall(CurrentCall))
	{
		HandlePhoneVoiceStateChanged(Phone->GetCallState(), CurrentCall);
	}
}

void ADeliveryPlayerController::HandlePhoneVoiceStateChanged(
	EDeliveryPhoneCallState NewState, const FDeliveryPhoneCall& Call)
{
	if (!IsLocalController() || !DialogueVoice)
	{
		return;
	}

	if (NewState != EDeliveryPhoneCallState::InCall)
	{
		DialogueVoice->StopSpeaking();
		ActiveVoiceCallId = 0;
		ActivePhoneCallerName = FText::GetEmpty();
		ActivePhoneSubtitle.Reset();
		if (UDeliveryPromptSubsystem* Prompt = UDeliveryPromptSubsystem::Get(this))
		{
			Prompt->ClearSubtitle();
		}
		return;
	}

	// OnRep_Queue / OnRep_CallState 可能在同一帧各触发一次，同一通不能从头播放两遍。
	if (Call.CallId <= 0 || Call.CallId == ActiveVoiceCallId)
	{
		return;
	}

	ActiveVoiceCallId = Call.CallId;
	const FDeliveryPhoneCallContent Content =
		UDeliveryPhoneCallQueueComponent::GetCallContent(Call);
	ActivePhoneCallerName = Content.CallerName;
	ActivePhoneSubtitle.Reset();
	if (UDeliveryPromptSubsystem* Prompt = UDeliveryPromptSubsystem::Get(this))
	{
		Prompt->ClearSubtitle();
	}
	DialogueVoice->SpeakText(Content.Dialogue);
}

void ADeliveryPlayerController::HandlePhoneVoiceGlyphRevealed(
	const FString& Glyph, int32 GlyphIndex)
{
	if (!IsLocalController() || ActiveVoiceCallId <= 0)
	{
		return;
	}

	ActivePhoneSubtitle.Append(Glyph);
	if (UDeliveryPromptSubsystem* Prompt = UDeliveryPromptSubsystem::Get(this))
	{
		Prompt->SetSubtitle(ActivePhoneCallerName, FText::FromString(ActivePhoneSubtitle));
	}
}

namespace
{
	bool ContextMapsAction(const UInputMappingContext* Context, const UInputAction* Action)
	{
		if (!Context || !Action)
		{
			return false;
		}
		for (const FEnhancedActionKeyMapping& Mapping : Context->GetMappings())
		{
			if (Mapping.Action == Action)
			{
				return true;
			}
		}
		return false;
	}
}

void ADeliveryPlayerController::EnsureInteractMapping(UEnhancedInputLocalPlayerSubsystem* Subsystem)
{
	UInputAction* Interact = InteractAction.LoadSynchronous();
	if (!Subsystem || !Interact || !InteractKey.IsValid())
	{
		return;
	}

	// 资产里已经有映射就什么都不做——重复映射同一个动作没必要，也省得两套绑定互相干扰。
	for (const UInputMappingContext* Context : DefaultMappingContexts)
	{
		if (ContextMapsAction(Context, Interact))
		{
			return;
		}
	}
	for (const UInputMappingContext* Context : MobileExcludedMappingContexts)
	{
		if (ContextMapsAction(Context, Interact))
		{
			return;
		}
	}

	if (!RuntimeInteractContext)
	{
		RuntimeInteractContext = NewObject<UInputMappingContext>(this, TEXT("RuntimeInteractContext"));
		RuntimeInteractContext->MapKey(Interact, InteractKey);
	}
	Subsystem->AddMappingContext(RuntimeInteractContext, 0);
	UE_LOG(LogDelivery, Log,
		TEXT("IMC 资产里没有 %s 的映射，已在运行时补上 %s 键。"
			 "（正常情况下它应该在 IMC_Default 里，检查那个资产是不是被拉取冲掉了）"),
		*GetNameSafe(Interact), *InteractKey.ToString());
}

void ADeliveryPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (!IsLocalPlayerController())
	{
		return;
	}

	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		for (UInputMappingContext* CurrentContext : DefaultMappingContexts)
		{
			Subsystem->AddMappingContext(CurrentContext, 0);
		}

		for (UInputMappingContext* CurrentContext : MobileExcludedMappingContexts)
		{
			Subsystem->AddMappingContext(CurrentContext, 0);
		}

		EnsureInteractMapping(Subsystem);
	}

	if (InputComponent && TogglePhoneKey.IsValid())
	{
		InputComponent->BindKey(TogglePhoneKey, IE_Pressed, this,
			&ADeliveryPlayerController::HandleTogglePhonePressed);
	}
}

void ADeliveryPlayerController::HandleTogglePhonePressed()
{
	// 纯本机 UI 开关，不用过服务器
	TogglePhoneUI();
}

void ADeliveryPlayerController::RequestAnswerCall()
{
	if (HasAuthority())
	{
		if (UDeliveryPhoneCallQueueComponent* Phone = UDeliveryPhoneCallQueueComponent::Get(this))
		{
			Phone->AnswerCurrentCall();
		}

		return;
	}

	ServerAnswerCall();
}

void ADeliveryPlayerController::ServerAnswerCall_Implementation()
{
	RequestAnswerCall();
}

void ADeliveryPlayerController::RequestHangUpCall()
{
	if (HasAuthority())
	{
		if (UDeliveryPhoneCallQueueComponent* Phone = UDeliveryPhoneCallQueueComponent::Get(this))
		{
			Phone->HangUpCurrentCall();
		}

		return;
	}

	ServerHangUpCall();
}

void ADeliveryPlayerController::ServerHangUpCall_Implementation()
{
	RequestHangUpCall();
}
