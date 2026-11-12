// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/DeliveryPlayerController.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "Game/DeliveryStartupFlow.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Blueprint/UserWidget.h"
#include "Audio/DeliveryDialogueVoiceComponent.h"
#include "Audio/DeliveryMusicComponent.h"
#include "Delivery.h"
#include "Components/PostProcessComponent.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedInputComponent.h"
#include "InputKeyEventArgs.h"
#include "UI/DeliveryHintComponent.h"
#include "UI/DeliveryHUD.h"
#include "UI/Controllers/DeliveryPhoneController.h"
#include "Audio/DeliveryPhoneAudioComponent.h"
#include "Engine/LocalPlayer.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Materials/MaterialInterface.h"
#include "Phone/DeliveryPhoneCallQueueComponent.h"
#include "Task/Tracking/DeliveryGroundArrowComponent.h"
#include "Task/Tracking/DeliveryObjectiveRingComponent.h"
#include "Task/DeliverySettlementComponent.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

ADeliveryPlayerController::ADeliveryPlayerController()
{
    ResidentConversation=CreateDefaultSubobject<UDeliveryConversationComponent>(TEXT("ResidentConversation"));
    DialogueVoice = CreateDefaultSubobject<UDeliveryDialogueVoiceComponent>(TEXT("DialogueVoice"));
    GroundArrow = CreateDefaultSubobject<UDeliveryGroundArrowComponent>(TEXT("GroundArrow"));
    ObjectiveRing = CreateDefaultSubobject<UDeliveryObjectiveRingComponent>(TEXT("ObjectiveRing"));
    Settlement = CreateDefaultSubobject<UDeliverySettlementComponent>(TEXT("Settlement"));

    Hints = CreateDefaultSubobject<UDeliveryHintComponent>(TEXT("Hints"));

    PhoneAudio = CreateDefaultSubobject<UDeliveryPhoneAudioComponent>(TEXT("PhoneAudio"));

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

    static ConstructorHelpers::FObjectFinder<UInputMappingContext> DefaultIMC(
        TEXT("/Game/Input/IMC_Default"));
    if (DefaultIMC.Succeeded())
    {
        DefaultMappingContexts.Add(DefaultIMC.Object);
    }

    static ConstructorHelpers::FObjectFinder<UInputMappingContext> MouseLookIMC(
        TEXT("/Game/Input/IMC_MouseLook"));
    if (MouseLookIMC.Succeeded())
    {
        MobileExcludedMappingContexts.Add(MouseLookIMC.Object);
    }

    TogglePhoneAction = TSoftObjectPtr<UInputAction>(FSoftObjectPath(TEXT("/Game/Input/Actions/IA_TogglePhone.IA_TogglePhone")));

    // 软引用晚绑：IA_Interact 是脚本生成的资产，构造函数只在模块加载时跑一次，
    // 用 ConstructorHelpers 的话新建出来的资产在同一次会话里永远解析不到
    InteractAction =
        TSoftObjectPtr<UInputAction>(FSoftObjectPath(TEXT("/Game/Input/Actions/IA_Interact.IA_Interact")));
    InteractKey = EKeys::F;
}

void ADeliveryPlayerController::ServerReportStartupReady_Implementation(ADeliveryStartupFlow* Flow)
{
    if (Flow && Flow == ADeliveryStartupFlow::GetDeliveryStartupFlow(this))
        Flow->ReportPlayerReady(this);
}

// 本机启用交互物高亮，界面和电话由各自处理器管理
void ADeliveryPlayerController::BeginPlay()
{
    Super::BeginPlay();
    if (HighlightPostProcess) HighlightPostProcess->bEnabled = IsLocalController();
}

namespace
{
bool ContextMapsAction(const UInputMappingContext *Context, const UInputAction *Action)
{
    if (!Context || !Action)
    {
        return false;
    }
    for (const FEnhancedActionKeyMapping &Mapping : Context->GetMappings())
    {
        if (Mapping.Action == Action)
        {
            return true;
        }
    }
    return false;
}
} // namespace

