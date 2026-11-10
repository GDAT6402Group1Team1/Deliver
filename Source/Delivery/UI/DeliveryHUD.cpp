#include "UI/DeliveryHUD.h"
#include "UI/DeliveryStartupWidget.h"
#include "UI/DeliveryHotbarWidget.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "GameFramework/PlayerController.h"
#include "Player/DeliveryPlayerController.h"
#include "UI/Controllers/DeliveryPhoneController.h"
#include "TimerManager.h"

ADeliveryHUD::ADeliveryHUD()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickInterval = 0.1f;
    HotbarWidgetClass = FSoftClassPath(TEXT("/Game/UI/Inventory/WBP_DeliveryHotbar.WBP_DeliveryHotbar_C"));
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
