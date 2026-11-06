// 安排出拳时间、释放冲量并发出命中窗口事件
// Copyright Epic Games, Inc. All Rights Reserved.

#include "Combat/DeliveryRagdollCombatComponent.h"

#include "Combat/DeliveryCollisionChannels.h"
#include "Combat/DeliveryImpactReceiver.h"
#include "Player/DeliverPlayerState.h"
#include "Character/DeliveryCharacter.h"
#include "Engine/World.h"

#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "TimerManager.h"
#include "Delivery.h"

UDeliveryRagdollCombatComponent::UDeliveryRagdollCombatComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
    PrimaryComponentTick.TickGroup = TG_PostPhysics;
    // 启用组件复制，以支持命中音效的多播调用
    SetIsReplicatedByDefault(true);
}

// 在各客户端播放服务器确认的命中音效
void UDeliveryRagdollCombatComponent::MulticastPlayPunchHit_Implementation(FVector_NetQuantize10 Location)
{
    if (PunchHitSound)
    {
        UGameplayStatics::PlaySoundAtLocation(this, PunchHitSound, Location);
    }
}

// 连接角色身体，为后续物理出拳准备引用
void UDeliveryRagdollCombatComponent::BeginPlay()
{
    Super::BeginPlay();
    Mesh = GetOwner()->FindComponentByClass<USkeletalMeshComponent>();
    ActiveRagdoll = GetOwner()->FindComponentByClass<UDeliveryActiveRagdollComponent>();
}

// 出拳请求与蓄力

// 记录出拳手和方向，并安排释放、命中与结束时间
bool UDeliveryRagdollCombatComponent::StartPunch(EMeleeHand Hand, FVector AimDir)
{
    // 身体尚未准备好，或上一拳还在收回时，不开始下一拳
    UWorld *World = GetWorld();
    if (bIsPunching || !World || !Mesh || !ActiveRagdoll || !ActiveRagdoll->IsRagdollActive() ||
        ActiveRagdoll->IsPunchRecovering())
    {
        return false;
    }

    // 本次出拳使用按下攻击时的水平方向
    CachedAimDir = FVector(AimDir.X, AimDir.Y, 0.f).GetSafeNormal();
    if (CachedAimDir.IsNearlyZero() || !ActiveRagdoll->CanDrivePunch(Hand == EMeleeHand::Left ? 1.f : -1.f))
    {
        return false;
    }

    ActiveHand = Hand;
    bIsPunching = true;
    bHitWindowOpen = false;
    const float HandSide = Hand == EMeleeHand::Left ? 1.0f : -1.0f;
    ActiveRagdoll->BeginBodyDrivenPunch(CachedAimDir, HandSide);
#if !UE_BUILD_SHIPPING
    UE_LOG(LogDelivery, Log, TEXT("PunchDiagEvent actor=%s time=%.3f event=Begin hand=%d aim=%s"),
           *GetNameSafe(GetOwner()), World->GetTimeSeconds(), int32(Hand), *CachedAimDir.ToCompactString());
#endif

    FTimerManager &TimerManager = World->GetTimerManager();
    // 动作计时只负责释放与结束，是否命中由有效窗口中的实际拳路决定
    const float ReleaseTime = FMath::Max(WindupDelay, 0.05f);
    TimerManager.SetTimer(PunchDriveTimer, this, &UDeliveryRagdollCombatComponent::FirePunchDrive,
                          ReleaseTime, false);
    TimerManager.SetTimer(EndTimer, this, &UDeliveryRagdollCombatComponent::FirePunchEnded,
                          FMath::Max(PunchEndDelay, ReleaseTime + 0.06f), false);

    return true;
}

