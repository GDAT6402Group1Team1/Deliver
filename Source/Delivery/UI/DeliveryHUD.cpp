#include "UI/DeliveryHUD.h"
#include "UI/DeliveryConversationWidget.h"
#include "UI/DeliveryTrunkStatusWidget.h"
#include "Vehicle/DeliveryTrunkPassengerComponent.h"
#include "UI/DeliveryStartupWidget.h"
#include "UI/DeliveryHotbarWidget.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "GameFramework/PlayerController.h"
#include "Player/DeliveryPlayerController.h"
#include "UI/Controllers/DeliveryPhoneController.h"
#include "TimerManager.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Blueprint/GameViewportSubsystem.h"

ADeliveryHUD::ADeliveryHUD()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickInterval = 0.1f;
    HotbarWidgetClass = FSoftClassPath(TEXT("/Game/UI/Inventory/WBP_DeliveryHotbar.WBP_DeliveryHotbar_C"));
    ConversationWidgetClass = FSoftClassPath(TEXT("/Game/UI/Conversation/WBP_Conversation.WBP_Conversation_C"));
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
    RefreshHotbar();
    RefreshTrunkStatus();
    RefreshConversationLayout();
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
    if (StartupWidget) StartupWidget->RemoveFromParent();
    StartupWidget = nullptr;
    SetPanelOpen(TEXT("Startup"), false);
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
    ConversationWidget->AddToViewport(40);
    // 两轴都设为零时，视口采用 UMG 内容的实际高度，宽度由内部 SizeBox 限制
    ConversationWidget->SetDesiredSizeInViewport(FVector2D::ZeroVector);
    ConversationWidget->SetAlignmentInViewport(FVector2D(.5f,1.f));
    RefreshConversationLayout();
    SetPanelOpen(TEXT("ResidentConversation"),true);
    ConversationWidget->FocusInput();
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
    if (ConversationWidget) { ConversationWidget->RemoveFromParent(); ConversationWidget=nullptr; }
    SetPanelOpen(TEXT("ResidentConversation"),false);
}
