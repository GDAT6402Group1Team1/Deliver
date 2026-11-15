#include "UI/DeliveryHintComponent.h"
#include "AI/Companion/DeliveryCompanionEncounterScene.h"
#include "Character/DeliveryCharacter.h"
#include "Components/SceneComponent.h"
#include "Input/DeliveryInputComponent.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Vehicle/DeliveryVehicleSummonComponent.h"
#include "Interaction/DeliveryPromptSubsystem.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"

UDeliveryHintComponent::UDeliveryHintComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    ReadyText = NSLOCTEXT("Delivery", "SummonReady", "按 {0} 召唤摩托车");
    CooldownText = NSLOCTEXT("Delivery", "SummonCooling", "召唤冷却 {0} 秒");
}

void UDeliveryHintComponent::BeginPlay()
{
    Super::BeginPlay();
    Player = Cast<APlayerController>(GetOwner());
    if (!Player) return;
    // 服务器切换控制对象或客户端收到新 Pawn 时，重新连接角色和车辆的通知
    Player->OnPossessedPawnChanged.AddUniqueDynamic(this, &UDeliveryHintComponent::HandlePawnChanged);
    SpawnHandle = GetWorld()->AddOnActorSpawnedHandler(
        FOnActorSpawned::FDelegate::CreateUObject(this, &UDeliveryHintComponent::HandleActorSpawned));
    DestroyHandle = GetWorld()->AddOnActorDestroyedHandler(
        FOnActorDestroyed::FDelegate::CreateUObject(this, &UDeliveryHintComponent::HandleVehicleChanged));
    BindInput();
    HandlePawnChanged(nullptr, Player->GetPawn());
}

void UDeliveryHintComponent::BindInput()
{
    Player = Cast<APlayerController>(GetOwner());
    if (!Player || !Player->IsLocalController() || !GetWorld()) return;
    if (!bEncounterScenesInitialized)
    {
        bEncounterScenesInitialized = true;
        for (TActorIterator<ADeliveryCompanionEncounterScene> It(GetWorld()); It; ++It)
            BindEncounterScene(*It);
        UpdateEncounterHint();
    }
    auto* NewInput = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Player->GetLocalPlayer());
    if (Input != NewInput)
    {
        if (Input) Input->ControlMappingsRebuiltDelegate.RemoveDynamic(this, &UDeliveryHintComponent::RefreshHint);
        Input = NewInput;
        if (Input) Input->ControlMappingsRebuiltDelegate.AddUniqueDynamic(this, &UDeliveryHintComponent::RefreshHint);
    }
    RefreshHint();
}

void UDeliveryHintComponent::UnbindPawn()
{
    BindEncounterPawnRoot(nullptr);
    if (Summon) Summon->OnCooldownChanged.RemoveDynamic(this, &UDeliveryHintComponent::RefreshHint);
    if (VehicleEvents) VehicleEvents->OnEvent.RemoveDynamic(this, &UDeliveryHintComponent::HandleVehicleEvent);
    Summon = nullptr;
    VehicleEvents = nullptr;
}

void UDeliveryHintComponent::HandlePawnChanged(APawn* OldPawn, APawn* NewPawn)
{
    UnbindPawn();
    if (auto* Character = Cast<ADeliveryCharacter>(NewPawn))
    {
        Summon = Character->FindComponentByClass<UDeliveryVehicleSummonComponent>();
        if (Summon) Summon->OnCooldownChanged.AddUniqueDynamic(this, &UDeliveryHintComponent::RefreshHint);
    }
    else if (auto* Bike = Cast<ADeliveryMotorbike>(NewPawn))
    {
        VehicleEvents = Bike->FindComponentByClass<UDeliveryEventComponent>();
        if (VehicleEvents) VehicleEvents->OnEvent.AddUniqueDynamic(this, &UDeliveryHintComponent::HandleVehicleEvent);
    }
    UpdateEncounterHint();
    RefreshHint();
}

void UDeliveryHintComponent::HandleVehicleEvent(EDeliveryEvent Event)
{
    if (Event == EDeliveryEvent::CameraChanged) RefreshHint();
}

void UDeliveryHintComponent::HandleActorSpawned(AActor* Actor)
{
    if (!Player || !Player->IsLocalController()) return;
    if (auto* Scene = Cast<ADeliveryCompanionEncounterScene>(Actor))
    {
        BindEncounterScene(Scene);
        UpdateEncounterHint();
    }
    else HandleVehicleChanged(Actor);
}

