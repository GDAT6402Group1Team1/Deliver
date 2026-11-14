// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/DeliveryPlayerController.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionComponent.h"
#include "Game/DeliveryStartupFlow.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Blueprint/UserWidget.h"
#include "Audio/DeliveryDialogueVoiceComponent.h"
#include "Audio/DeliveryMusicComponent.h"
#include "Speech/DeliverySpeechInputComponent.h"
#include "Delivery.h"
#include "Components/PostProcessComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "ConvexVolume.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedInputComponent.h"
#include "InputKeyEventArgs.h"
#include "UI/DeliveryHintComponent.h"
#include "UI/DeliveryHUD.h"
#include "UI/Controllers/DeliveryPhoneController.h"
#include "Audio/DeliveryPhoneAudioComponent.h"
#include "Engine/LocalPlayer.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "GenericPlatform/GenericWindow.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Materials/MaterialInterface.h"
#include "Phone/DeliveryPhoneCallQueueComponent.h"
#include "Task/Tracking/DeliveryGroundArrowComponent.h"
#include "Task/Tracking/DeliveryObjectiveRingComponent.h"
#include "Task/DeliverySettlementComponent.h"
#include "SceneView.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

ADeliveryPlayerController::ADeliveryPlayerController()
{
    ResidentConversation=CreateDefaultSubobject<UDeliveryConversationComponent>(TEXT("ResidentConversation"));
    SpeechInput=CreateDefaultSubobject<UDeliverySpeechInputComponent>(TEXT("SpeechInput"));
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

void ADeliveryPlayerController::InvalidateCompanionView()
{
    if (HasAuthority()) ++CompanionViewRevision;
    if (IsLocalController() && GetWorld())
        LocalCompanionViewChangedAt = static_cast<float>(GetWorld()->GetTimeSeconds());
}

void ADeliveryPlayerController::OnPossess(APawn* InPawn)
{
    InvalidateCompanionView();
    Super::OnPossess(InPawn);
}

void ADeliveryPlayerController::OnUnPossess()
{
    InvalidateCompanionView();
    Super::OnUnPossess();
}

void ADeliveryPlayerController::SetViewTarget(AActor* NewViewTarget, FViewTargetTransitionParams TransitionParams)
{
    InvalidateCompanionView();
    Super::SetViewTarget(NewViewTarget, TransitionParams);
}

bool ADeliveryPlayerController::AreCompanionCatchUpBoxesUnseen(APawn* ViewPawn, AActor* ViewTarget,
    const FVector& StartCenter, const FVector& EndCenter, const FVector& Extent) const
{
    // 旁观、调试、过渡和尚未初始化的镜头均视为未知，不能默认无人看见
    if (!IsLocalPlayerController() || !IsValid(ViewPawn) || ViewPawn != GetPawn() ||
        ViewPawn->GetController() != this || ViewTarget != ViewPawn || ViewTarget != GetViewTarget() ||
        (!ViewPawn->IsA<ADeliveryCharacter>() && !ViewPawn->IsA<ADeliveryMotorbike>()) ||
        !PlayerCameraManager || PlayerCameraManager->ViewTarget.Target != ViewTarget ||
        PlayerCameraManager->PendingViewTarget.Target || PlayerCameraManager->CameraStyle != NAME_Default ||
        PlayerCameraManager->bClientSimulatingViewTarget || PlayerCameraManager->GetCameraCacheTime() <= 0.0f ||
        PlayerCameraManager->GetCameraCacheTime() <= LocalCompanionViewChangedAt ||
        StartCenter.ContainsNaN() || EndCenter.ContainsNaN() || Extent.ContainsNaN() || Extent.GetMin() <= 0.0)
        return false;

    ULocalPlayer* LocalPlayer = GetLocalPlayer();
    FViewport* Viewport = LocalPlayer && LocalPlayer->ViewportClient ? LocalPlayer->ViewportClient->Viewport : nullptr;
    if (!Viewport || !GEngine || GEngine->IsStereoscopic3D(Viewport)) return false;

    FSceneViewProjectionData Projection;
    if (!LocalPlayer->GetProjectionData(Viewport, Projection) || !Projection.IsValidViewRectangle() ||
        !Projection.IsPerspectiveProjection() || Projection.ViewOrigin.ContainsNaN()) return false;
    const FMatrix ViewProjection = Projection.ComputeViewProjectionMatrix();
    if (ViewProjection.ContainsNaN()) return false;

    // 使用本机实际宽高比和投影，不把近远裁剪或遮挡当作安全补位依据
    FConvexVolume Frustum;
    GetViewFrustumBounds(Frustum, ViewProjection, false, false);
    if (Frustum.Planes.Num() != 4) return false;

    // 服务器传入身体及安全余量，再补两厘米覆盖网络量化误差
    const FVector PaddedExtent = Extent + FVector(2.0);
    return !Frustum.IntersectBox(StartCenter, PaddedExtent) && !Frustum.IntersectBox(EndCenter, PaddedExtent);
}

void ADeliveryPlayerController::ClientCheckCompanionCatchUp_Implementation(ADeliveryCompanionCharacter* Companion,
    FGuid RequestId, FVector_NetQuantize StartCenter, FVector_NetQuantize EndCenter, FVector_NetQuantize Extent)
{
    APawn* ViewPawn = GetPawn();
    AActor* ViewTarget = GetViewTarget();
    const bool bUnseen = IsValid(Companion) && Companion->GetWorld() == GetWorld() && RequestId.IsValid() &&
        AreCompanionCatchUpBoxesUnseen(ViewPawn, ViewTarget, StartCenter, EndCenter, Extent);
    ServerReportCompanionCatchUpView(Companion, RequestId, ViewPawn, ViewTarget, bUnseen);
}

void ADeliveryPlayerController::ServerReportCompanionCatchUpView_Implementation(ADeliveryCompanionCharacter* Companion,
    FGuid RequestId, APawn* ViewPawn, AActor* ViewTarget, bool bUnseen)
{
    if (!HasAuthority() || !IsValid(Companion) || Companion->GetWorld() != GetWorld() ||
        !RequestId.IsValid() || !IsValid(Companion->Companion)) return;
    // 同伴组件核验挑战、全部观察者、超时和版本，单个客户端回答不代表可补位
    Companion->Companion->AcceptCatchUpVisibility(this, RequestId, ViewPawn, ViewTarget, bUnseen);
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

bool ADeliveryPlayerController::HasWalkingInputFocus() const
{
    const ULocalPlayer* LocalPlayer = GetLocalPlayer();
    // 没有本机视口的服务器和独立逻辑测试不读取 Slate 的全局焦点
    if (!LocalPlayer) return true;
    UGameViewportClient* ViewportClient = LocalPlayer->ViewportClient.Get();
    const auto Viewport = ViewportClient ? ViewportClient->GetGameViewportWidget() : nullptr;
    if (!Viewport || !FSlateApplication::IsInitialized()) return false;
    const auto& Slate = FSlateApplication::Get();
    const auto Window = Slate.FindWidgetWindow(Viewport.ToSharedRef());
    const auto NativeWindow = Window.IsValid() ? Window->GetNativeWindow() : nullptr;
    const auto SlateUser = LocalPlayer->GetSlateUser();
    // 输入框或按钮取得焦点后，只允许界面操作，不能沿用此前的移动键
    return Slate.IsActive() && Window == Slate.GetActiveTopLevelWindow() &&
        NativeWindow.IsValid() && NativeWindow->IsForegroundWindow() && SlateUser.IsValid() &&
        SlateUser->GetFocusedWidget() == Viewport;
}

void ADeliveryPlayerController::PreProcessInput(float DeltaTime, bool bGamePaused)
{
    Super::PreProcessInput(DeltaTime, bGamePaused);
    if (!IsLocalController() || !Cast<ADeliveryCharacter>(GetPawn())) return;
    UpdateWalkingInputFocus(!bGamePaused && HasWalkingInputFocus());
}

void ADeliveryPlayerController::UpdateWalkingInputFocus(bool bAllowed)
{
    const bool bLostFocus = bWalkingInputAllowed && !bAllowed;
    bWalkingInputAllowed = bAllowed;
    if (bLostFocus) FlushPressedKeys();
}

void ADeliveryPlayerController::FlushPressedKeys()
{
    // 引擎失焦清键不会清理布娃娃中独立保存的移动意图
    if (auto* ControlledCharacter = Cast<ADeliveryCharacter>(GetPawn())) ControlledCharacter->DoMove(0.0f, 0.0f);
    Super::FlushPressedKeys();
}

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
