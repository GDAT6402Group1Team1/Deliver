// 交通车负责服务器距离校验，车辆音效组件负责本机空间播放
#include "Traffic/DeliveryTrafficCarComponent.h"
#include "Audio/DeliveryVehicleAudioComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpectatorPawn.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"

bool UDeliveryTrafficCarComponent::TryHonk()
{
    AActor* Owner = GetOwner();
    UWorld* World = GetWorld();
    // 交通组件一直靠 Tick 工作，旧蓝图没有启用 Auto Activate，不能用 IsActive 判断它是否在运行
    if (!IsRegistered() || !World || !Owner || !Owner->HasAuthority() || !HasBegunPlay()
        || Owner->IsActorBeingDestroyed() || bRouteLostPending || bScaleInRunning || bScaleOutRunning
        || World->GetTimeSeconds() < NextHornTime || HornVolume <= 0.0f)
    {
        return false;
    }

    TArray<USoundBase*> Candidates;
    for (USoundBase* Sound : HornSounds)
    {
        if (IsValid(Sound) && !Sound->IsLooping())
        {
            Candidates.AddUnique(Sound);
        }
    }
    if (Candidates.Num() > 1)
    {
        Candidates.Remove(LastHornSound.Get());
    }
    if (Candidates.IsEmpty())
    {
        return false;
    }

    USoundBase* Sound = Candidates[FMath::RandRange(0, Candidates.Num() - 1)];
    // 在服务器只抽样一次，所有接收者使用同一组音量和音调；反填的上下限也能使用
    const auto SampleRange = [](const FVector2D& Range, float LowerBound, float UpperBound)
    {
        const float A = FMath::Clamp(static_cast<float>(Range.X), LowerBound, UpperBound);
        const float B = FMath::Clamp(static_cast<float>(Range.Y), LowerBound, UpperBound);
        return FMath::FRandRange(FMath::Min(A, B), FMath::Max(A, B));
    };
    const float Volume = HornVolume * SampleRange(HornVolumeRange, 0.0f, 2.0f);
    const float Pitch = SampleRange(HornPitchRange, 0.5f, 2.0f);
    LastHornSound = Sound;
    // 降低音调会延长声音，冷却下限跟随实际播放倍率，避免截断上一次鸣笛
    NextHornTime = World->GetTimeSeconds() + FMath::Max3(0.1f, HornCooldown, Sound->GetDuration() / Pitch);
    MulticastPlayHorn(Sound, Volume, Pitch, HornAttenuation);
    return true;
}

void UDeliveryTrafficCarComponent::UpdatePlayerProximityHorn(float DeltaTime)
{
    if (!GetOwner() || !GetOwner()->HasAuthority())
    {
        return;
    }
    if (!bAutoHonkNearPlayers || bRouteLostPending || bScaleInRunning || bScaleOutRunning)
    {
        HonkedPlayers.Reset();
        HornCheckElapsed = 0.0f;
        return;
    }
    HornCheckElapsed += DeltaTime;
    if (HornCheckElapsed < 0.2f)
    {
        return;
    }
    HornCheckElapsed = 0.0f;

    const float TriggerDistance = FMath::Max(0.0f, HornTriggerDistance);
    const float RearmDistance = FMath::Max(TriggerDistance + 1.0f, HornRearmDistance);
    const FVector Forward = GetOwner()->GetActorForwardVector().GetSafeNormal2D();
    const float MinForwardDot = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(HornForwardHalfAngle, 1.0f, 90.0f)));
    TSet<TWeakObjectPtr<APlayerController>> StillInRange;
    TArray<APlayerController*> NearbyPlayers;
    bool bNewPlayerNearby = false;
    for (auto It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
    {
        APlayerController* Player = It->Get();
        const APawn* Pawn = IsValid(Player) ? Player->GetPawn() : nullptr;
        if (!IsValid(Pawn) || Pawn->IsActorBeingDestroyed() || Pawn->IsA<ASpectatorPawn>()
            || Player->IsInState(NAME_Spectating))
        {
            continue;
        }
        // 使用当前 Pawn，步行和骑车走同一套判断，完整三维距离避免高架上下误触发
        const FVector ToPlayer = Pawn->GetActorLocation() - GetOwner()->GetActorLocation();
        const double DistanceSq = ToPlayer.SizeSquared();
        if (DistanceSq <= FMath::Square(RearmDistance))
        {
            StillInRange.Add(Player);
        }
        // 距离仍用三维，方向只看水平车头，坡面和人物高度不改变前方扇区
        const double ForwardDot = FVector::DotProduct(Forward, ToPlayer.GetSafeNormal2D());
        if (DistanceSq <= FMath::Square(TriggerDistance) && ForwardDot > 0.0 && ForwardDot >= MinForwardDot)
        {
            NearbyPlayers.Add(Player);
            bNewPlayerNearby |= !HonkedPlayers.Contains(Player);
        }
    }
    for (auto It = HonkedPlayers.CreateIterator(); It; ++It)
    {
        if (!StillInRange.Contains(*It))
        {
            It.RemoveCurrent();
        }
    }
    if (bNewPlayerNearby && TryHonk())
    {
        // 一次鸣笛覆盖同时在场的玩家，冷却拒绝时保留等待资格
        for (APlayerController* Player : NearbyPlayers)
        {
            HonkedPlayers.Add(Player);
        }
    }
}

void UDeliveryTrafficCarComponent::MulticastPlayHorn_Implementation(
    USoundBase* Sound, float Volume, float Pitch, USoundAttenuation* Attenuation)
{
    if (GetNetMode() == NM_DedicatedServer || !IsValid(Sound))
    {
        return;
    }
    if (IsValid(TrafficAudio))
    {
        TrafficAudio->PlayHorn(Sound, Volume, Pitch, Attenuation);
    }
    OnHornPlayed.Broadcast(Sound);
}

void UDeliveryTrafficCarComponent::MulticastStopHorn_Implementation()
{
    if (IsValid(TrafficAudio))
    {
        TrafficAudio->StopHorn();
    }
}

void UDeliveryTrafficCarComponent::ResetHornProximity()
{
    HonkedPlayers.Reset();
    HornCheckElapsed = 0.0f;
    if (GetOwner() && GetOwner()->HasAuthority())
    {
        MulticastStopHorn();
    }
    else if (IsValid(TrafficAudio))
    {
        TrafficAudio->StopHorn();
    }
    // 重置位置不解除已有冷却，避免在玩家附近反复重生时连响
}

void UDeliveryTrafficCarComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    RestoreOriginalMaterials();
    FadeMeshes.Reset();
    FadeMaterialSlots.Reset();
    OriginalFadeMaterials.Reset();
    FadeMaterialInstances.Reset();
    if (GetWorld())
    {
        GetWorld()->GetTimerManager().ClearTimer(NextWaveTimer);
    }
    HonkedPlayers.Reset();
    LastHornSound.Reset();
    if (IsValid(TrafficAudio))
    {
        TrafficAudio->StopHorn();
        if (bOwnsTrafficAudio)
        {
            TrafficAudio->DestroyComponent();
        }
    }
    TrafficAudio = nullptr;
    Super::EndPlay(EndPlayReason);
}