// 清除本次攻击的计时器并恢复身体状态
void UDeliveryRagdollCombatComponent::CancelPunch()
{
    // 晕倒或其他动作打断时清掉未触发的事件，避免角色倒地后仍突然出拳
    if (!bIsPunching)
        return;
#if !UE_BUILD_SHIPPING
    UE_LOG(LogDelivery, Log, TEXT("PunchDiagEvent actor=%s time=%.3f event=Cancel hand=%d"),
           *GetNameSafe(GetOwner()), GetWorld()->GetTimeSeconds(), int32(ActiveHand));
#endif
    GetWorld()->GetTimerManager().ClearTimer(PunchDriveTimer);
    GetWorld()->GetTimerManager().ClearTimer(EndTimer);
    bHitWindowOpen = false;
    SetComponentTickEnabled(false);
    ActiveRagdoll->EndBodyDrivenPunch();
    bIsPunching = false;
}

// 释放拳头与命中窗口

// 释放拳头并施加起始冲量，本机同时播放出拳声音
void UDeliveryRagdollCombatComponent::FirePunchDrive()
{
    if (!bIsPunching)
        return;
    ActiveRagdoll->ReleaseBodyDrivenPunch();
    if (GetOwner()->HasAuthority())
    {
        // 从释放时的拳头位置开始记录，避免把蓄力阶段也算作攻击
        PreviousTraceCenter = GetPunchTraceTransform().GetLocation();
        bHitWindowOpen = true;
        SetComponentTickEnabled(true);
    }
#if !UE_BUILD_SHIPPING
    UE_LOG(LogDelivery, Log, TEXT("PunchDiagEvent actor=%s time=%.3f event=Release hand=%d impulse=%.1f"),
           *GetNameSafe(GetOwner()), GetWorld()->GetTimeSeconds(), int32(ActiveHand), PunchImpulse);
#endif

    // 姿势电机负责轨迹，冲量负责让拳头一开始就带着速度出去
    // 三段手臂分别使用可配置的倍率；默认让手部获得最大速度增量
    const bool bLeft = ActiveHand == EMeleeHand::Left;
    Mesh->AddImpulse(CachedAimDir * PunchImpulse * FMath::Max(0.f, HandImpulseScale),
                     bLeft ? LeftHandBone : RightHandBone, true);
    Mesh->AddImpulse(CachedAimDir * PunchImpulse * FMath::Max(0.f, ForearmImpulseScale),
                     bLeft ? LeftForearmBone : RightForearmBone, true);
    Mesh->AddImpulse(CachedAimDir * PunchImpulse * FMath::Max(0.f, UpperArmImpulseScale),
                     bLeft ? LeftArmBone : RightArmBone, true);

    // 操作音跟本机预测走，左右键反馈不会多等一次网络往返
    const APawn *OwnerPawn = Cast<APawn>(GetOwner());
    USoundBase *PunchSound = bLeft ? LeftPunchSound : RightPunchSound;
    if (OwnerPawn && OwnerPawn->IsLocallyControlled() && PunchSound)
    {
        UGameplayStatics::PlaySoundAtLocation(this, PunchSound,
                                              Mesh->GetBoneLocation(bLeft ? LeftHandBone : RightHandBone));
    }
}

// 计算本次出拳用于命中查询的位置和方向
FTransform UDeliveryRagdollCombatComponent::GetPunchTraceTransform() const
{
    // 在拳头位置沿出拳方向偏移，得到命中检测的中心
    const FName HandBone = ActiveHand == EMeleeHand::Left ? LeftHandBone : RightHandBone;
    const FVector Location =
        Mesh->GetBoneLocation(HandBone, EBoneSpaces::WorldSpace) + CachedAimDir * ArmReachOffset;
    return FTransform(CachedAimDir.ToOrientationQuat(), Location);
}

// 物理后记录拳头路径，再通知技能调用 GatherHits 查询并结算
// 两帧位置之间做扫掠，减少只检查当前拳头位置造成的漏检
void UDeliveryRagdollCombatComponent::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!bIsPunching || !bHitWindowOpen || !GetOwner()->HasAuthority())
        return;

    TraceStart = PreviousTraceCenter;
    TraceEnd = GetPunchTraceTransform().GetLocation();
    PreviousTraceCenter = TraceEnd;
    // 窗口内每帧通知一次，同一拳的重复目标由技能中的 HitActors 排除
    OnPunchHitWindow.Broadcast(ActiveHand);
}

