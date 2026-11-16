#include "UI/HUD/DeliveryHUD.h"
#include "UI/Conversation/DeliveryConversationWidget.h"
#include "UI/HUD/DeliveryTrunkStatusWidget.h"
#include "Vehicle/Trunk/DeliveryTrunkPassengerComponent.h"
#include "UI/HUD/DeliveryStartupWidget.h"
#include "UI/HUD/DeliveryHotbarWidget.h"
#include "UI/Conversation/DeliverySpeechStatusWidget.h"
#include "UI/Conversation/DeliveryAiInspectorWidget.h"
#include "UI/HUD/DeliveryMinimapWidget.h"
#include "UI/Controllers/DeliveryAiInspectorController.h"
#include "Speech/DeliverySpeechInputComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/Motorbike/DeliveryMotorbike.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "GameFramework/PlayerController.h"
#include "Player/DeliveryPlayerController.h"
#include "UI/Controllers/DeliveryPhoneController.h"
#include "TimerManager.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/SlateBlueprintLibrary.h"
#include "Blueprint/GameViewportSubsystem.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "HAL/PlatformTime.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "GenericPlatform/GenericWindow.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

// ── 生命周期 ──

/// @brief 每 0.1 秒更新一次；设置物品栏、交谈面板、语音状态图标和 AI 观察面板的默认 WBP
ADeliveryHUD::ADeliveryHUD()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickInterval = 0.1f;
    HotbarWidgetClass = FSoftClassPath(TEXT("/Game/UI/Inventory/WBP_DeliveryHotbar.WBP_DeliveryHotbar_C"));
    ConversationWidgetClass = FSoftClassPath(TEXT("/Game/UI/Conversation/WBP_Conversation.WBP_Conversation_C"));
    SpeechStatusWidgetClass = FSoftClassPath(TEXT("/Game/UI/Conversation/WBP_SpeechStatus.WBP_SpeechStatus_C"));
    AiInspectorWidgetClass = FSoftClassPath(TEXT("/Game/UI/Conversation/WBP_AiInspector.WBP_AiInspector_C"));
}

/// @brief 步行时返回这个角色，骑车时返回车上的驾驶员；物品栏和后备箱状态都读它
ADeliveryCharacter* ADeliveryHUD::ResolveCharacter(APawn* Pawn)
{
    if (ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(Pawn)) return Character;
    if (ADeliveryMotorbike* Bike = Cast<ADeliveryMotorbike>(Pawn)) return Bike->GetDriver();
    return nullptr;
}

/// @brief 每 0.1 秒：应用还没生效的输入模式，刷新物品栏和后备箱状态，摆放交谈面板、语音状态图标和 AI 观察面板
void ADeliveryHUD::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    TryApplyInputMode();
    RefreshHotbar();
    RefreshTrunkStatus();
    RefreshConversationLayout();
    RefreshSpeechStatusLayout();
    if (AiInspectorController) AiInspectorController->ValidateContext();
    RefreshAiInspectorLayout();
}

// ── 物品栏 ──

/// @brief 本机：找到当前角色（骑车时是驾驶员），第一次时创建物品栏，背包换了就重新绑定；角色还没同步时隐藏
void ADeliveryHUD::RefreshHotbar()
{
    APlayerController* Player = GetOwningPlayerController();
    if (!Player || !Player->IsLocalController()) return;
    ADeliveryCharacter* Character = ResolveCharacter(Player->GetPawn());
    if (!IsValid(Character))
    {
        // 控制对象或驾驶员尚未同步时先隐藏界面，避免继续显示旧角色的物品
        if (HotbarWidget) HotbarWidget->SetVisibility(ESlateVisibility::Collapsed);
        return;
    }
    UDeliveryInventoryComponent* Inventory = Character->GetInventoryComponent();
    if (BoundCharacter.Get() != Character)
    {
        // 更换角色时释放旧控件，同一个角色上下车则复用已有快捷栏
        RemoveHotbar();
        BoundCharacter = Character;
    }
    if (!HotbarWidget)
    {
        UClass* LoadedClass = HotbarWidgetClass.LoadSynchronous();
        if (!LoadedClass) LoadedClass = UDeliveryHotbarWidget::StaticClass();
        HotbarWidget = CreateWidget<UDeliveryHotbarWidget>(Player, LoadedClass);
        if (!HotbarWidget) return;
        HotbarWidget->SetInventory(Inventory);
        BoundInventory = Inventory;
        HotbarWidget->AddToViewport(10);
    }
    if (BoundInventory.Get() != Inventory)
    {
        HotbarWidget->SetInventory(Inventory);
        BoundInventory = Inventory;
    }
    HotbarWidget->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
}

