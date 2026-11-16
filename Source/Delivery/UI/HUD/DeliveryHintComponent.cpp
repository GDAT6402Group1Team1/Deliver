#include "UI/HUD/DeliveryHintComponent.h"
#include "AI/Companion/Encounter/DeliveryCompanionEncounterScene.h"
#include "Character/DeliveryCharacter.h"
#include "Components/SceneComponent.h"
#include "Input/DeliveryInputComponent.h"
#include "Vehicle/Motorbike/DeliveryMotorbike.h"
#include "Vehicle/Summon/DeliveryVehicleSummonComponent.h"
#include "Interaction/DeliveryPromptSubsystem.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"

// ── 生命周期 ──

/// @brief 不需要每帧更新；设置召唤的默认提示文字
UDeliveryHintComponent::UDeliveryHintComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    ReadyText = NSLOCTEXT("Delivery", "SummonReady", "按 {0} 召唤摩托车");
    CooldownText = NSLOCTEXT("Delivery", "SummonCooling", "召唤冷却 {0} 秒");
}

/// @brief 订阅控制对象变化和 Actor 生成、销毁，调用 BindInput，再按当前控制对象调用 HandlePawnChanged
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

// ── 刷新 ──

/// @brief 本机：第一次调用时订阅场景里所有二狗初遇场景；订阅 Enhanced Input 的映射重建通知，然后调用 RefreshHint
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

// ── 通知 ──

/// @brief 取消对旧控制对象（召唤组件、车辆事件、身体位置）的订阅
void UDeliveryHintComponent::UnbindPawn()
{
    BindEncounterPawnRoot(nullptr);
    if (Summon) Summon->OnCooldownChanged.RemoveDynamic(this, &UDeliveryHintComponent::RefreshHint);
    if (VehicleEvents) VehicleEvents->OnEvent.RemoveDynamic(this, &UDeliveryHintComponent::HandleVehicleEvent);
    Summon = nullptr;
    VehicleEvents = nullptr;
}

/// @brief 控制对象换了：取消旧对象的订阅；新对象是角色时订阅召唤组件的冷却变化，是摩托车时订阅车辆事件
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

/// @brief 车辆事件：切换视角 CameraChanged 时调用 RefreshHint
void UDeliveryHintComponent::HandleVehicleEvent(EDeliveryEvent Event)
{
    if (Event == EDeliveryEvent::CameraChanged) RefreshHint();
}

/// @brief 生成了初遇场景就订阅它；生成了摩托车就调用 HandleVehicleChanged
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

/// @brief 订阅初遇场景的受阻变化和销毁
void UDeliveryHintComponent::BindEncounterScene(ADeliveryCompanionEncounterScene* Scene)
{
    if (!IsValid(Scene) || Scene->IsActorBeingDestroyed() || Scene->GetWorld() != GetWorld()) return;
    const TWeakObjectPtr<ADeliveryCompanionEncounterScene> WeakScene(Scene);
    if (EncounterScenes.Contains(WeakScene)) return;
    EncounterScenes.Add(WeakScene);
    Scene->OnEncounterObstructionChanged.AddUObject(this, &UDeliveryHintComponent::UpdateEncounterHint);
    Scene->OnEndPlay.AddUniqueDynamic(this, &UDeliveryHintComponent::HandleEncounterEndPlay);
}

/// @brief 初遇场景被销毁时取消订阅，再调用 UpdateEncounterHint
void UDeliveryHintComponent::HandleEncounterEndPlay(AActor* Actor, EEndPlayReason::Type Reason)
{
    auto* Scene = CastChecked<ADeliveryCompanionEncounterScene>(Actor);
    Scene->OnEncounterObstructionChanged.RemoveAll(this);
    Scene->OnEndPlay.RemoveDynamic(this, &UDeliveryHintComponent::HandleEncounterEndPlay);
    EncounterScenes.Remove(TWeakObjectPtr<ADeliveryCompanionEncounterScene>(Scene));
    UpdateEncounterHint();
}

// ── 二狗初遇 ──

/// @brief 订阅玩家身体的位置变化，玩家走动时调用 UpdateEncounterHint；传入空时取消订阅
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

/// @brief 找到 800 cm 内最近的、被挡住的初遇场景：挡着门口提示让开门口，挡着弹出路线提示留出空地
/// 只有存在受阻场景时才订阅玩家位置；提示变了才调用 RefreshHint
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

// ── 通知 ──

/// @brief 地图车辆增减也会影响能否召唤，等生成或销毁完成后再查询
void UDeliveryHintComponent::HandleVehicleChanged(AActor* Actor)
{
    if (!Player || !Player->IsLocalController() || !Cast<ADeliveryMotorbike>(Actor)) return;
    GetWorld()->GetTimerManager().ClearTimer(RefreshTimer);
    RefreshTimer = GetWorld()->GetTimerManager().SetTimerForNextTick(this, &UDeliveryHintComponent::RefreshHint);
}

// ── 刷新 ──

/// @brief 玩家换了输入设备（手柄或键鼠），换了就调用 RefreshHint 显示对应的键名
void UDeliveryHintComponent::SetInputDevice(bool bGamepad)
{
    if (bUsingGamepad == bGamepad) return;
    bUsingGamepad = bGamepad;
    RefreshHint();
}

/// @brief Action 在当前设备上映射的键名；没有映射时返回空
FText UDeliveryHintComponent::GetKeyText(const UInputAction* Action) const
{
    if (!Input || !Action) return FText::GetEmpty();
    for (const FKey& Key : Input->QueryKeysMappedToAction(Action))
    {
        if (Key.IsGamepadKey() == bUsingGamepad) return Key.GetDisplayName(false);
    }
    return FText::GetEmpty();
}

/// @brief 按当前状态重新算提示文字：初遇提示优先，其次骑车的视角提示，最后步行的召唤提示
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

/// @brief 记下文字，交给 DeliveryPromptSubsystem 显示；文字为空时清掉提示；bDimmed 为 true 时显示成灰色（冷却中）
void UDeliveryHintComponent::ShowHint(const FText& Text, bool bDimmed)
{
    CurrentText = Text;
    if (auto* Prompt = UDeliveryPromptSubsystem::Get(this))
    {
        if (Text.IsEmpty()) Prompt->ClearCornerHint(this);
        else Prompt->SetCornerHint(this, Text, bDimmed);
    }
}

// ── 生命周期 ──

/// @brief 取消所有订阅和计时器，清掉左下角提示
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
