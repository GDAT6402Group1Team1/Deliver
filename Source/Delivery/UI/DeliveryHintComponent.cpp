#include "UI/DeliveryHintComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Input/DeliveryInputComponent.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Vehicle/DeliveryVehicleSummonComponent.h"
#include "Interaction/DeliveryPromptSubsystem.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"

UDeliveryHintComponent::UDeliveryHintComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    ReadyText = NSLOCTEXT("Delivery", "SummonReady", "Press {0} to Summon Bike");
    CooldownText = NSLOCTEXT("Delivery", "SummonCooling", "Summon Cooldown: {0}s");
}

void UDeliveryHintComponent::BeginPlay()
{
    Super::BeginPlay();
    Player = Cast<APlayerController>(GetOwner());
    if (!Player) return;
    // 服务器切换控制对象或客户端收到新 Pawn 时，重新连接角色和车辆的通知
    Player->OnPossessedPawnChanged.AddUniqueDynamic(this, &UDeliveryHintComponent::HandlePawnChanged);
    BindInput();
    HandlePawnChanged(nullptr, Player->GetPawn());
    SpawnHandle = GetWorld()->AddOnActorSpawnedHandler(
        FOnActorSpawned::FDelegate::CreateUObject(this, &UDeliveryHintComponent::HandleVehicleChanged));
    DestroyHandle = GetWorld()->AddOnActorDestroyedHandler(
        FOnActorDestroyed::FDelegate::CreateUObject(this, &UDeliveryHintComponent::HandleVehicleChanged));
}

void UDeliveryHintComponent::BindInput()
{
    Player = Cast<APlayerController>(GetOwner());
    if (!Player || !Player->IsLocalController()) return;
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
    RefreshHint();
}

void UDeliveryHintComponent::HandleVehicleEvent(EDeliveryEvent Event)
{
    if (Event == EDeliveryEvent::CameraChanged) RefreshHint();
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
    if (auto* Bike = Cast<ADeliveryMotorbike>(Player->GetPawn()))
    {
        const FText Key = GetKeyText(Bike->GetCameraToggleAction());
        if (Key.IsEmpty()) { ShowHint(FText::GetEmpty()); return; }
        const FText Format = Bike->IsFreeLookCamera()
            ? NSLOCTEXT("Delivery", "MotorbikeHintFree", "Press {0} to Switch Camera (Free)")
            : NSLOCTEXT("Delivery", "MotorbikeHintFixed", "Press {0} to Switch Camera (Fixed)");
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