/// @brief 移除物品栏并清空绑定；换了角色时调用，同一个角色上下车时复用
void ADeliveryHUD::RemoveHotbar()
{
    if (HotbarWidget)
    {
        HotbarWidget->SetInventory(nullptr);
        HotbarWidget->RemoveFromParent();
        HotbarWidget = nullptr;
    }
    BoundInventory.Reset();
    BoundCharacter.Reset();
}

// ── 生命周期 ──

/// @brief 关掉所有界面和处理器，关闭麦克风，清空面板记录；之后不再请求鼠标或键盘焦点
void ADeliveryHUD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // 清理面板时只更新本机状态，退出的世界不再请求鼠标或键盘焦点
    bEndingPlay = true;
    bInputModePending = false;
    InspectorMinimap.Reset();
    NextInspectorMinimapSearchAt = 0;
    if (AiInspectorController) AiInspectorController->Shutdown();
    AiInspectorController=nullptr;
    if (AiInspectorWidget) AiInspectorWidget->RemoveFromParent();
    AiInspectorWidget=nullptr;
    if (auto* Input=BoundSpeechInput.Get())
    {
        Input->OnStateChanged.Remove(SpeechStateHandle);
        Input->DisableVoice();
    }
    BoundSpeechInput.Reset();
    SpeechStateHandle.Reset();
    if (SpeechStatusWidget)
    {
        SpeechStatusWidget->SetSpeechInput(nullptr);
        SpeechStatusWidget->RemoveFromParent();
        SpeechStatusWidget=nullptr;
    }
    CloseResidentConversation();
    if (TrunkStatus) { TrunkStatus->RemoveFromParent(); TrunkStatus=nullptr; }
    GetWorldTimerManager().ClearTimer(StartupCloseTimer);
    if (StartupFlow.IsValid()) StartupFlow->OnPhaseChanged.RemoveDynamic(this, &ADeliveryHUD::HandleStartupPhase);
    StartupFlow.Reset();
    if (StartupWidget) StartupWidget->RemoveFromParent();
    StartupWidget = nullptr;
    if (PhoneController) PhoneController->Shutdown();
    PhoneController = nullptr;
    OpenPanels.Reset();
    ApplyInputMode();
    RemoveHotbar();
    Super::EndPlay(EndPlayReason);
}

/// @brief 依次调用 InitializeStartupMenu、GetPhoneController、InitializeSpeechStatus、InitializeAiInspector
void ADeliveryHUD::BeginPlay()
{
    Super::BeginPlay();
    InitializeStartupMenu();
    GetPhoneController();
    InitializeSpeechStatus();
    InitializeAiInspector();
}

// ── AI 观察面板 ──

/// @brief 非 Shipping 本机：创建观察面板和处理器 UDeliveryAiInspectorController，放在右上角
void ADeliveryHUD::InitializeAiInspector()
{
#if !UE_BUILD_SHIPPING
    auto* Player=GetOwningPlayerController();
    if (!Player || !Player->IsLocalController()) return;
    UClass* WidgetClass=AiInspectorWidgetClass.LoadSynchronous();
    if (!WidgetClass || !WidgetClass->IsChildOf(UDeliveryAiInspectorWidget::StaticClass()))
    {
        UE_LOG(LogTemp,Warning,TEXT("LLM NPC inspector Widget Blueprint is missing or has the wrong parent"));
        return;
    }
    AiInspectorWidget=CreateWidget<UDeliveryAiInspectorWidget>(Player,WidgetClass);
    if (!AiInspectorWidget) return;
    AiInspectorWidget->AddToViewport(30);
    AiInspectorWidget->SetDesiredSizeInViewport(FVector2D::ZeroVector);
    AiInspectorWidget->SetAlignmentInViewport(FVector2D(1.f,0.f));
    AiInspectorController=NewObject<UDeliveryAiInspectorController>(this);
    AiInspectorController->Initialize(Player,this,AiInspectorWidget);
    RefreshAiInspectorLayout();
#endif
}