// 结束出拳状态，通知技能清理本次攻击
void UDeliveryRagdollCombatComponent::FirePunchEnded()
{
    if (!bIsPunching)
        return;
#if !UE_BUILD_SHIPPING
    UE_LOG(LogDelivery, Log, TEXT("PunchDiagEvent actor=%s time=%.3f event=End hand=%d"),
           *GetNameSafe(GetOwner()), GetWorld()->GetTimeSeconds(), int32(ActiveHand));
#endif
    bHitWindowOpen = false;
    SetComponentTickEnabled(false);
    ActiveRagdoll->EndBodyDrivenPunch();
    bIsPunching = false;
    // 检测已关闭，技能收到通知后结束并解绑；手臂姿态继续完成收拳
    OnPunchEnded.Broadcast(ActiveHand);
}

// 技能调用与命中收集

// 根据身体朝向发动出拳，不使用镜头方向改变拳路
bool UDeliveryRagdollCombatComponent::StartAttack(EMeleeHand Hand)
{
    const ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(GetOwner());
    if (!Character)
        return false;
    const UDeliveryActiveRagdollComponent *Body = Character->GetActiveRagdoll();
    const FVector Direction =
        Body ? Body->GetBodyForward() : Character->GetActorForwardVector().GetSafeNormal2D();
    return !Direction.IsNearlyZero() && StartPunch(Hand, Direction);
}
// 在服务器查询命中的角色，同一角色只返回一次
TArray<AActor *> UDeliveryRagdollCombatComponent::GatherHits(EMeleeHand Hand) const
{
    TArray<AActor *> Results;
    const AActor *Owner = GetOwner();
    if (!Owner || !Owner->HasAuthority() || !GetWorld() || !bHitWindowOpen || Hand != ActiveHand)
    {
        return Results;
    }
    TArray<FHitResult> Hits;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(PunchHit), false, Owner);
    // 球体覆盖拳头从上一帧到本帧的移动，遇到阻挡时不继续穿过后方目标
    GetWorld()->SweepMultiByChannel(Hits, TraceStart, TraceEnd, FQuat::Identity, ECC_PhysicsBody,
                                    FCollisionShape::MakeSphere(GetHitRadius()), Params);

    for (const FHitResult& Hit : Hits)
    {
        ADeliveryCharacter *Other = Cast<ADeliveryCharacter>(Hit.GetActor());
        if (Other && Other != Owner && Other->GetPlayerState<ADeliverPlayerState>() &&
            Other->GetActiveRagdoll() && Other->GetActiveRagdoll()->IsRagdollActive())
        {
            // 本次查询中合并同一角色的多个刚体；跨帧去重由技能负责
            Results.AddUnique(Other);
        }
    }

    // 路人**另开一次扫掠**，不并进上面那一次。
    // 两个原因：①路人在自己的碰撞通道上（见 DeliveryCollisionChannels.h），
    // ②上面那条查询是调通过的，往里加通道会改变它的命中集合——
    // 摩托车受击那次就是因为这个才单开一次 ECC_Pawn 查询。
    TArray<FHitResult> PedestrianHits;
    GetWorld()->SweepMultiByChannel(PedestrianHits, TraceStart, TraceEnd, FQuat::Identity,
                                    DeliveryCollision::Pedestrian,
                                    FCollisionShape::MakeSphere(GetHitRadius()), Params);
    for (const FHitResult& Hit : PedestrianHits)
    {
        AActor *HitActor = Hit.GetActor();
        if (!HitActor || HitActor == Owner)
        {
            continue;
        }
        const IDeliveryImpactReceiver *Receiver = Cast<IDeliveryImpactReceiver>(HitActor);
        // 已经躺下的不再算进命中：否则一拳打在尸体上也会播命中音效
        if (Receiver && !Receiver->IsKnockedDown())
        {
            Results.AddUnique(HitActor);
        }
    }

    return Results;
}