// 现有输入配置缺少交互操作时补充映射
void ADeliveryPlayerController::EnsureInteractMapping(UEnhancedInputLocalPlayerSubsystem *Subsystem)
{
    UInputAction *Interact = InteractAction.LoadSynchronous();
    if (!Subsystem || !Interact || !InteractKey.IsValid())
    {
        return;
    }

    // 资产里已经有映射就什么都不做——重复映射同一个动作没必要，也省得两套绑定互相干扰
    for (const UInputMappingContext *Context : DefaultMappingContexts)
    {
        if (ContextMapsAction(Context, Interact))
        {
            return;
        }
    }
    for (const UInputMappingContext *Context : MobileExcludedMappingContexts)
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

// 控制器输入

// 绑定手机、交互和本机界面相关按键
void ADeliveryPlayerController::SetupInputComponent()
{
    Super::SetupInputComponent();

    if (!IsLocalPlayerController())
    {
        return;
    }

    if (UEnhancedInputLocalPlayerSubsystem *Subsystem =
            ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
    {
        for (UInputMappingContext *CurrentContext : DefaultMappingContexts)
        {
            Subsystem->AddMappingContext(CurrentContext, 0);
        }

        for (UInputMappingContext *CurrentContext : MobileExcludedMappingContexts)
        {
            Subsystem->AddMappingContext(CurrentContext, 0);
        }

        EnsureInteractMapping(Subsystem);
    }

    if (auto* EnhancedInput = Cast<UEnhancedInputComponent>(InputComponent))
    {
        if (UInputAction* Action = TogglePhoneAction.LoadSynchronous())
        {
            EnhancedInput->BindAction(Action, ETriggerEvent::Started, this,
                                      &ADeliveryPlayerController::HandleTogglePhonePressed);
        }
    }
    Hints->BindInput();
    ResidentConversation->BindInput();
}

// 手机界面操作转发

UDeliveryPhoneController* ADeliveryPlayerController::GetPhoneController() const
{
    auto* HUD = Cast<ADeliveryHUD>(GetHUD());
    return HUD ? HUD->GetPhoneController() : nullptr;
}

// 输入只转交操作，手机开关规则由处理器决定
void ADeliveryPlayerController::HandleTogglePhonePressed()
{
    if (auto* Phone = GetPhoneController()) Phone->Toggle();
}

// 保留现有蓝图入口，面板状态统一交给 HUD
void ADeliveryPlayerController::SetUIPanelOpen(FName PanelId, bool bOpen)
{
    if (auto* HUD = Cast<ADeliveryHUD>(GetHUD())) HUD->SetPanelOpen(PanelId, bOpen);
}

void ADeliveryPlayerController::ReportPhoneUIOpen(UUserWidget* PhoneWidget, bool bOpen)
{
    auto* Player = PhoneWidget ? Cast<ADeliveryPlayerController>(PhoneWidget->GetOwningPlayer()) : nullptr;
    if (Player)
        if (auto* Phone = Player->GetPhoneController()) Phone->SetOpen(bOpen);
}

bool ADeliveryPlayerController::IsAnyUIPanelOpen() const
{
    const auto* HUD = Cast<ADeliveryHUD>(GetHUD());
    return HUD && HUD->IsAnyPanelOpen();
}

bool ADeliveryPlayerController::IsPhoneUIOpen() const
{
    const auto* HUD = Cast<ADeliveryHUD>(GetHUD());
    return HUD && HUD->IsPanelOpen(UDeliveryPhoneController::PanelId);
}

// 接听与挂断请求

// 通过服务器入口请求接听共享来电
void ADeliveryPlayerController::RequestAnswerCall()
{
    if (HasAuthority())
    {
        if (UDeliveryPhoneCallQueueComponent *Phone = UDeliveryPhoneCallQueueComponent::Get(this))
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

// 通过服务器入口请求拒接或挂断共享来电
void ADeliveryPlayerController::RequestHangUpCall()
{
    if (HasAuthority())
    {
        if (UDeliveryPhoneCallQueueComponent *Phone = UDeliveryPhoneCallQueueComponent::Get(this))
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

// 角色交互的服务器入口

// 确认请求者控制角色或其车辆，再转交角色交互组件
void ADeliveryPlayerController::ServerRequestInteraction_Implementation(ADeliveryCharacter *ActingCharacter,
                                                                        EDeliveryInteractionRequest Request,
                                                                        AActor *Target)
{
    if (!IsValid(ActingCharacter) || ActingCharacter->GetWorld() != GetWorld())
        return;
    const ADeliveryMotorbike *Vehicle = Cast<ADeliveryMotorbike>(GetPawn());
    // 只能操作自己当前控制的角色，或自己车辆上的驾驶员
    const bool bControlsCharacter = GetPawn() == ActingCharacter;
    const bool bControlsDriver =
        Vehicle && Vehicle->GetDriver() == ActingCharacter && ActingCharacter->GetCurrentVehicle() == Vehicle;
    if (bControlsCharacter || bControlsDriver)
    {
        ActingCharacter->GetInteractionComponent()->HandleRequest(Request, Target);
    }
}

// 只在实际使用另一种设备时更新键名，忽略摇杆零值和轻微漂移
bool ADeliveryPlayerController::InputKey(const FInputKeyEventArgs& Params)
{
    if (Hints && IsLocalController() &&
        (Params.Event == IE_Pressed || (Params.Event == IE_Axis && FMath::Abs(Params.AmountDepressed) > 0.2f)))
    {
        Hints->SetInputDevice(Params.Key.IsGamepadKey());
    }
    return Super::InputKey(Params);
}

FText ADeliveryPlayerController::GetPhoneKeyText() const
{
    return Hints ? Hints->GetKeyText(TogglePhoneAction.LoadSynchronous()) : FText::GetEmpty();
}

// 面板状态由 HUD 保存，保留诊断命令的查询入口
const TSet<FName>& ADeliveryPlayerController::GetOpenUIPanelsForDebug() const
{
    if (const ADeliveryHUD* HUD = GetHUD<ADeliveryHUD>())
        return HUD->GetOpenPanelsForDebug();
    static const TSet<FName> EmptyPanels;
    return EmptyPanels;
}