/// @brief F6：展开或收起 AI 观察面板，再调用 RefreshAiInspectorLayout 重新摆放
void ADeliveryHUD::ToggleAiInspector()
{
    if (AiInspectorController) AiInspectorController->ToggleExpanded();
    RefreshAiInspectorLayout();
}

/// @brief 把观察面板放在小地图下方，底部避开交谈面板，告诉面板最多能占多大
void ADeliveryHUD::RefreshAiInspectorLayout()
{
    auto* Player=GetOwningPlayerController();
    if (bEndingPlay || !AiInspectorWidget || !Player || !Player->IsLocalController()) return;
    int32 Width=0,Height=0;
    Player->GetViewportSize(Width,Height);
    if (Width<=0 || Height<=0) return;
    const float Scale=FMath::Max(.1f,UWidgetLayoutLibrary::GetViewportScale(this));
    auto* Minimap=InspectorMinimap.Get();
    if (Minimap && (Minimap->GetWorld()!=GetWorld() || Minimap->GetOwningPlayer()!=Player || !Minimap->GetCachedWidget().IsValid()))
    {
        InspectorMinimap.Reset();
        Minimap=nullptr;
        NextInspectorMinimapSearchAt=0;
    }
    const double Now=FPlatformTime::Seconds();
    if (!Minimap && Now>=NextInspectorMinimapSearchAt)
    {
        // 主界面可能稍后构建，只在缺少引用时查找嵌套控件，找到后沿用本玩家的弱引用
        NextInspectorMinimapSearchAt=Now+.5;
        TArray<UUserWidget*> Widgets;
        UWidgetBlueprintLibrary::GetAllWidgetsOfClass(this,Widgets,UDeliveryMinimapWidget::StaticClass(),false);
        for (auto* Widget:Widgets)
            if (auto* Candidate=Cast<UDeliveryMinimapWidget>(Widget); Candidate && Candidate->GetWorld()==GetWorld() &&
                Candidate->GetOwningPlayer()==Player && Candidate->GetCachedWidget().IsValid())
            {
                InspectorMinimap=Candidate;
                Minimap=Candidate;
                break;
            }
    }
    // 第一帧尚无几何时，按正式小地图的顶部边距和默认直径预留
    float Top=(24.f+320.f)*Scale+12.f;
    if (Minimap && Minimap->IsVisible())
    {
        const FGeometry& Geometry=Minimap->GetCachedGeometry();
        const FVector2D MapSize=Geometry.GetLocalSize();
        if (MapSize.X>0 && MapSize.Y>0 && !MapSize.ContainsNaN())
        {
            FVector2D PixelPosition,ViewportPosition;
            USlateBlueprintLibrary::LocalToViewport(this,Geometry,MapSize,PixelPosition,ViewportPosition);
            if (FMath::IsFinite(PixelPosition.Y) && PixelPosition.Y>0)
                Top=float(PixelPosition.Y)+12.f;
        }
    }
    float Bottom=Height-FMath::Max(92.f,100.f*Scale);
    const auto* Viewport=UGameViewportSubsystem::Get();
    if (!Viewport) return;
    if (ConversationWidget && ConversationWidget->IsInViewport())
    {
        const auto Slot=Viewport->GetWidgetSlot(ConversationWidget);
        // 底部对话展开时，观察面板的可滚动区域缩短，不盖住输入和字幕
        Bottom=FMath::Min(Bottom,float(Slot.Offsets.Top*Scale)-float(ConversationWidget->GetDesiredSize().Y*Scale)-12.f);
    }
    AiInspectorWidget->SetAvailableSize(FVector2D(FMath::Max(1.f,(Width-32.f)/Scale),FMath::Max(80.f,(Bottom-Top)/Scale)));
    const FVector2D Position(Width-16.f,Top);
    const FMargin Current=Viewport->GetWidgetSlot(AiInspectorWidget).Offsets;
    if (!FMath::IsNearlyEqual(Current.Left,float(Position.X)/Scale) || !FMath::IsNearlyEqual(Current.Top,float(Position.Y)/Scale))
        AiInspectorWidget->SetPositionInViewport(Position);
}

