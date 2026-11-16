// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/DeliveryPlayerController.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "Game/DeliveryStartupFlow.h"
#include "Character/DeliveryCharacter.h"
#include "Events/DeliveryEventComponent.h"
#include "Vehicle/Motorbike/DeliveryMotorbike.h"
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
#include "UI/HUD/DeliveryHintComponent.h"
#include "UI/HUD/DeliveryHUD.h"
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

// ── 生命周期 ──

/// @brief 创建上面那些本机组件和高亮后处理，设置交互 IA 的软引用
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

// ── 开局 ──

// ── 开局 ──
// 客户端的开始界面加载完成后调用 ServerReportStartupReady，服务器转给 ADeliveryStartupFlow 的 ReportPlayerReady

/// @brief 客户端加载完成后通过自己的控制器报告给服务器，服务器转给 Flow 的 ReportPlayerReady；客户端不能直接对世界里的 Actor 发 RPC
void ADeliveryPlayerController::ServerReportStartupReady_Implementation(ADeliveryStartupFlow* Flow)
{
    if (Flow && Flow == ADeliveryStartupFlow::GetDeliveryStartupFlow(this))
        Flow->ReportPlayerReady(this);
}

// ── 二狗补位 ──

// ── 二狗补位 ──
// 二狗组件在服务器上调用 ClientCheckCompanionCatchUp 问每个玩家，本机调用 AreCompanionCatchUpBoxesUnseen 判断后用 ServerReportCompanionCatchUpView 回答
// 换控制对象或镜头时 InvalidateCompanionView 让版本号加一，换之前发出的问题的回答作废

/// @brief 服务器上镜头版本号加一；本机记下换视角的时间，之后至少等一次镜头更新再回答
void ADeliveryPlayerController::InvalidateCompanionView()
{
    if (HasAuthority()) ++CompanionViewRevision;
    if (IsLocalController() && GetWorld())
        LocalCompanionViewChangedAt = static_cast<float>(GetWorld()->GetTimeSeconds());
}

// ── 生命周期 ──

/// @brief 换控制对象时调用 InvalidateCompanionView
void ADeliveryPlayerController::OnPossess(APawn* InPawn)
{
    InvalidateCompanionView();
    Super::OnPossess(InPawn);
}

/// @brief 失去控制对象时调用 InvalidateCompanionView
void ADeliveryPlayerController::OnUnPossess()
{
    InvalidateCompanionView();
    Super::OnUnPossess();
}

// ── 二狗补位 ──

/// @brief 换镜头时调用 InvalidateCompanionView，旧的补位回答作废
void ADeliveryPlayerController::SetViewTarget(AActor* NewViewTarget, FViewTargetTransitionParams TransitionParams)
{
    InvalidateCompanionView();
    Super::SetViewTarget(NewViewTarget, TransitionParams);
}

/// @brief 按本机镜头的视锥判断两个盒子是否都看不见；旁观、过渡或镜头还没准备好时一律算看得见
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

/// @brief 服务器问本机：StartCenter 和 EndCenter 两个盒子在本机镜头里是不是都看不见，算完调用 ServerReportCompanionCatchUpView 回答
void ADeliveryPlayerController::ClientCheckCompanionCatchUp_Implementation(ADeliveryCompanionCharacter* Companion,
    FGuid RequestId, FVector_NetQuantize StartCenter, FVector_NetQuantize EndCenter, FVector_NetQuantize Extent)
{
    APawn* ViewPawn = GetPawn();
    AActor* ViewTarget = GetViewTarget();
    const bool bUnseen = IsValid(Companion) && Companion->GetWorld() == GetWorld() && RequestId.IsValid() &&
        AreCompanionCatchUpBoxesUnseen(ViewPawn, ViewTarget, StartCenter, EndCenter, Extent);
    ServerReportCompanionCatchUpView(Companion, RequestId, ViewPawn, ViewTarget, bUnseen);
}

/// @brief 本机回答服务器：bUnseen 为 true 表示两个盒子都看不见；服务器转给二狗组件的 AcceptCatchUpVisibility 核对
void ADeliveryPlayerController::ServerReportCompanionCatchUpView_Implementation(ADeliveryCompanionCharacter* Companion,
    FGuid RequestId, APawn* ViewPawn, AActor* ViewTarget, bool bUnseen)
{
    if (!HasAuthority() || !IsValid(Companion) || Companion->GetWorld() != GetWorld() ||
        !RequestId.IsValid() || !IsValid(Companion->Companion)) return;
    // 同伴组件核验挑战、全部观察者、超时和版本，单个客户端回答不代表可补位
    Companion->Companion->AcceptCatchUpVisibility(this, RequestId, ViewPawn, ViewTarget, bUnseen);
}

// ── 生命周期 ──

/// @brief 本机启用交互物高亮，界面和电话由各自处理器管理
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

// ── 输入映射 ──

/// @brief 现有输入配置缺少交互操作时补充映射
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

// ── 输入 ──

/// @brief 游戏窗口在前台、键盘焦点在游戏视口上时返回 true；焦点在输入框或按钮上时返回 false
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

/// @brief 每帧处理输入之前：步行时检查游戏窗口是否有键盘焦点，没有就调用 UpdateWalkingInputFocus(false)
void ADeliveryPlayerController::PreProcessInput(float DeltaTime, bool bGamePaused)
{
    Super::PreProcessInput(DeltaTime, bGamePaused);
    if (!IsLocalController() || !Cast<ADeliveryCharacter>(GetPawn())) return;
    UpdateWalkingInputFocus(!bGamePaused && HasWalkingInputFocus());
}

