#include "UI/DeliveryHUD.h"
#include "UI/DeliveryConversationWidget.h"
#include "UI/DeliveryTrunkStatusWidget.h"
#include "Vehicle/DeliveryTrunkPassengerComponent.h"
#include "UI/DeliveryStartupWidget.h"
#include "UI/DeliveryHotbarWidget.h"
#include "UI/DeliverySpeechStatusWidget.h"
#include "UI/DeliveryAiInspectorWidget.h"
#include "UI/DeliveryMinimapWidget.h"
#include "UI/Controllers/DeliveryAiInspectorController.h"
#include "Speech/DeliverySpeechInputComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/DeliveryMotorbike.h"
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

ADeliveryHUD::ADeliveryHUD()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickInterval = 0.1f;
    HotbarWidgetClass = FSoftClassPath(TEXT("/Game/UI/Inventory/WBP_DeliveryHotbar.WBP_DeliveryHotbar_C"));
    ConversationWidgetClass = FSoftClassPath(TEXT("/Game/UI/Conversation/WBP_Conversation.WBP_Conversation_C"));
    SpeechStatusWidgetClass = FSoftClassPath(TEXT("/Game/UI/Conversation/WBP_SpeechStatus.WBP_SpeechStatus_C"));
    AiInspectorWidgetClass = FSoftClassPath(TEXT("/Game/UI/Conversation/WBP_AiInspector.WBP_AiInspector_C"));
}

ADeliveryCharacter* ADeliveryHUD::ResolveCharacter(APawn* Pawn)
{
    if (ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(Pawn)) return Character;
    if (ADeliveryMotorbike* Bike = Cast<ADeliveryMotorbike>(Pawn)) return Bike->GetDriver();
    return nullptr;
}

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

void ADeliveryHUD::BeginPlay()
{
    Super::BeginPlay();
    InitializeStartupMenu();
    GetPhoneController();
    InitializeSpeechStatus();
    InitializeAiInspector();
}

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

void ADeliveryHUD::ToggleAiInspector()
{
    if (AiInspectorController) AiInspectorController->ToggleExpanded();
    RefreshAiInspectorLayout();
}

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

void ADeliveryHUD::HandleSpeechStateChanged(EDeliverySpeechInputState State)
{
    // 对话按钮先检查正式界面，这里兜底阻止其他入口留下没有指示的麦克风
    if (!HasSpeechStatusWidget() && State!=EDeliverySpeechInputState::Off && State!=EDeliverySpeechInputState::Error)
        if (auto* Input=BoundSpeechInput.Get()) Input->DisableVoice();
}

bool ADeliveryHUD::HasSpeechStatusWidget() const
{
    return IsValid(SpeechStatusWidget) && SpeechStatusWidget->IsInViewport();
}

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

void ADeliveryHUD::HandleStartupPhase(EDeliveryStartupPhase Phase)
{
    if (Phase != EDeliveryStartupPhase::Playing) return;
    // 等本轮通知结束，让菜单先收到 Playing，再解除绑定并移除
    if (!GetWorldTimerManager().IsTimerActive(StartupCloseTimer))
        StartupCloseTimer = GetWorldTimerManager().SetTimerForNextTick(this, &ADeliveryHUD::CloseStartupMenu);
}

void ADeliveryHUD::CloseStartupMenu()
{
    // 控件还在焦点路径中时恢复模式，再移除菜单
    SetPanelOpen(TEXT("Startup"), false);
    if (StartupWidget) StartupWidget->RemoveFromParent();
    StartupWidget = nullptr;
}

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

void ADeliveryHUD::ApplyInputMode()
{
    APlayerController* Player = GetOwningPlayerController();
    if (!Player || !Player->IsLocalController()) return;
    Player->bShowMouseCursor = IsAnyPanelOpen();
    bInputModePending = !bEndingPlay;
    TryApplyInputMode();
}

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
void ADeliveryHUD::CloseResidentConversation()
{
    // 先处理当前焦点，再解除文本框与视口的联系
    SetPanelOpen(TEXT("ResidentConversation"),false);
    if (ConversationWidget) { ConversationWidget->RemoveFromParent(); ConversationWidget=nullptr; }
}