void UDeliveryHintComponent::BindEncounterScene(ADeliveryCompanionEncounterScene* Scene)
{
    if (!IsValid(Scene) || Scene->IsActorBeingDestroyed() || Scene->GetWorld() != GetWorld()) return;
    const TWeakObjectPtr<ADeliveryCompanionEncounterScene> WeakScene(Scene);
    if (EncounterScenes.Contains(WeakScene)) return;
    EncounterScenes.Add(WeakScene);
    Scene->OnEncounterObstructionChanged.AddUObject(this, &UDeliveryHintComponent::UpdateEncounterHint);
    Scene->OnEndPlay.AddUniqueDynamic(this, &UDeliveryHintComponent::HandleEncounterEndPlay);
}

void UDeliveryHintComponent::HandleEncounterEndPlay(AActor* Actor, EEndPlayReason::Type Reason)
{
    auto* Scene = CastChecked<ADeliveryCompanionEncounterScene>(Actor);
    Scene->OnEncounterObstructionChanged.RemoveAll(this);
    Scene->OnEndPlay.RemoveDynamic(this, &UDeliveryHintComponent::HandleEncounterEndPlay);
    EncounterScenes.Remove(TWeakObjectPtr<ADeliveryCompanionEncounterScene>(Scene));
    UpdateEncounterHint();
}

void UDeliveryHintComponent::BindEncounterPawnRoot(USceneComponent* Root)
{
    if (Root && EncounterPawnRoot.Get() == Root && EncounterPawnTransformHandle.IsValid()) return;
    if (auto* Previous = EncounterPawnRoot.Get()) Previous->TransformUpdated.Remove(EncounterPawnTransformHandle);
    EncounterPawnTransformHandle.Reset();
    EncounterPawnRoot = Root;
    if (Root)
        EncounterPawnTransformHandle = Root->TransformUpdated.AddWeakLambda(this,
            [this](USceneComponent*, EUpdateTransformFlags, ETeleportType) { UpdateEncounterHint(); });
}

void UDeliveryHintComponent::UpdateEncounterHint()
{
    if (!Player || !Player->IsLocalController() || !GetWorld()) return;
    const APawn* Pawn = Player->GetPawn();
    const bool bCanSeeHint = IsValid(Pawn) && !Pawn->IsActorBeingDestroyed() && Pawn->GetWorld() == GetWorld() &&
        (Pawn->IsA<ADeliveryCharacter>() || Pawn->IsA<ADeliveryMotorbike>());
    bool bAnyObstruction = false;
    double NearestDistance = FMath::Square(800.0);
    auto NearestObstruction = EDeliveryCompanionEncounterObstruction::None;
    for (const auto& WeakScene : EncounterScenes)
    {
        const auto* Scene = WeakScene.Get();
        if (!IsValid(Scene) || Scene->IsActorBeingDestroyed()) continue;
        const auto Obstruction = Scene->GetEncounterObstruction();
        if (Obstruction == EDeliveryCompanionEncounterObstruction::None) continue;
        bAnyObstruction = true;
        if (!bCanSeeHint) continue;
        const double Distance = FVector::DistSquared(Pawn->GetActorLocation(), Scene->GetTriggerWorldLocation());
        if (Distance <= NearestDistance)
        {
            NearestDistance = Distance;
            NearestObstruction = Obstruction;
        }
    }
    // 仅在有受阻场景时监听玩家位置，离开提示范围立刻恢复原提示
    BindEncounterPawnRoot(bAnyObstruction && bCanSeeHint ? Pawn->GetRootComponent() : nullptr);
    FText Text;
    if (NearestObstruction == EDeliveryCompanionEncounterObstruction::Doorway)
        Text = NSLOCTEXT("Delivery", "NoodleShopDoorwayBlocked", "请让开面馆门口。");
    else if (NearestObstruction == EDeliveryCompanionEncounterObstruction::EjectionPath)
        Text = NSLOCTEXT("Delivery", "NoodleShopEjectionPathBlocked", "给二狗在门外留点空地。");
    if (Text.EqualTo(EncounterHint)) return;
    EncounterHint = MoveTemp(Text);
    RefreshHint();
}