// ── 语音状态图标 ──

/// @brief 本机：订阅麦克风组件的状态变化，创建语音状态图标放在左下角
void ADeliveryHUD::InitializeSpeechStatus()
{
    auto* Player=Cast<ADeliveryPlayerController>(GetOwningPlayerController());
    if (!Player || !Player->IsLocalController()) return;
    auto* Input=Player->GetSpeechInput();
    if (!Input) return;
    BoundSpeechInput=Input;
    SpeechStateHandle=Input->OnStateChanged.AddUObject(this,&ADeliveryHUD::HandleSpeechStateChanged);
    UClass* WidgetClass=SpeechStatusWidgetClass.LoadSynchronous();
    if (!WidgetClass || !WidgetClass->IsChildOf(UDeliverySpeechStatusWidget::StaticClass()))
    {
        UE_LOG(LogTemp,Warning,TEXT("Speech status Widget Blueprint is missing or has the wrong parent"));
        return;
    }
    SpeechStatusWidget=CreateWidget<UDeliverySpeechStatusWidget>(Player,WidgetClass);
    if (!SpeechStatusWidget) return;
    SpeechStatusWidget->SetSpeechInput(Input);
    SpeechStatusWidget->AddToViewport(35);
    SpeechStatusWidget->SetDesiredSizeInViewport(FVector2D::ZeroVector);
    SpeechStatusWidget->SetAlignmentInViewport(FVector2D(0.f,1.f));
    RefreshSpeechStatusLayout();
}

/// @brief 麦克风状态变化：没有语音状态图标时关掉麦克风，玩家不会在看不到提示的情况下被收音
void ADeliveryHUD::HandleSpeechStateChanged(EDeliverySpeechInputState State)
{
    // 交谈面板的开麦按钮会先调用 HasSpeechStatusWidget 检查；其他地方开了麦克风而图标不在时，这里把麦克风关掉
    if (!HasSpeechStatusWidget() && State!=EDeliverySpeechInputState::Off && State!=EDeliverySpeechInputState::Error)
        if (auto* Input=BoundSpeechInput.Get()) Input->DisableVoice();
}

/// @brief 语音状态图标是否已经在视口里；交谈面板开麦前先查它，没有图标时不开麦
bool ADeliveryHUD::HasSpeechStatusWidget() const
{
    return IsValid(SpeechStatusWidget) && SpeechStatusWidget->IsInViewport();
}

