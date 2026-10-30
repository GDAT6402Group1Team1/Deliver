// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryPlayerController.h"
#include "Blueprint/UserWidget.h"
#include "Audio/DeliveryDialogueVoiceComponent.h"
#include "Audio/DeliveryMusicComponent.h"
#include "Components/AudioComponent.h"
#include "Delivery.h"
#include "Components/PostProcessComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Materials/MaterialInterface.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "Interaction/DeliveryPromptSubsystem.h"
#include "Task/DeliveryPhoneCallQueueComponent.h"
#include "Task/DeliveryGroundArrowComponent.h"
#include "Task/DeliveryObjectiveRingComponent.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

ADeliveryPlayerController::ADeliveryPlayerController()
{
	DialogueVoice = CreateDefaultSubobject<UDeliveryDialogueVoiceComponent>(TEXT("DialogueVoice"));
	GroundArrow = CreateDefaultSubobject<UDeliveryGroundArrowComponent>(TEXT("GroundArrow"));
	ObjectiveRing = CreateDefaultSubobject<UDeliveryObjectiveRingComponent>(TEXT("ObjectiveRing"));

	Music = CreateDefaultSubobject<UDeliveryMusicComponent>(TEXT("Music"));

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
	StopIncomingCallAudio();
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
	if (!IsLocalController())
	{
		return;
	}

	// 状态复制可能让同一状态回调多次，只在真正的状态边沿播放一次操作音。
	if (!bPhoneAudioStateInitialized)
	{
		bPhoneAudioStateInitialized = true;
		LastPhoneAudioState = NewState;
		if (NewState == EDeliveryPhoneCallState::Ringing && IncomingCallSound)
		{
			IncomingCallAudio = UGameplayStatics::SpawnSound2D(
				this, IncomingCallSound, 1.0f, 1.0f, 0.0f, nullptr, false, false);
		}
	}
	else if (NewState != LastPhoneAudioState)
	{
		const EDeliveryPhoneCallState PreviousState = LastPhoneAudioState;
		LastPhoneAudioState = NewState;

		if (NewState == EDeliveryPhoneCallState::Ringing)
		{
			StopIncomingCallAudio();
			if (IncomingCallSound)
			{
				IncomingCallAudio = UGameplayStatics::SpawnSound2D(
					this, IncomingCallSound, 1.0f, 1.0f, 0.0f, nullptr, false, false);
			}
		}
		else
		{
			StopIncomingCallAudio();
			if (NewState == EDeliveryPhoneCallState::InCall && AnswerCallSound)
			{
				UGameplayStatics::PlaySound2D(this, AnswerCallSound);
			}
			else if (NewState == EDeliveryPhoneCallState::Idle
				&& PreviousState != EDeliveryPhoneCallState::Idle && HangUpCallSound)
			{
				UGameplayStatics::PlaySound2D(this, HangUpCallSound);
			}
		}
	}

	if (NewState != EDeliveryPhoneCallState::InCall)
	{
		if (DialogueVoice)
		{
			DialogueVoice->StopSpeaking();
		}
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
	if (DialogueVoice)
	{
		DialogueVoice->SpeakText(Content.Dialogue);
	}
}

void ADeliveryPlayerController::StopIncomingCallAudio()
{
	if (IncomingCallAudio)
	{
		IncomingCallAudio->Stop();
		IncomingCallAudio->DestroyComponent();
		IncomingCallAudio = nullptr;
	}
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

// 手机面板在 OpenUIPanels 里的名字。名字起得长是刻意的：Unity Build 会把多个 cpp
// 合并成一个翻译单元，短名常量（PanelId、RetrySeconds 之类）在本项目已经撞过一次。
static const FName DeliveryPhoneUIPanelId(TEXT("DeliveryPhoneUI"));

void ADeliveryPlayerController::HandleTogglePhonePressed()
{
	// 纯本机 UI 开关，不用过服务器
	if (PhoneToggleSound)
	{
		UGameplayStatics::PlaySound2D(this, PhoneToggleSound);
	}

	// **状态由 C++ 翻，光标跟着状态走。** 蓝图那边的 TogglePhoneUI 只负责把面板
	// 加进/移出视口，不许碰 bShowMouseCursor——两边各自取反就是失同步的来源。
	bPhoneUIOpen = !bPhoneUIOpen;
	SetUIPanelOpen(DeliveryPhoneUIPanelId, bPhoneUIOpen);

	TogglePhoneUI();
}

void ADeliveryPlayerController::SetUIPanelOpen(FName PanelId, bool bOpen)
{
	if (PanelId.IsNone())
	{
		return;
	}
	if (PanelId == DeliveryPhoneUIPanelId)
	{
		bPhoneUIOpen = bOpen;
	}

	const int32 Before = OpenUIPanels.Num();
	if (bOpen)
	{
		OpenUIPanels.Add(PanelId);
	}
	else
	{
		OpenUIPanels.Remove(PanelId);
	}

	// 值没变就别重复设输入模式：SetInputMode 会动焦点，每帧调一次会把 UI 里的
	// 输入框焦点抢掉。只在"有没有面板"这件事真的翻转时才应用。
	const bool bHadAny = Before > 0;
	const bool bHasAny = OpenUIPanels.Num() > 0;
	if (bHadAny != bHasAny)
	{
		ApplyUIInputMode();
	}
}

void ADeliveryPlayerController::ReportPhoneUIOpen(UUserWidget* PhoneWidget, bool bOpen)
{
	ADeliveryPlayerController* Controller = PhoneWidget
		? Cast<ADeliveryPlayerController>(PhoneWidget->GetOwningPlayer()) : nullptr;
	if (Controller)
	{
		Controller->SetUIPanelOpen(DeliveryPhoneUIPanelId, bOpen);
	}
}

bool ADeliveryPlayerController::IsAnyUIPanelOpen() const
{
	return OpenUIPanels.Num() > 0;
}

void ADeliveryPlayerController::ApplyUIInputMode()
{
	// 只有本机控制器有光标和输入模式这回事；服务器上那几个代理控制器碰了没意义。
	if (!IsLocalController())
	{
		return;
	}

	const bool bWantCursor = IsAnyUIPanelOpen();
	bShowMouseCursor = bWantCursor;

	if (bWantCursor)
	{
		FInputModeGameAndUI Mode;
		// 不锁鼠标到视口：面板开着时玩家要能把指针移到任何地方点。
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		// 捕获期间也别藏光标，否则按住左键拖动时指针消失、看着像卡住。
		Mode.SetHideCursorDuringCapture(false);
		SetInputMode(Mode);
	}
	else
	{
		// 回到纯游戏：鼠标移动全部交给视角，不再被光标吃掉。
		SetInputMode(FInputModeGameOnly());
	}
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