/// @brief 记下能否步行输入；从能变成不能时调用 FlushPressedKeys，按着的移动键不再生效
void ADeliveryPlayerController::UpdateWalkingInputFocus(bool bAllowed)
{
    const bool bLostFocus = bWalkingInputAllowed && !bAllowed;
    bWalkingInputAllowed = bAllowed;
    if (bLostFocus) FlushPressedKeys();
}

/// @brief 引擎清空按键时（例如窗口失去焦点），顺带把角色的移动意图清零，布娃娃另外存着移动意图
void ADeliveryPlayerController::FlushPressedKeys()
{
    // 引擎失焦清键不会清理布娃娃中独立保存的移动意图
    if (auto* ControlledCharacter = Cast<ADeliveryCharacter>(GetPawn())) ControlledCharacter->DoMove(0.0f, 0.0f);
    Super::FlushPressedKeys();
}

/// @brief 绑定手机、交互和本机界面相关按键
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

/// @brief HUD 上的手机处理器
UDeliveryPhoneController* ADeliveryPlayerController::GetPhoneController() const
{
    auto* HUD = Cast<ADeliveryHUD>(GetHUD());
    return HUD ? HUD->GetPhoneController() : nullptr;
}

/// @brief 输入只转交操作，手机开关规则由处理器决定
void ADeliveryPlayerController::HandleTogglePhonePressed()
{
    if (auto* Phone = GetPhoneController()) Phone->Toggle();
}

// ── 界面与鼠标 ──

/// @brief 保留现有蓝图入口，面板状态统一交给 HUD
void ADeliveryPlayerController::SetUIPanelOpen(FName PanelId, bool bOpen)
{
    if (auto* HUD = Cast<ADeliveryHUD>(GetHUD())) HUD->SetPanelOpen(PanelId, bOpen);
}

/// @brief 手机面板在实际打开或收起后调用（包括来电自动展开、接听后收起），通过 PhoneWidget 找到控制器再转给手机处理器
void ADeliveryPlayerController::ReportPhoneUIOpen(UUserWidget* PhoneWidget, bool bOpen)
{
    auto* Player = PhoneWidget ? Cast<ADeliveryPlayerController>(PhoneWidget->GetOwningPlayer()) : nullptr;
    if (Player)
        if (auto* Phone = Player->GetPhoneController()) Phone->SetOpen(bOpen);
}

/// @brief HUD 上还有没有打开的面板
bool ADeliveryPlayerController::IsAnyUIPanelOpen() const
{
    const auto* HUD = Cast<ADeliveryHUD>(GetHUD());
    return HUD && HUD->IsAnyPanelOpen();
}

/// @brief 手机面板是否打开，读 HUD 的记录
bool ADeliveryPlayerController::IsPhoneUIOpen() const
{
    const auto* HUD = Cast<ADeliveryHUD>(GetHUD());
    return HUD && HUD->IsPanelOpen(UDeliveryPhoneController::PanelId);
}

// ── 电话 ──

// ── 电话 ──
// 电话队列在 GameState 上：服务器上直接调用 AnswerCurrentCall 或 HangUpCurrentCall，客户端先发 Server RPC，到了服务器再调用同一个函数

/// @brief 接听当前来电；服务器上直接调用电话队列的 AnswerCurrentCall，客户端发 ServerAnswerCall
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

/// @brief 客户端接听请求到了服务器，调用 RequestAnswerCall
void ADeliveryPlayerController::ServerAnswerCall_Implementation()
{
    RequestAnswerCall();
}

/// @brief 通过服务器入口请求拒接或挂断共享来电
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

/// @brief 客户端挂断请求到了服务器，调用 RequestHangUpCall
void ADeliveryPlayerController::ServerHangUpCall_Implementation()
{
    RequestHangUpCall();
}

// ── 交付 ──

/// @brief 服务器确认交付成功后调用：本机在 Recipient（交货的那具身体）上发布 DeliverySucceeded 事件，播放音效
/// 玩家正在骑车时也能收到，因为这个 RPC 发给控制器而不是角色
void ADeliveryPlayerController::ClientConfirmDelivery_Implementation(ADeliveryCharacter* Recipient)
{
    if (!IsLocalController() || !IsValid(Recipient) || Recipient->IsActorBeingDestroyed() || Recipient->GetWorld()!=GetWorld()) return;
    if (auto* Events=Recipient->FindComponentByClass<UDeliveryEventComponent>())
        Events->Publish(EDeliveryEvent::DeliverySucceeded);
}

// ── 交互 ──

/// @brief 确认请求者控制角色或其车辆，再转交角色交互组件
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

// ── 输入 ──

/// @brief 只在实际使用另一种设备时更新键名，忽略摇杆零值和轻微漂移
bool ADeliveryPlayerController::InputKey(const FInputKeyEventArgs& Params)
{
    if (Hints && IsLocalController() &&
        (Params.Event == IE_Pressed || (Params.Event == IE_Axis && FMath::Abs(Params.AmountDepressed) > 0.2f)))
    {
        Hints->SetInputDevice(Params.Key.IsGamepadKey());
    }
    return Super::InputKey(Params);
}

/// @brief 手机键当前映射的键名，按当前设备显示
FText ADeliveryPlayerController::GetPhoneKeyText() const
{
    return Hints ? Hints->GetKeyText(TogglePhoneAction.LoadSynchronous()) : FText::GetEmpty();
}

// ── 界面与鼠标 ──

/// @brief 面板状态由 HUD 保存，保留诊断命令的查询入口
const TSet<FName>& ADeliveryPlayerController::GetOpenUIPanelsForDebug() const
{
    if (const ADeliveryHUD* HUD = GetHUD<ADeliveryHUD>())
        return HUD->GetOpenPanelsForDebug();
    static const TSet<FName> EmptyPanels;
    return EmptyPanels;
}