/// @brief 把语音状态图标放在物品栏上方；和交谈面板重叠时移到交谈面板上方
void ADeliveryHUD::RefreshSpeechStatusLayout()
{
    if (!SpeechStatusWidget || !GetOwningPlayerController()) return;
    int32 Width=0,Height=0;
    GetOwningPlayerController()->GetViewportSize(Width,Height);
    if (Width<=0 || Height<=0) return;
    const float Scale=FMath::Max(.1f,UWidgetLayoutLibrary::GetViewportScale(this));
    const auto* Viewport=UGameViewportSubsystem::Get();
    if (!Viewport) return;
    // 默认留出底部物品栏，窄屏发生重叠时移到对话框上方
    FVector2D DpiPosition(20.f,FMath::Max(16.f,Height/Scale-128.f));
    const FVector2D StatusSize=SpeechStatusWidget->GetDesiredSize();
    if (ConversationWidget && StatusSize.X>0 && StatusSize.Y>0)
    {
        const auto PanelSlot=Viewport->GetWidgetSlot(ConversationWidget);
        const FVector2D PanelSize=ConversationWidget->GetDesiredSize();
        const FVector2D PanelMin=FVector2D(PanelSlot.Offsets.Left,PanelSlot.Offsets.Top)-PanelSlot.Alignment*PanelSize;
        const FVector2D PanelMax=PanelMin+PanelSize;
        if (DpiPosition.X<PanelMax.X && DpiPosition.X+StatusSize.X>PanelMin.X &&
            DpiPosition.Y>PanelMin.Y && DpiPosition.Y-StatusSize.Y<PanelMax.Y)
            DpiPosition.Y=FMath::Max(16.0+StatusSize.Y,PanelMin.Y-12.0);
    }
    const FVector2D Position=DpiPosition*Scale;
    const FMargin Current=Viewport->GetWidgetSlot(SpeechStatusWidget).Offsets;
    if (!FMath::IsNearlyEqual(Current.Left,float(Position.X)/Scale) || !FMath::IsNearlyEqual(Current.Top,float(Position.Y)/Scale))
        SpeechStatusWidget->SetPositionInViewport(Position);
}

// ── 开始菜单 ──

/// @brief 本机且还没开局时：订阅 ADeliveryStartupFlow 的阶段变化，登记 Startup 面板打开，创建开始菜单
void ADeliveryHUD::InitializeStartupMenu()
{
    APlayerController* Player = GetOwningPlayerController();
    if (!Player || !Player->IsLocalController()) return;
    StartupFlow = ADeliveryStartupFlow::GetDeliveryStartupFlow(this);
    if (!StartupFlow.IsValid() || StartupFlow->GetPhase() == EDeliveryStartupPhase::Playing) return;
    StartupFlow->OnPhaseChanged.AddUniqueDynamic(this, &ADeliveryHUD::HandleStartupPhase);
    SetPanelOpen(TEXT("Startup"), true);
    if (UClass* MenuClass = StartupFlow->MenuWidgetClass.LoadSynchronous())
    {
        StartupWidget = CreateWidget<UDeliveryStartupWidget>(Player, MenuClass);
        if (StartupWidget) StartupWidget->AddToViewport(1000);
    }
}

/// @brief 进入 Playing 阶段时，下一帧调用 CloseStartupMenu；这一帧先让菜单自己也收到 Playing
void ADeliveryHUD::HandleStartupPhase(EDeliveryStartupPhase Phase)
{
    if (Phase != EDeliveryStartupPhase::Playing) return;
    // 等本轮通知结束，让菜单先收到 Playing，再解除绑定并移除
    if (!GetWorldTimerManager().IsTimerActive(StartupCloseTimer))
        StartupCloseTimer = GetWorldTimerManager().SetTimerForNextTick(this, &ADeliveryHUD::CloseStartupMenu);
}

/// @brief 登记 Startup 面板关闭（恢复输入模式），再移除开始菜单
void ADeliveryHUD::CloseStartupMenu()
{
    // 控件还在焦点路径中时恢复模式，再移除菜单
    SetPanelOpen(TEXT("Startup"), false);
    if (StartupWidget) StartupWidget->RemoveFromParent();
    StartupWidget = nullptr;
}

// ── 面板与鼠标 ──

/// @brief 手机处理器；第一次调用时在本机创建
UDeliveryPhoneController* ADeliveryHUD::GetPhoneController()
{
    auto* Player = Cast<ADeliveryPlayerController>(GetOwningPlayerController());
    if (!PhoneController && Player && Player->IsLocalController())
    {
        PhoneController = NewObject<UDeliveryPhoneController>(this);
        PhoneController->Initialize(Player, this);
    }
    return PhoneController;
}