// 地图车辆增减也会影响能否召唤，等生成或销毁完成后再查询
void UDeliveryHintComponent::HandleVehicleChanged(AActor* Actor)
{
    if (!Player || !Player->IsLocalController() || !Cast<ADeliveryMotorbike>(Actor)) return;
    GetWorld()->GetTimerManager().ClearTimer(RefreshTimer);
    RefreshTimer = GetWorld()->GetTimerManager().SetTimerForNextTick(this, &UDeliveryHintComponent::RefreshHint);
}

void UDeliveryHintComponent::SetInputDevice(bool bGamepad)
{
    if (bUsingGamepad == bGamepad) return;
    bUsingGamepad = bGamepad;
    RefreshHint();
}

FText UDeliveryHintComponent::GetKeyText(const UInputAction* Action) const
{
    if (!Input || !Action) return FText::GetEmpty();
    for (const FKey& Key : Input->QueryKeysMappedToAction(Action))
    {
        if (Key.IsGamepadKey() == bUsingGamepad) return Key.GetDisplayName(false);
    }
    return FText::GetEmpty();
}

void UDeliveryHintComponent::RefreshHint()
{
    if (!Player || !Player->IsLocalController() || !GetWorld()) return;
    GetWorld()->GetTimerManager().ClearTimer(CountdownTimer);
    if (!EncounterHint.IsEmpty()) { ShowHint(EncounterHint); return; }
    if (auto* Bike = Cast<ADeliveryMotorbike>(Player->GetPawn()))
    {
        const FText Key = GetKeyText(Bike->GetCameraToggleAction());
        if (Key.IsEmpty()) { ShowHint(FText::GetEmpty()); return; }
        const FText Format = Bike->IsFreeLookCamera()
            ? NSLOCTEXT("Delivery", "MotorbikeHintFree", "按 {0} 切换视角（当前：自由）")
            : NSLOCTEXT("Delivery", "MotorbikeHintFixed", "按 {0} 切换视角（当前：固定）");
        ShowHint(FText::Format(Format, Key));
        return;
    }
    auto* Character = Cast<ADeliveryCharacter>(Player->GetPawn());
    if (!Character || !Summon || !Summon->CanOfferSummon())
    {
        ShowHint(FText::GetEmpty());
        return;
    }
    const FText Key = GetKeyText(Character->GetPlayerInput()->GetSummonAction());
    if (Key.IsEmpty()) { ShowHint(FText::GetEmpty()); return; }
    const float Remaining = Summon->GetCooldownRemaining();
    if (Remaining > 0.0f)
    {
        ShowHint(FText::Format(CooldownText, FText::AsNumber(FMath::CeilToInt(Remaining))), true);
        // 每次用服务器结束时间重算，卡顿时不会把倒计时越拖越长
        GetWorld()->GetTimerManager().SetTimer(CountdownTimer, this, &UDeliveryHintComponent::RefreshHint,
                                             FMath::Max(0.01f, FMath::Min(Remaining, 1.0f)), false);
    }
    else ShowHint(FText::Format(ReadyText, Key));
}

void UDeliveryHintComponent::ShowHint(const FText& Text, bool bDimmed)
{
    CurrentText = Text;
    if (auto* Prompt = UDeliveryPromptSubsystem::Get(this))
    {
        if (Text.IsEmpty()) Prompt->ClearCornerHint(this);
        else Prompt->SetCornerHint(this, Text, bDimmed);
    }
}

void UDeliveryHintComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UnbindPawn();
    for (const auto& WeakScene : EncounterScenes)
    {
        if (auto* Scene = WeakScene.Get())
        {
            Scene->OnEncounterObstructionChanged.RemoveAll(this);
            Scene->OnEndPlay.RemoveDynamic(this, &UDeliveryHintComponent::HandleEncounterEndPlay);
        }
    }
    EncounterScenes.Reset();
    EncounterHint = FText::GetEmpty();
    if (Player) Player->OnPossessedPawnChanged.RemoveDynamic(this, &UDeliveryHintComponent::HandlePawnChanged);
    if (Input) Input->ControlMappingsRebuiltDelegate.RemoveDynamic(this, &UDeliveryHintComponent::RefreshHint);
    if (UWorld* World = GetWorld())
    {
        World->RemoveOnActorSpawnedHandler(SpawnHandle);
        World->RemoveOnActorDestroyedHandler(DestroyHandle);
        World->GetTimerManager().ClearTimer(CountdownTimer);
        World->GetTimerManager().ClearTimer(RefreshTimer);
    }
    ShowHint(FText::GetEmpty());
    Super::EndPlay(EndPlayReason);
}