/// @brief 登记面板打开或关闭；从有面板变成没有（或反过来）、或开始菜单开关时，调用 ApplyInputMode 切换鼠标和输入模式
void ADeliveryHUD::SetPanelOpen(FName PanelId, bool bOpen)
{
    if (PanelId.IsNone()) return;
    const bool bWasOpen = IsAnyPanelOpen();
    const bool bStartupWasOpen = IsPanelOpen(TEXT("Startup"));
    if (bOpen) OpenPanels.Add(PanelId);
    else OpenPanels.Remove(PanelId);
    // 启动菜单切换时也要更新 UIOnly；其他面板沿用首开末关规则
    if (bWasOpen != IsAnyPanelOpen() || bStartupWasOpen != IsPanelOpen(TEXT("Startup"))) ApplyInputMode();
}

/// @brief 按 IsAnyPanelOpen 设置是否显示鼠标，记下要切换输入模式，再调用 TryApplyInputMode
void ADeliveryHUD::ApplyInputMode()
{
    APlayerController* Player = GetOwningPlayerController();
    if (!Player || !Player->IsLocalController()) return;
    Player->bShowMouseCursor = IsAnyPanelOpen();
    bInputModePending = !bEndingPlay;
    TryApplyInputMode();
}

/// @brief 游戏窗口在前台、键盘焦点在这个视口里时返回 true；PIE 嵌在编辑器里时不能只看编辑器窗口
bool ADeliveryHUD::CanApplyInputMode() const
{
    if (bEndingPlay || !GetWorld() || !FSlateApplication::IsInitialized()) return false;
    const auto* Player = GetOwningPlayerController();
    const auto* LocalPlayer = Player ? Player->GetLocalPlayer() : nullptr;
    const auto* ViewportClient = GetWorld()->GetGameViewport();
    const auto Viewport = ViewportClient ? ViewportClient->GetGameViewportWidget() : nullptr;
    if (!LocalPlayer || !Viewport) return false;
    const auto& Slate = FSlateApplication::Get();
    if (!Slate.IsActive()) return false;
    const auto Window = Slate.FindWidgetWindow(Viewport.ToSharedRef());
    if (!Window.IsValid() || Window != Slate.GetActiveTopLevelWindow()) return false;
    // Slate 的焦点记录可能由代码更新，还要核对真正处于前台的原生窗口
    const auto NativeWindow = Window->GetNativeWindow();
    const auto SlateUser = LocalPlayer->GetSlateUser();
    // UMG 也是视口后代；内嵌 PIE 不能仅凭共用的编辑器窗口认领焦点
    return NativeWindow.IsValid() && NativeWindow->IsForegroundWindow() &&
        SlateUser.IsValid() && SlateUser->IsWidgetInFocusPath(Viewport);
}

/// @brief 游戏窗口有焦点时才切换输入模式：开始菜单开着用 UIOnly，有面板开着用 GameAndUI，全关用 GameOnly
void ADeliveryHUD::TryApplyInputMode()
{
    if (!bInputModePending || !CanApplyInputMode()) return;
    APlayerController* Player = GetOwningPlayerController();
    if (!Player || !Player->IsLocalController()) return;
    bInputModePending = false;
    if (IsPanelOpen(TEXT("Startup")))
    {
        FInputModeUIOnly Mode;
        Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
        Player->SetInputMode(Mode);
    }
    else if (IsAnyPanelOpen())
    {
        FInputModeGameAndUI Mode;
        Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
        Mode.SetHideCursorDuringCapture(false);
        Player->SetInputMode(Mode);
    }
    else Player->SetInputMode(FInputModeGameOnly());
}

// ── 物品栏 ──

/// @brief 玩家躺在后备箱里时显示后备箱状态条（文字和进度），否则隐藏
void ADeliveryHUD::RefreshTrunkStatus()
{
    auto* PC=GetOwningPlayerController();
    auto* Character=PC ? ResolveCharacter(PC->GetPawn()):nullptr;
    auto* Passenger=Character ? Character->TrunkPassenger.Get():nullptr;
    if (!Passenger || !Passenger->IsPassenger())
    {
        if (TrunkStatus) TrunkStatus->SetVisibility(ESlateVisibility::Collapsed);
        return;
    }
    if (!TrunkStatus)
    {
        TrunkStatus=CreateWidget<UDeliveryTrunkStatusWidget>(PC);
        if (!TrunkStatus) return;
        TrunkStatus->AddToViewport(20);
    }
    TrunkStatus->SetVisibility(ESlateVisibility::HitTestInvisible);
    const bool bTransition=Passenger->GetPhase()==EDeliveryPassengerPhase::Loading ||
        Passenger->GetPhase()==EDeliveryPassengerPhase::Unloading;
    TrunkStatus->SetStatus(Passenger->GetStatusText(),bTransition ? Passenger->GetPhaseProgress():Passenger->GetRecoveryProgress());
}

// ── 交谈面板 ──

/// @brief 关掉旧的交谈面板，新建 WBP_Conversation、调用 SetResident，让 AI 观察面板看这位 NPC，登记面板打开，再把焦点给输入框
void ADeliveryHUD::OpenResidentConversation(ADeliveryNpcCharacter* Resident)
{
    CloseResidentConversation();
    UClass* WidgetClass=ConversationWidgetClass.LoadSynchronous();
    if (!WidgetClass || !WidgetClass->IsChildOf(UDeliveryConversationWidget::StaticClass()))
    {
        UE_LOG(LogTemp,Warning,TEXT("Conversation Widget Blueprint is missing or has the wrong parent"));
        return;
    }
    ConversationWidget=CreateWidget<UDeliveryConversationWidget>(GetOwningPlayerController(),WidgetClass);
    if (!ConversationWidget) return;
    ConversationWidget->SetResident(Resident);
    if (AiInspectorController) AiInspectorController->ObserveNpc(Resident);
    ConversationWidget->AddToViewport(40);
    // 两轴都设为零时，视口采用 UMG 内容的实际高度，宽度由内部 SizeBox 限制
    ConversationWidget->SetDesiredSizeInViewport(FVector2D::ZeroVector);
    ConversationWidget->SetAlignmentInViewport(FVector2D(.5f,1.f));
    RefreshConversationLayout();
    SetPanelOpen(TEXT("ResidentConversation"),true);
    if (CanApplyInputMode()) ConversationWidget->FocusInput();
}

/// @brief 交谈面板宽度取屏幕宽的 52%（440 到 720），居中放在物品栏上方
void ADeliveryHUD::RefreshConversationLayout()
{
    if (!ConversationWidget || !GetOwningPlayerController()) return;
    int32 Width=0,Height=0;
    GetOwningPlayerController()->GetViewportSize(Width,Height);
    if (Width<=0 || Height<=0) return;
    const float Scale=FMath::Max(.1f,UWidgetLayoutLibrary::GetViewportScale(this));
    // 窄条居中放在物品栏上方，展开例句时由蓝图内容向上增加高度
    float PanelWidth=FMath::Clamp(Width*.52f,440.f,720.f);
    PanelWidth=FMath::Min(PanelWidth,FMath::Max(1.f,Width-32.f));
    ConversationWidget->SetAvailableWidth(PanelWidth/Scale);
    // 物品栏也随 DPI 放大，底部间距采用相同缩放，避免高分辨率下压住例句按钮
    const float BottomGap=FMath::Max(110.f,120.f*Scale);
    const FVector2D Position(Width*.5f,FMath::Max(16.f,Height-BottomGap));
    const auto* Viewport=UGameViewportSubsystem::Get();
    if (!Viewport) return;
    const FMargin Current=Viewport->GetWidgetSlot(ConversationWidget).Offsets;
    if (!FMath::IsNearlyEqual(Current.Left,float(Position.X)/Scale) || !FMath::IsNearlyEqual(Current.Top,float(Position.Y)/Scale))
        ConversationWidget->SetPositionInViewport(Position);
}

/// @brief 登记交谈面板关闭（恢复输入模式），再把面板移出视口
void ADeliveryHUD::CloseResidentConversation()
{
    // 先处理当前焦点，再解除文本框与视口的联系
    SetPanelOpen(TEXT("ResidentConversation"),false);
    if (ConversationWidget) { ConversationWidget->RemoveFromParent(); ConversationWidget=nullptr; }
}
