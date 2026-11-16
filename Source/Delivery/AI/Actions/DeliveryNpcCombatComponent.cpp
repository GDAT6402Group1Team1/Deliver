#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "GameplayEffect.h"
#include "Combat/Types/DeliveryCombatEvents.h"
#include "Combat/DeliveryRagdollCombatComponent.h"
#include "Misc/ScopeExit.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Actions/DeliveryVehicleHoldComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Combat/Types/DeliveryCollisionChannels.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "AIController.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

namespace
{
    // 临时身体反应位于动作和倾听之上，结束后原来的焦点自动重新生效
    constexpr EAIFocusPriority::Type HitReactionFocus=EAIFocusPriority::LastFocusPriority+3;
    constexpr double HitFacingMaximumSeconds=1.0;
    constexpr float HitFacingArrivalDegrees=5.f;

    float ClampFightApproachOverride(float Range)
    {
        return FMath::IsFinite(Range)?FMath::Clamp(Range,0.f,1200.f):0.f;
    }

    FVector2f GetStrikeBodySize(const AActor& Actor)
    {
        // 主角站立时也在模拟布娃娃，停用胶囊碰撞后仍用它的体型确定靠近距离
        if (const auto* Player=Cast<ADeliveryCharacter>(&Actor); Player && Player->GetCapsuleComponent())
        {
            const auto* Capsule=Player->GetCapsuleComponent();
            return FVector2f(Capsule->GetScaledCapsuleRadius(),Capsule->GetScaledCapsuleHalfHeight());
        }
        FVector2f Size;
        Actor.GetSimpleCollisionCylinder(Size.X,Size.Y);
        return Size;
    }
}

// ── 生命周期 ──
// BeginPlay 在服务器上订阅网格的 OnBoneTransformsFinalized，网格每帧算完姿势后调用 OnStrikePoseUpdated 检查拳头

// ── 生命周期 ──
// BeginPlay 在服务器上订阅网格的 OnBoneTransformsFinalized，网格每帧算完姿势后调用 OnStrikePoseUpdated 检查拳头
// TickComponent 在服务器上推进受击转身、出拳超时、扶起复核和倒地计时

/// @brief 组件要同步给客户端，并每帧更新
UDeliveryNpcCombatComponent::UDeliveryNpcCombatComponent()
{
    SetIsReplicatedByDefault(true); PrimaryComponentTick.bCanEverTick=true;
}

/// @brief 服务器：记下网格 StrikeMesh，订阅它的 OnBoneTransformsFinalized，每帧姿势算完后调用 OnStrikePoseUpdated
void UDeliveryNpcCombatComponent::BeginPlay()
{
    Super::BeginPlay();
    if (!GetOwner()->HasAuthority()) return;
    if (const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        StrikeMesh=Npc->GetMesh();

        // 战斗组件先更新状态，网格算完这一帧的姿势后再读拳头位置
        StrikePoseHandle=Npc->GetMesh()->RegisterOnBoneTransformsFinalizedDelegate(
            FOnBoneTransformsFinalizedMultiCast::FDelegate::CreateUObject(this,&ThisClass::OnStrikePoseUpdated));
    }
}

// ── 出拳 ──
// StrikeOnce 动作：行为树任务调用 StartStrike，网格每帧算完姿势后 OnStrikePoseUpdated 扫一遍拳头，接触时间段结束时调用 FinishStrike 记下打中没有

/// @brief 身体有出拳用的骨骼和动画
bool UDeliveryNpcCombatComponent::HasStrikePose() const
{
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    return Npc && Npc->Presentation && Npc->Presentation->AttackAnimation && Npc->GetMesh() &&
        Npc->GetMesh()->GetSkeletalMeshAsset() && Npc->GetMesh()->GetBoneIndex(StrikeHandBone)!=INDEX_NONE;
}

/// @brief 走到离目标多近停下来出拳，按双方身体宽度算，只留一小段空隙
float UDeliveryNpcCombatComponent::GetStrikeApproachDistance(const AActor* Target) const
{
    if (!GetOwner() || !Target) return 0;
    const float Bodies=GetStrikeBodySize(*GetOwner()).X+GetStrikeBodySize(*Target).X;
    return FMath::Min(FMath::Max(10.f,StrikeRange),Bodies+FMath::Clamp(StrikeBodyGap,0.f,12.f));
}

/// @brief 距离、遮挡、目标状态都满足
bool UDeliveryNpcCombatComponent::CanStrikeTarget(AActor* Target,float MaxDistance) const
{
    if (!GetOwner() || !GetWorld() || bDown || !IsValid(Target) || Target==GetOwner() || Target->GetWorld()!=GetWorld()) return false;
    const auto* Player=Cast<ADeliveryCharacter>(Target);
    const auto* Receiver=Cast<IDeliveryImpactReceiver>(Target);
    const bool bValidReceiver=Receiver && (!Receiver->IsKnockedDown() || Receiver->CanReceiveHitWhileDown());
    const bool bValidPlayer=Player && Player->GetActiveRagdoll() && Player->GetActiveRagdoll()->GetControlMode()==EDeliveryRagdollControlMode::Active;
    if ((!bValidReceiver && !bValidPlayer) ||
        FVector::DistSquared(Target->GetActorLocation(),GetOwner()->GetActorLocation())>FMath::Square(MaxDistance)) return false;
    FHitResult Hit; FCollisionQueryParams Params(SCENE_QUERY_STAT(ResidentStrike),false,GetOwner());
    return !GetWorld()->LineTraceSingleByChannel(Hit,GetOwner()->GetActorLocation(),Target->GetActorLocation(),ECC_Visibility,Params) || Hit.GetActor()==Target;
}

/// @brief 现在就能打到：离得够近、中间没挡、自己没倒地；冷却在 StartStrike 里再查
bool UDeliveryNpcCombatComponent::CanStrike(AActor* Target) const
{
    if (!IsValid(Target) || !GetOwner() || !HasStrikePose()) return false;
    const float Distance=GetStrikeApproachDistance(Target);
    const float OwnerHeight=GetStrikeBodySize(*GetOwner()).Y;
    const float TargetHeight=GetStrikeBodySize(*Target).Y;
    const float HeightTolerance=FMath::Abs(OwnerHeight-TargetHeight)+FMath::Min(OwnerHeight,TargetHeight)*.5f;
    const FVector Difference=Target->GetActorLocation()-GetOwner()->GetActorLocation();
    // 身高不同会改变胶囊中心高度，靠近距离按地面上的两人间距计算
    return Difference.SizeSquared2D()<=FMath::Square(Distance) && FMath::Abs(Difference.Z)<=HeightTolerance &&
        CanStrikeTarget(Target,FMath::Sqrt(FMath::Square(Distance)+FMath::Square(HeightTolerance)));
}

/// @brief 能不能走过去打：近身直接算能，远的要导航能完整走到，才列进模型的候选
bool UDeliveryNpcCombatComponent::CanApproachStrike(AActor* Target) const
{
    if (!CanContinueStrikeApproach(Target)) return false;
    if (CanStrike(Target)) return true;
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    return Npc && Npc->CanReachTarget(Target);
}

/// @brief 走过去的途中目标还有效、冷却已过；路能不能走由导航任务自己查
bool UDeliveryNpcCombatComponent::CanContinueStrikeApproach(AActor* Target) const
{
    return HasStrikePose() && IsReadyToStrike() &&
        CanStrikeTarget(Target,FMath::Max(StrikeRange,StrikeApproachRange));
}

/// @brief 开始挥一拳
/// @return 开始挥拳时返回 true；只有拳头在接触时间段里真的扫到目标才算打中
bool UDeliveryNpcCombatComponent::StartStrike(AActor* Target,FGuid Id)
{
    return StartStrikeInternal(Target,Id,Id,false);
}

/// @brief 记下目标、时间和编号，挂上拳头扫掠；bLightHit 表示交战中的轻拳
bool UDeliveryNpcCombatComponent::StartStrikeInternal(AActor* Target,FGuid Id,FGuid ActionId,bool bLightHit)
{
    if (!GetOwner()->HasAuthority() || !Id.IsValid() || !ActionId.IsValid() || Id==FinishedStrikeId ||
        !StrikeMesh.IsValid() || !IsReadyToStrike() || !CanStrike(Target)) return false;
    AttackId=Id; AttackTarget=Target; StrikeAnimationId=Id; StrikeActionId=ActionId;
    StrikeStartedAt=GetWorld()->GetTimeSeconds(); bCurrentStrikeIsLight=bLightHit;
    FinishedStrikeId.Invalidate(); bFinishedStrikeHit=false;
    PreviousHandTime=-1;
    NextStrikeAt=StrikeStartedAt+StrikeWindup+1;
    // 服务器没有渲染视角时也要更新拳头，判定结束后恢复原来的动画优化设置
    if (auto* Mesh=StrikeMesh.Get())
    {
        PreviousMeshVisibility=static_cast<uint8>(Mesh->VisibilityBasedAnimTickOption);
        bPreviousUpdateRateOptimization=Mesh->bEnableUpdateRateOptimizations;
        bOverrideMeshTick=true;
        Mesh->VisibilityBasedAnimTickOption=EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
        Mesh->bEnableUpdateRateOptimizations=false;
    }
    GetOwner()->ForceNetUpdate();
    return true;
}

/// @brief 查这一拳打中没有
/// @return 已判定时返回 true，结果写在 bOutHit；还没判定或已取消时返回 false
bool UDeliveryNpcCombatComponent::GetStrikeResult(FGuid Id,bool& bOutHit) const
{
    bOutHit=false;
    if (!Id.IsValid() || FinishedStrikeId!=Id) return false;
    bOutHit=bFinishedStrikeHit;
    return true;
}

/// @brief 这一拳开始的服务器时间，客户端据此对齐出拳动画；还在走近时返回负数
double UDeliveryNpcCombatComponent::GetStrikeStartTime(FGuid Id) const
{
    return Id.IsValid() && StrikeActionId==Id?StrikeStartedAt:-1;
}

// ── 交战 ──
// Fight 动作：NPC 的 ExecuteAction 调用 BeginFight，之后行为树任务每次挥拳调用 StartFightStrike；ContinueFight 延长时间，EndFight 结束并进入冷却

/// @brief 冷却已过，可以挥下一拳
bool UDeliveryNpcCombatComponent::IsReadyToStrike() const
{
    return GetWorld() && !bDown && !IsReactingToHit() && !AttackId.IsValid() && GetWorld()->GetTimeSeconds()>=NextStrikeAt;
}

// ── 受击 ──
// 被打或被撞：ReceiveMeleeImpact、ReceiveImpact、ReceiveFightHit 记下这一下，受击后仰期间 UpdateHitFacing 让 NPC 转向打它的人

/// @brief 是否正在挨打后仰，0.55 秒内
bool UDeliveryNpcCombatComponent::IsReactingToHit() const
{
    if (!GetWorld() || bDown || HitReactionStartedAt<0) return false;
    const auto* State=GetWorld()->GetGameState();
    const double Now=State?State->GetServerWorldTimeSeconds():GetWorld()->GetTimeSeconds();
    return Now>=HitReactionStartedAt && Now-HitReactionStartedAt<HitReactionDuration;
}

// ── 交战 ──

/// @brief 现在能不能开始和这个人打
bool UDeliveryNpcCombatComponent::CanBeginFight(AActor* Target,float AuthorizedApproachRange) const
{
    return GetWorld() && !FightActionId.IsValid() && GetWorld()->GetTimeSeconds()>=NextFightAt &&
        CanChooseFightTarget(Target,AuthorizedApproachRange);
}

/// @brief 这个人能不能列进模型的交战候选：身体能出拳、在追击范围内、没被挡住
bool UDeliveryNpcCombatComponent::CanChooseFightTarget(AActor* Target,float AuthorizedApproachRange) const
{
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    const float ApproachLimit=FMath::Max3(StrikeRange,StrikeApproachRange,ClampFightApproachOverride(AuthorizedApproachRange));
    if (!GetWorld() || !Npc || !HasStrikePose() || !CanStrikeTarget(Target,ApproachLimit)) return false;
    // 短暂后仰或收拳不会作废模型决定，行为树会等身体恢复后再挥拳
    return CanStrike(Target) || Npc->CanReachTarget(Target);
}

/// @brief 开始一场交战，记下对手、起点和持续时间
/// @return 开始时返回 true；bModelDecision 表示模型选的，规则安排的为 false
bool UDeliveryNpcCombatComponent::BeginFight(AActor* Target,FGuid ActionId,float Duration,bool bModelDecision,float AuthorizedApproachRange)
{
    const float RangeOverride=bModelDecision?0.f:ClampFightApproachOverride(AuthorizedApproachRange);
    if (!GetOwner()->HasAuthority() || !ActionId.IsValid() || !FMath::IsFinite(Duration) || Duration<StrikeDuration ||
        Duration>(bModelDecision?DeliveryNpc::FightDecisionSeconds:DeliveryNpc::OtherActionSeconds) ||
        !CanBeginFight(Target,RangeOverride)) return false;
    FightActionId=ActionId; FightTarget=Target; FightOrigin=GetOwner()->GetActorLocation();
    FightApproachOverride=RangeOverride;
    FightEndsAt=GetWorld()->GetTimeSeconds()+Duration; FightPunches=0; FightHits=0;
    bModelFight=bModelDecision; FightBurstPunches=0;
    // 对方看见有人冲自己来，记下现场变化；是否还手仍由对方自己的模型决定
    if (auto* Other=Cast<ADeliveryNpcCharacter>(Target))
        Other->ObserveFact(TEXT("FightThreat"),GetOwner(),TEXT("这个人朝我冲过来，准备动手"),EDeliveryNpcEvidenceSource::Seen);
    return true;
}

/// @brief 模型决定接着打同一个人：只延长结束时间，当前这拳、起点和命中数都保留
bool UDeliveryNpcCombatComponent::ContinueFight(FGuid ActionId,float Duration)
{
    if (!GetOwner()->HasAuthority() || !bModelFight || !FMath::IsFinite(Duration) || Duration<=0 ||
        Duration>DeliveryNpc::FightDecisionSeconds || !GetFightStopReason(ActionId).IsNone()) return false;
    FightEndsAt=GetWorld()->GetTimeSeconds()+Duration;
    return true;
}

/// @brief 交战该不该停
/// @return 该停时返回原因（倒地、对手没了、时间到、拳数够了等），还能打返回 None；收拳和冷却由行为树等
FName UDeliveryNpcCombatComponent::GetFightStopReason(FGuid ActionId) const
{
    if (!GetWorld() || !ActionId.IsValid() || FightActionId!=ActionId) return TEXT("FightCancelled");
    if (bDown) return TEXT("AttackerDown");
    const auto* Target=FightTarget.Get();
    if (!IsValid(Target)) return TEXT("TargetLost");
    if (const auto* Receiver=Cast<IDeliveryImpactReceiver>(Target); Receiver && Receiver->IsKnockedDown() && !Receiver->CanReceiveHitWhileDown()) return TEXT("TargetDown");
    if (const auto* Player=Cast<ADeliveryCharacter>(Target); Player && Player->GetActiveRagdoll() &&
        Player->GetActiveRagdoll()->GetControlMode()!=EDeliveryRagdollControlMode::Active) return TEXT("TargetDown");
    if (GetWorld()->GetTimeSeconds()>=FightEndsAt) return TEXT("FightTimeLimit");
    const float TravelLimit=FMath::Max(FMath::Clamp(FightTravelRange,160.f,1200.f),FightApproachOverride);
    const float ApproachLimit=FMath::Max3(StrikeRange,StrikeApproachRange,FightApproachOverride);
    if (FVector::DistSquared(GetOwner()->GetActorLocation(),FightOrigin)>FMath::Square(TravelLimit) ||
        FVector::DistSquared(GetOwner()->GetActorLocation(),Target->GetActorLocation())>FMath::Square(ApproachLimit))
        return TEXT("TargetEscaped");
    return NAME_None;
}

/// @brief 交战中挥一拳，PunchId 保证每拳只算一次
bool UDeliveryNpcCombatComponent::StartFightStrike(AActor* Target,FGuid ActionId,FGuid PunchId)
{
    if (!GetOwner()->HasAuthority() || Target!=FightTarget.Get() || !GetFightStopReason(ActionId).IsNone() ||
        (!bModelFight && FightPunches>=GetFightPunchLimit())) return false;
    if (!StartStrikeInternal(Target,PunchId,ActionId,true)) return false;
    ++FightPunches;
    if (bModelFight && ++FightBurstPunches>=GetFightPunchLimit())
    {
        // 连挥几拳后短暂收拳，仍处于交战中，后台判断可以同时继续
        NextStrikeAt+=FMath::Clamp(FightBurstPauseSeconds,0.f,1.5f);
        FightBurstPunches=0;
    }
    return true;
}

/// @brief 结束交战，bApplyCooldown 时要歇一会儿才能再开打
void UDeliveryNpcCombatComponent::EndFight(FGuid ActionId,bool bApplyCooldown)
{
    if (!ActionId.IsValid() || ActionId!=FightActionId) return;
    CancelStrike(); FightActionId.Invalidate(); FightTarget.Reset(); FightEndsAt=0; FightApproachOverride=0.f;
    bModelFight=false;
    if (GetWorld()) NextFightAt=GetWorld()->GetTimeSeconds()+(bApplyCooldown?FMath::Clamp(FightCooldown,1.f,15.f):0.f);
}

/// @brief 用一句话描述这场交战的结果，写进记忆给模型看
FString UDeliveryNpcCombatComponent::DescribeFight(FGuid ActionId,FName Reason) const
{
    if (!ActionId.IsValid() || ActionId!=FightActionId) return FString();
    const auto* TargetNpc=Cast<ADeliveryNpcCharacter>(FightTarget.Get());
    const FString Name=TargetNpc?TargetNpc->DisplayName.ToString():TEXT("对方");
    FString End=TEXT("这次交战已经停止");
    if (Reason==TEXT("TargetDown")) End=TEXT("对方倒地，我停手了");
    else if (Reason==TEXT("TargetEscaped")) End=TEXT("对方跑远，我没有继续追");
    else if (Reason==TEXT("FightTimeLimit")) End=TEXT("追打时间已到，我停手了");
    else if (Reason==TEXT("FightPunchLimit")) End=TEXT("这轮挥拳次数已到，我停手了");
    else if (Reason==TEXT("PathUnavailable") || Reason==TEXT("PathInterrupted")) End=TEXT("前面的路走不通，我停下了");
    else if (Reason==TEXT("Hit")) End=TEXT("我被击倒，交战中断");
    else if (Reason==TEXT("ReconsiderAfterHit")) End=TEXT("我挨了一拳，先停下重新判断要继续打、跑开还是停手");
    return FString::Printf(TEXT("我向%s挥拳 %d 次，命中 %d 次。%s。"),*Name,FightPunches,FightHits,*End);
}

/// @brief 描述自己此刻的状况：刚挨了几拳、是否倒地、打出几拳；模型据此决定继续、逃跑还是停手
FString UDeliveryNpcCombatComponent::DescribeCombatState() const
{
    const double Now=GetWorld()?GetWorld()->GetTimeSeconds():0;
    const int32 RecentHits=Now-LastLightHitAt<=FMath::Clamp(LightHitResetSeconds,2.f,20.f)?ReceivedLightHits:0;
    const TCHAR* Body=bDown?TEXT("已经倒地"):IsReactingToHit()?TEXT("刚挨打，正在后仰"):TEXT("站着，可以行动");
    return FString::Printf(TEXT("这阵子实际挨了 %d 记轻拳；%s交战挥拳 %d 次，实际命中 %d 次。现在%s。"),
        RecentHits,FightActionId.IsValid()?TEXT("本轮"):TEXT("上一轮"),FightPunches,FightHits,Body);
}

// ── 出拳 ──

/// @brief 只在动作编号和拳编号都对上时取消
void UDeliveryNpcCombatComponent::CancelStrike(FGuid ActionId,FGuid PunchId)
{
    if (ActionId==StrikeActionId && PunchId==AttackId && PunchId.IsValid()) CancelStrike();
}

/// @brief 取消当前这拳，还没发生的命中不再判定
void UDeliveryNpcCombatComponent::CancelStrike()
{
    const FGuid CancelledAction=StrikeActionId;
    const FGuid CancelledPunch=AttackId;
    RestoreMeshTickSettings(); PreviousHandTime=-1;
    AttackId.Invalidate(); AttackTarget.Reset(); StrikeAnimationId.Invalidate(); StrikeActionId.Invalidate(); StrikeStartedAt=-1;
    bCurrentStrikeIsLight=false;
    FinishedStrikeId.Invalidate(); bFinishedStrikeHit=false;
    if (GetOwner() && GetOwner()->HasAuthority()) GetOwner()->ForceNetUpdate();
    if (CancelledPunch.IsValid()) OnStrikeFinished.Broadcast(CancelledAction,CancelledPunch,true);
}

/// @brief 挥拳时为了每帧更新骨骼改过网格的更新设置，这里还原
void UDeliveryNpcCombatComponent::RestoreMeshTickSettings()
{
    if (!bOverrideMeshTick) return;
    if (auto* Mesh=StrikeMesh.Get())
    {
        Mesh->VisibilityBasedAnimTickOption=static_cast<EVisibilityBasedAnimTickOption>(PreviousMeshVisibility);
        Mesh->bEnableUpdateRateOptimizations=bPreviousUpdateRateOptimization;
    }
    bOverrideMeshTick=false;
}

/// @brief 判定这一拳：记日志、打中就扣血推人、发 OnStrikeFinished
void UDeliveryNpcCombatComponent::FinishStrike(bool bHit,const FVector& ImpactPoint)
{
    if (!AttackId.IsValid()) return;
    const FGuid CompletedAction=StrikeActionId;
    const FGuid CompletedPunch=AttackId;
    ON_SCOPE_EXIT
    {
        if (IsValid(this) && FinishedStrikeId==CompletedPunch)
            OnStrikeFinished.Broadcast(CompletedAction,CompletedPunch,false);
    };
    auto* Target=AttackTarget.Get();
    const bool bLightHit=bCurrentStrikeIsLight;
    FinishedStrikeId=AttackId; bFinishedStrikeHit=bHit;
    UE_LOG(LogTemp,Log,TEXT("NpcPunch result=%s npc=%s target=%s punch=%s distance=%.1f point=%s"),
        bHit?TEXT("hit"):TEXT("miss"),*GetNameSafe(GetOwner()),*GetNameSafe(Target),*FinishedStrikeId.ToString(),
        IsValid(Target)?FVector::Dist(GetOwner()->GetActorLocation(),Target->GetActorLocation()):-1.0,*ImpactPoint.ToCompactString());
    // 先结束这一拳，再通知受击者；受击可能立即取消双方动作
    AttackId.Invalidate(); AttackTarget.Reset(); PreviousHandTime=-1; RestoreMeshTickSettings();
    if (!bHit || !IsValid(Target)) return;
    if (bLightHit && FightActionId.IsValid()) ++FightHits;
    const FVector Direction=(Target->GetActorLocation()-GetOwner()->GetActorLocation()).GetSafeNormal();
    if (auto* Npc=Cast<ADeliveryNpcCharacter>(Target); (bLightHit || bHarmlessStrikes) && Npc)
        Npc->Combat->ReceiveFightHit(GetOwner(),CompletedPunch,bHarmlessStrikes);
    else if (auto* Receiver=Cast<IDeliveryImpactReceiver>(Target))
        { if (!bHarmlessStrikes) Receiver->ReceiveMeleeHitFrom(Direction*OutgoingImpactImpulse,ImpactPoint,GetOwner(),CompletedPunch); }
    else if (auto* Player=Cast<ADeliveryCharacter>(Target))
    {
        const auto* PlayerAbilities=Cast<IAbilitySystemInterface>(DeliveryCombatEvents::FindPlayerState(Player));
        auto* ASC=PlayerAbilities?PlayerAbilities->GetAbilitySystemComponent():nullptr;
        if (IsValid(ASC) && !bHarmlessStrikes && OutgoingPlayerDamage>0)
        {
            auto Context=ASC->MakeEffectContext(); Context.AddSourceObject(GetOwner());
            auto Spec=ASC->MakeOutgoingSpec(Player->GetDamageEffect(),1,Context);
            if (Spec.IsValid()) { Spec.Data->SetSetByCallerMagnitude(TAG_Effect_Type_Damage,-OutgoingPlayerDamage); ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data); }
        }
        if (IsValid(Player) && !Player->IsActorBeingDestroyed() && IsValid(Player->GetActiveRagdoll()) &&
            !bHarmlessStrikes && OutgoingPlayerImpulse>0)
            Player->GetActiveRagdoll()->ApplyMeleeImpact(Direction*OutgoingPlayerImpulse,ImpactPoint);
        // 只在真实接触后播放，挥空和二狗的无伤轻拍保留各自原有表现
        // GAS 的同步通知可能销毁玩家，所以结算后再次检查
        if (IsValid(Player) && !Player->IsActorBeingDestroyed() && !bHarmlessStrikes)
        {
            if (auto* Combat=Player->GetRagdollCombat(); IsValid(Combat))
                Combat->MulticastPlayPunchHit(ImpactPoint,false);
            Player->NotifyMeleeHit();
        }
    }
    DeliveryCombatEvents::ReportHit(GetOwner(),Target,CompletedPunch,ImpactPoint);
}

/// @brief 每次骨骼更新后：在接触时间段里沿拳头这一帧走过的路径扫球，查遮挡，碰到目标就 FinishStrike
void UDeliveryNpcCombatComponent::OnStrikePoseUpdated()
{
    if (!IsInGameThread() || !GetOwner()->HasAuthority() || !AttackId.IsValid() || !GetWorld()) return;
    auto* Mesh=StrikeMesh.Get();
    auto* Target=AttackTarget.Get();
    if (!Mesh || !HasStrikePose() || !IsValid(Target) || bDown || IsReactingToHit())
    { FinishStrike(false,FVector::ZeroVector); return; }
    const double Now=GetWorld()->GetTimeSeconds();
    if (Now<=PreviousHandTime) return;
    const double Elapsed=Now-StrikeStartedAt;
    const double WindowStart=FMath::Clamp(StrikeContactStart,0.f,.5f);
    const double WindowEnd=FMath::Clamp(StrikeContactEnd,static_cast<float>(WindowStart+.03),.8f);
    const FVector Hand=Mesh->GetSocketTransform(StrikeHandBone,RTS_World).TransformPosition(StrikeHandOffset);
    const double PreviousElapsed=PreviousHandTime-StrikeStartedAt;
    const double Step=Now-PreviousHandTime;
    const bool bContinuous=PreviousHandTime>=StrikeStartedAt && Step<=.15 &&
        FVector::DistSquared(GetOwner()->GetActorLocation(),PreviousOwnerPosition)<=FMath::Square(50.f);
    const FVector Previous=PreviousHandPosition;
    PreviousHandPosition=Hand; PreviousHandTime=Now; PreviousOwnerPosition=GetOwner()->GetActorLocation();
    if (bContinuous && Elapsed>=WindowStart && PreviousElapsed<=WindowEnd)
    {
        // 只检测有效窗口内的这一小段拳路；瞬移或长时间没更新骨骼时重新开始采样
        const float StartAlpha=static_cast<float>(FMath::Clamp((WindowStart-PreviousElapsed)/Step,0.0,1.0));
        const float EndAlpha=static_cast<float>(FMath::Clamp((WindowEnd-PreviousElapsed)/Step,0.0,1.0));
        const FVector Start=FMath::Lerp(Previous,Hand,StartAlpha);
        const FVector End=FMath::Lerp(Previous,Hand,EndAlpha);
        FCollisionObjectQueryParams Objects;
        Objects.AddObjectTypesToQuery(ECC_Pawn); Objects.AddObjectTypesToQuery(ECC_PhysicsBody);
        Objects.AddObjectTypesToQuery(DeliveryCollision::Pedestrian);
        Objects.AddObjectTypesToQuery(ECC_WorldStatic); Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
        Objects.AddObjectTypesToQuery(ECC_Vehicle);
        FCollisionQueryParams Params(SCENE_QUERY_STAT(NpcFistContact),false,GetOwner());
        TArray<FHitResult> Hits;
        GetWorld()->SweepMultiByObjectType(Hits,Start,End,FQuat::Identity,Objects,
            FCollisionShape::MakeSphere(FMath::Clamp(StrikeHandRadius,2.f,12.f)),Params);
        Hits.Sort([](const FHitResult& A,const FHitResult& B){ return A.Time<B.Time; });
        for (const auto& Hit:Hits)
        {
            const auto* Component=Hit.GetComponent();
            if (!Component) continue;
            if (Hit.GetActor()==Target)
            {
                // NPC 现有身体胶囊负责接触；玩家使用当前布娃娃的身体，忽略交互和感知区域
                if (const auto* Npc=Cast<ADeliveryNpcCharacter>(Target); Npc && Component!=Npc->GetCapsuleComponent()) continue;
                if (const auto* Player=Cast<ADeliveryCharacter>(Target); Player && Component!=Player->GetMesh()) continue;
                FHitResult Obstacle;
                const bool bBlocked=GetWorld()->LineTraceSingleByChannel(Obstacle,Start,Hit.ImpactPoint,ECC_Visibility,Params) &&
                    Obstacle.GetActor()!=Target;
                // 保留身体之间的遮挡检查，墙边的拳头不能从墙另一侧击中目标
                if (!bBlocked && CanStrikeTarget(Target,StrikeApproachRange)) FinishStrike(true,Hit.ImpactPoint);
                break;
            }
            // 对象查询会把触发区域也列为命中，只让真正阻挡视线或人物身体的组件挡拳
            if (Component->GetCollisionResponseToChannel(ECC_Visibility)==ECR_Block ||
                Component->GetCollisionResponseToChannel(ECC_Pawn)==ECR_Block) break;
        }
    }
    if (AttackId.IsValid() && Elapsed>=WindowEnd) FinishStrike(false,FVector::ZeroVector);
}

// ── 受击 ──

/// @brief 记下这一拳，已经记过的返回 false
bool UDeliveryNpcCombatComponent::RememberPunch(FGuid PunchId)
{
    if (!PunchId.IsValid() || RecentLightHits.Contains(PunchId)) return false;
    RecentLightHits.Add(PunchId);
    if (RecentLightHits.Num()>32) RecentLightHits.RemoveAt(0);
    return true;
}

/// @brief 撤掉挨打时转头看向打人者的朝向；只撤这一层，模型选的朝向不动
void UDeliveryNpcCombatComponent::ResetHitFacing()
{
    auto* AI=HitFacingController.Get();
    // 一般在 AI 控制器 UnPossess 换角色之前调用；只在控制器还控制着这个 NPC、或者已经不控制任何角色时清它的关注点，不动别的角色
    // 关注的还是打人者时才清，模型后来设的关注对象不动
    if (bHitFacingApplied && AI && (!AI->GetPawn() || AI->GetPawn()==GetOwner()) &&
        AI->GetFocusActorForPriority(HitReactionFocus)==HitFacingSource.Get() &&
        (HitFacingSource.IsValid() || !FAISystem::IsValidLocation(AI->GetFocalPointForPriority(HitReactionFocus))))
        AI->ClearFocus(HitReactionFocus);
    HitFacingController.Reset(); HitFacingSource.Reset(); HitFacingListener.Reset();
    HitFacingId.Invalidate(); HitFacingActionId.Invalidate(); HitFacingEndsAt=0; bHitFacingApplied=false;
    RecoveryFacingSource.Reset(); RecoveryFacingEpisode.Invalidate();
}

/// @brief 挨打时让 AI 控制器转头看向打人者
void UDeliveryNpcCombatComponent::RequestHitFacing(AActor* Source,FGuid HitId)
{
    ResetHitFacing();
    auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    auto* AI=IsValid(Npc)?Cast<AAIController>(Npc->GetController()):nullptr;
    if (bEndingHitFacing || !HitId.IsValid() || !GetWorld() || !IsValid(Npc) || !Npc->HasAuthority() || Npc->IsHidden() || !AI ||
        bDown || bRecovering ||
        !IsValid(Source) || Source==Npc || Source->IsHidden() || Source->GetWorld()!=GetWorld()) return;
    HitFacingController=AI; HitFacingSource=Source; HitFacingId=HitId;
    HitFacingListener=Npc->Agent->GetListeningSpeaker();
    HitFacingActionId=Npc->Agent->GetPublicState().ActionId;
    HitFacingEndsAt=GetWorld()->GetTimeSeconds()+HitFacingMaximumSeconds;
    // 先登记本次命中，取消拽车可能稍后才从 MOVE_None 恢复 Walking
    // Tick 只读取当前编号；旧取消回调末尾不重新启动朝向，也不在空中设置焦点
}

/// @brief 到时间就撤掉转头
void UDeliveryNpcCombatComponent::UpdateHitFacing(double Now)
{
    if (RecoveryFacingEpisode.IsValid() && (!bDown || bDeliberate || RecoveryFacingEpisode!=DownEpisodeId ||
        !RecoveryFacingSource.IsValid() || RecoveryFacingSource->IsHidden() || RecoveryFacingSource->GetWorld()!=GetWorld()))
    { RecoveryFacingSource.Reset(); RecoveryFacingEpisode.Invalidate(); }
    if (!HitFacingId.IsValid()) return;
    const FGuid FacingId=HitFacingId;
    auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    auto* AI=HitFacingController.Get();
    auto* Source=HitFacingSource.Get();
    if (bEndingHitFacing || !IsValid(Npc) || !Npc->HasAuthority() || Npc->IsHidden() || bDown || bRecovering ||
        !AI || AI!=Npc->GetController() || AI->GetPawn()!=Npc || !IsValid(Source) || Source->IsHidden() ||
        Source->GetWorld()!=GetWorld() || Now>=HitFacingEndsAt ||
        (bHitFacingApplied && AI->GetFocusActorForPriority(HitReactionFocus)!=Source))
    { ResetHitFacing(); return; }
    const auto* Agent=Npc->Agent.Get();
    const TWeakObjectPtr<AActor> Listener=Agent->GetListeningSpeaker();
    const FGuid CurrentActionId=Agent->GetPublicState().ActionId;
    // Follow/Stay 可在两个 Tick 之间完成，新的有效编号仍代表新的安排
    const bool bNewAction=CurrentActionId.IsValid() && CurrentActionId!=HitFacingActionId;
    const bool bNewListener=Agent->ShouldPauseForConversation() && !HitFacingListener.HasSameIndexAndSerialNumber(Listener);
    // 新指令或交谈接管；旧 Fight/Flee 只在原后仰期间暂停，之后立即让位
    if (bNewAction || bNewListener || (!IsReactingToHit() &&
        (Agent->HasActiveAction() || Agent->HasCompanionActivity() || Agent->ShouldPauseForConversation())))
    { ResetHitFacing(); return; }
    if (!Npc->GetCharacterMovement()->IsMovingOnGround())
    {
        // 待启动的请求最多保留到原截止时间；已转身后离地立即释放自己的焦点
        if (bHitFacingApplied) ResetHitFacing();
        return;
    }
    const FVector Toward=(Source->GetActorLocation()-Npc->GetActorLocation()).GetSafeNormal2D();
    if (!IsReactingToHit() && (Toward.IsNearlyZero() ||
        FMath::Abs(FMath::FindDeltaAngleDegrees(Npc->GetActorRotation().Yaw,Toward.Rotation().Yaw))<=HitFacingArrivalDegrees))
    { ResetHitFacing(); return; }
    if (!bHitFacingApplied && HitFacingId==FacingId)
    {
        // 只交给既有 CMC RotationRate 转身，不写 ActorRotation，不改变全局转速
        AI->SetFocus(Source,HitReactionFocus);
        bHitFacingApplied=true;
    }
}

/// @brief 倒地时记下打人者，起身后转向他
void UDeliveryNpcCombatComponent::RememberRecoveryFacing(AActor* Source)
{
    ResetHitFacing();
    if (!bEndingHitFacing && GetOwner()->HasAuthority() && bDown && !bDeliberate && DownEpisodeId.IsValid() &&
        IsValid(Source) && Source!=GetOwner() && !Source->IsHidden() && Source->GetWorld()==GetWorld())
    { RecoveryFacingSource=Source; RecoveryFacingEpisode=DownEpisodeId; }
}

/// @brief 起身后转向打人者，倒地编号要对上
void UDeliveryNpcCombatComponent::TryFaceSourceAfterRecovery(FGuid Episode)
{
    if (!Episode.IsValid() || RecoveryFacingEpisode!=Episode) return;
    const TWeakObjectPtr<AActor> Source=RecoveryFacingSource;
    RecoveryFacingSource.Reset(); RecoveryFacingEpisode.Invalidate();
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    // 起身完成事件可能已经安排新动作，不抢新对话、战斗或逃跑的方向
    if (!IsValid(Npc) || bDown || bRecovering || Npc->Agent->HasActiveAction() || Npc->Agent->HasCompanionActivity() ||
        Npc->Agent->ShouldPauseForConversation()) return;
    RequestHitFacing(Source.Get(),Episode);
}

// ── 起身 ──
// 倒地后 TickComponent 计时，到时调用 BeginRecovery 起身；玩家扶起由 StartAssistedRecovery 开始，故意躺下由 SetDeliberateProne 开始

/// @brief 倒地时又挨打，延长躺着的时间
void UDeliveryNpcCombatComponent::ExtendDownTime(AActor* Source)
{
    if (!bDown || !bReceiveHitsWhileDown || !GetOwner()->HasAuthority()) return;
    CancelAssistedRecovery(DownEpisodeId);
    const double Now=GetWorld()->GetTimeSeconds();
    RecoverAt=FMath::Max(RecoverAt,Now)+FMath::Clamp(DownHitExtensionSeconds,0.f,10.f);
    bRecovering=false; RecoveryStartedAt=-1;
    RememberRecoveryFacing(Source);
    GetOwner()->ForceNetUpdate();
    OnDownChanged.Broadcast(DownEpisodeId);
    if (!IsValid(this) || !IsValid(GetOwner())) return;
    if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner()))
        Npc->ObserveFact(TEXT("HitWhileDown"),Source,TEXT("我倒地后又被这一拳打中，需要再躺一会儿才能起来"),EDeliveryNpcEvidenceSource::Seen);
}

// ── 受击 ──

/// @brief 被拳头打中，PunchId 相同的只算一次
void UDeliveryNpcCombatComponent::ReceiveMeleeImpact(const FVector& Impulse,const FVector& Point,AActor* Source,FGuid PunchId)
{
    auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!IsValid(Npc) || !Npc->HasAuthority() || Npc->IsHidden() || Impulse.ContainsNaN() || Point.ContainsNaN() || !IsValid(Source) ||
        Source->GetWorld()!=GetWorld() || !RememberPunch(PunchId)) return;
    if (bDown)
    {
        if (bReceiveHitsWhileDown)
        {
            ExtendDownTime(Source);
            Npc->Presentation->PlayReaction(EDeliveryNpcVoiceReaction::Hit,PunchId);
        }
        return;
    }
    // 首次击倒沿角色的虚接口，保留初遇中断和已有受击声音
    Npc->ReceiveImpactFrom(Impulse,Point,Source);
}

/// @brief 服务器：被车撞或被推时立刻倒地，不等模型
/// 停下正在挥的拳和当前安排、松开拽着的车，用 LaunchCharacter 把 NPC 推出去，到 RecoverySeconds 后自己起来
void UDeliveryNpcCombatComponent::ReceiveImpact(const FVector& Impulse,AActor* Source)
{
    auto* Resident=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Resident || !Resident->HasAuthority() || bDown || Impulse.ContainsNaN()) return;
    bDown=true; bDeliberate=false; bRecovering=false;
    DownEpisodeId=FGuid::NewGuid();
    RememberRecoveryFacing(Source);
    CompletedRecoveryEpisode.Invalidate(); CompletedRecoveryHelper.Reset();
    KnockdownAttacker=Source;
    CancelStrike(); Resident->Agent->CancelPlan(TEXT("Hit"));
    if (auto* Hold=Resident->GetHeldVehicle()) Hold->Release(TEXT("HolderHit"));
    HitReactionStartedAt=-1; ReceivedLightHits=0;
    DownStartedAt=GetWorld()->GetTimeSeconds(); RecoveryStartedAt=-1; RecoverAt=DownStartedAt+RecoverySeconds;
    // 使用 CharacterMovement 处理 NPC 击退和地面碰撞，并限制击退速度
    Resident->LaunchCharacter(Impulse.GetClampedToMaxSize(KnockdownHorizontalSpeed)+FVector(0,0,KnockdownUpwardSpeed),true,true);
    OnRep_Down(); Resident->ForceNetUpdate();
    OnDownChanged.Broadcast(DownEpisodeId);
    if (!IsValid(this) || !IsValid(Resident)) return;
    Resident->ObserveFact(TEXT("Hit"),Source,TEXT("我被这个对象击倒了"),EDeliveryNpcEvidenceSource::Seen);
}

/// @brief 交战中挨了一拳：够数就倒，否则后仰
void UDeliveryNpcCombatComponent::ReceiveFightHit(AActor* Source,FGuid PunchId,bool bHarmless)
{
    auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc || !Npc->HasAuthority() || !IsValid(Source) || Source->GetWorld()!=GetWorld() || !RememberPunch(PunchId)) return;
    if (bDown) { if (bReceiveHitsWhileDown && !bHarmless) ExtendDownTime(Source); return; }
    if (bKnockDownOnAnyPunch && !bHarmless)
    {
        Npc->ReceiveImpactFrom((Npc->GetActorLocation()-Source->GetActorLocation()).GetSafeNormal()*35000.f,Npc->GetActorLocation(),Source);
        return;
    }
    const double Now=GetWorld()->GetTimeSeconds();
    if (Now-LastLightHitAt>FMath::Clamp(LightHitResetSeconds,2.f,20.f)) ReceivedLightHits=0;
    LastLightHitAt=Now; if (!bHarmless) ++ReceivedLightHits;
    if (!bHarmless && ReceivedLightHits>=FMath::Clamp(LightHitsToKnockDown,2,8))
    {
        Npc->ReceiveImpactFrom((Npc->GetActorLocation()-Source->GetActorLocation()).GetSafeNormal()*35000.f,Npc->GetActorLocation(),Source);
        return;
    }
    HitReactionStartedAt=Now;
    // 在同步取消回调前登记本次来源；较新的命中可以立即替换它，不在旧回调末尾重设
    RequestHitFacing(Source,PunchId);
    // 轻拳打断尚未命中的这一拳，后仰结束后仍按模型原来的选择追打或逃跑
    if (AttackId.IsValid())
    {
        const FGuid InterruptedPunch=AttackId;
        const FGuid InterruptedAction=StrikeActionId;
        FinishedStrikeId=AttackId; bFinishedStrikeHit=false;
        RestoreMeshTickSettings(); PreviousHandTime=-1;
        AttackId.Invalidate(); AttackTarget.Reset(); StrikeAnimationId.Invalidate(); StrikeActionId.Invalidate();
        StrikeStartedAt=-1; bCurrentStrikeIsLight=false;
        OnStrikeFinished.Broadcast(InterruptedAction,InterruptedPunch,true);
    }
    if (Npc->Agent->HasActiveAction())
    {
        const auto Action=Npc->Agent->GetPublicState();
        const bool bContinueModelCombat=Action.Origin==EDeliveryNpcActionOrigin::Model &&
            (Action.Action==EDeliveryNpcAction::Fight || Action.Action==EDeliveryNpcAction::Flee);
        if (!bContinueModelCombat)
        {
            const bool bWasFighting=FightActionId.IsValid();
            // 闲聊和其他动作仍会被受击打断，护主动作保留原来的规则检查
            Npc->Agent->TryFinishAction(Action.ActionId,EDeliveryNpcActionStatus::Cancelled,TEXT("ReconsiderAfterHit"));
            if (bWasFighting) NextFightAt=Now;
            Npc->Agent->CancelDecision();
        }
    }
    else if (Npc->Presentation->HasPendingSpeech() || !Npc->Agent->IsRespondingToAttackFrom(Source))
    {
        // 停止旧闲聊及其声音任务，已经结合本次攻击的请求可以继续，避免连续轻拳让回复永远算不完
        Npc->Agent->CancelDecision();
    }
    Npc->Presentation->CancelSpeech();
    if (auto* AI=Cast<AAIController>(Npc->GetController())) AI->StopMovement();
    Npc->GetCharacterMovement()->StopMovementImmediately();
    HitReactionStartedAt=Now;
    Npc->ForceNetUpdate();
    // 使用这次命中的编号播放人物短呼声，声音直接来自游戏资源
    Npc->Presentation->PlayReaction(EDeliveryNpcVoiceReaction::Hit,PunchId);
    Npc->ObserveFact(TEXT("LightMeleeHit"),Source,TEXT("这个人打了我一拳，我后仰了一下，仍然站着"),EDeliveryNpcEvidenceSource::Seen);
    // 站着挨拳时立即还手，行为树等后仰结束再挥拳
    if (bRetaliateWhenHit && IsValid(this) && IsValid(Npc) && !bDown && IsValid(Source))
        Npc->Agent->RequestRetaliation(Source);
}

// ── 起身 ──

/// @brief 现在能不能扶：倒着、不在起身、不是主动躺下
bool UDeliveryNpcCombatComponent::CanStartAssistedRecovery() const
{
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    return IsValid(Npc) && bDown && !bDeliberate && !bRecovering && !bAssistedRecovery &&
        DownEpisodeId.IsValid() && Npc->GetCharacterMovement()->IsMovingOnGround();
}

/// @brief 有人开始扶，倒地编号要对上
/// @return 开始起身时返回 true，真正站起来要等 OnRecoveryFinished
bool UDeliveryNpcCombatComponent::StartAssistedRecovery(FGuid ExpectedDownEpisode,APawn* Helper)
{
    if (!GetOwner()->HasAuthority() || !GetWorld() || ExpectedDownEpisode!=DownEpisodeId ||
        !CanStartAssistedRecovery() || !IsValid(Helper) || Helper->GetWorld()!=GetWorld() ||
        !CanContinueAssistedRecovery.IsBound() || !CanContinueAssistedRecovery.Execute(Helper,ExpectedDownEpisode)) return false;
    // 保存自然恢复的截止时间，取消扶起不会凭空延长原来的倒地
    UnassistedRecoverAt=RecoverAt;
    RecoveryHelper=Helper;
    bAssistedRecovery=true;
    BeginRecovery(GetWorld()->GetTimeSeconds());
    return true;
}

/// @brief 不扶了，回到原来的自然起身时间
void UDeliveryNpcCombatComponent::CancelAssistedRecovery(FGuid ExpectedDownEpisode)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || !bAssistedRecovery || ExpectedDownEpisode!=DownEpisodeId) return;
    bAssistedRecovery=false;
    RecoveryHelper.Reset();
    bRecovering=false;
    RecoveryStartedAt=-1;
    RecoverAt=FMath::Max(UnassistedRecoverAt,GetWorld()?static_cast<double>(GetWorld()->GetTimeSeconds()):0.0);
    UnassistedRecoverAt=0;
    GetOwner()->ForceNetUpdate();
}

/// @brief 这次起身是不是这个人扶起来的
bool UDeliveryNpcCombatComponent::WasRecoveryAssistedBy(FGuid Episode,APawn* Helper) const
{
    return Episode.IsValid() && Episode==CompletedRecoveryEpisode && IsValid(Helper) &&
        CompletedRecoveryHelper.Get()==Helper && !bDown;
}

/// @brief 主动躺下或取消躺下，不是被打倒
bool UDeliveryNpcCombatComponent::SetDeliberateProne(bool bProne)
{
    if (!GetOwner()->HasAuthority() || (bDown && !bDeliberate)) return false;
    ResetHitFacing();
    if (bProne && !bDown) DownStartedAt=GetWorld()->GetTimeSeconds();
    if (!bProne) { DownStartedAt=-1; RecoveryStartedAt=-1; bRecovering=false; }
    bDown=bProne; bDeliberate=bProne; RecoverAt=GetWorld()->GetTimeSeconds()+4;
    DownEpisodeId.Invalidate();
    OnRep_Down(); GetOwner()->ForceNetUpdate(); return true;
}

/// @brief 主动躺下时能不能起来
bool UDeliveryNpcCombatComponent::CanStartDeliberateRecovery() const
{
    const auto* Resident=Cast<ADeliveryNpcCharacter>(GetOwner());
    return IsValid(Resident) && IsDeliberatelyProne() && Resident->GetCharacterMovement()->IsMovingOnGround();
}

/// @brief 从主动躺下开始起身，已经在起身时接着起
bool UDeliveryNpcCombatComponent::StartDeliberateRecovery()
{
    if (!CanStartDeliberateRecovery() || !GetOwner()->HasAuthority() || !GetWorld()) return false;
    if (!bRecovering) BeginRecovery(GetWorld()->GetTimeSeconds());
    return true;
}

// ── 受击 ──

/// @brief 开始起身动作
void UDeliveryNpcCombatComponent::BeginRecovery(double Now)
{
    bRecovering=true;
    RecoveryStartedAt=Now;
    RecoverAt=Now+RecoveryMotionSeconds;
    GetOwner()->ForceNetUpdate();
}

/// @brief bDown 变化时（服务器手动调用，客户端收到同步时调用）：倒地时行走速度设为 0，起来后恢复 WalkSpeed
void UDeliveryNpcCombatComponent::OnRep_Down()
{
    if (auto* Resident=Cast<ADeliveryNpcCharacter>(GetOwner())) Resident->GetCharacterMovement()->MaxWalkSpeed=bDown?0.f:Resident->WalkSpeed;
}

// ── 生命周期 ──

// ── 生命周期 ──

/// @brief 服务器每帧：调用 UpdateHitFacing；出拳超过接触时间还没结束就按没打中调用 FinishStrike；玩家扶起时复核条件
/// 倒地到时间时先调用 BeginRecovery 播起身动作，起身动作结束后清掉倒地状态、广播 OnRecoveryFinished，写一条 Recovered 见闻
/// bRetaliateWhenHit 为 true 时再调用 Agent->RequestRetaliation 去打把自己打倒的人
void UDeliveryNpcCombatComponent::TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Fn)
{
    Super::TickComponent(Dt,Type,Fn); if (!GetOwner()->HasAuthority()) return;
    const double Now=GetWorld()->GetTimeSeconds();
    UpdateHitFacing(Now);
    // 没有新的骨骼姿势时按未命中结束，不能退回距离判定
    if (AttackId.IsValid() && Now>=StrikeStartedAt+FMath::Clamp(StrikeContactEnd,.15f,.8f)+.15)
        FinishStrike(false,FVector::ZeroVector);
    if (bAssistedRecovery)
    {
        const auto* Resident=Cast<ADeliveryNpcCharacter>(GetOwner());

        // 扶起过程中每帧调用 CanContinueAssistedRecovery 复核：玩家走开、换了角色、被挡住或自己又挨打时调用 CancelAssistedRecovery
        if (!IsValid(Resident) || !Resident->GetCharacterMovement()->IsMovingOnGround() ||
            !RecoveryHelper.IsValid() || !CanContinueAssistedRecovery.IsBound() ||
            !CanContinueAssistedRecovery.Execute(RecoveryHelper.Get(),DownEpisodeId))
            CancelAssistedRecovery(DownEpisodeId);
    }
    if (bDown && Now>=RecoverAt)
    {
        if (!bRecovering)
        {
            auto* Resident=CastChecked<ADeliveryNpcCharacter>(GetOwner());
            if (!Resident->GetCharacterMovement()->IsMovingOnGround()) return;
            BeginRecovery(Now);
            return;
        }
        const FGuid FinishedEpisode=DownEpisodeId;
        const TWeakObjectPtr<APawn> FinishedHelper=bAssistedRecovery?RecoveryHelper:TWeakObjectPtr<APawn>();
        bDown=false; bDeliberate=false; bRecovering=false; DownStartedAt=-1; RecoveryStartedAt=-1;
        bAssistedRecovery=false; RecoveryHelper.Reset(); UnassistedRecoverAt=0;
        CompletedRecoveryEpisode=FinishedEpisode; CompletedRecoveryHelper=FinishedHelper;
        DownEpisodeId.Invalidate();
        OnRep_Down(); GetOwner()->ForceNetUpdate();
        OnRecoveryFinished.Broadcast(FinishedEpisode,FinishedHelper.Get());
        if (!IsValid(this) || !IsValid(GetOwner())) return;
        TryFaceSourceAfterRecovery(FinishedEpisode);
        const TWeakObjectPtr<AActor> Attacker=KnockdownAttacker;
        KnockdownAttacker.Reset();
        if (auto* Resident=Cast<ADeliveryNpcCharacter>(GetOwner()))
        {
            Resident->ObserveFact(TEXT("Recovered"),nullptr,TEXT("我已经恢复站立，可以继续行动"),EDeliveryNpcEvidenceSource::ActionResult);
            // 起身后直接找把自己打倒的人，对方跑远或被挡住时仍交给模型判断
            if (bRetaliateWhenHit && IsValid(this) && IsValid(Resident) && !bDown && Attacker.IsValid())
                Resident->Agent->RequestRetaliation(Attacker.Get());
        }
    }
}

/// @brief 组件结束：停止受击转身、取消扶起、解绑委托、结束交战、取消出拳，取消网格姿势回调的订阅
void UDeliveryNpcCombatComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    bEndingHitFacing=true;
    ResetHitFacing();
    CancelAssistedRecovery(DownEpisodeId);
    CanContinueAssistedRecovery.Unbind(); OnDownChanged.Clear(); OnRecoveryFinished.Clear();
    DownEpisodeId.Invalidate(); CompletedRecoveryEpisode.Invalidate(); CompletedRecoveryHelper.Reset();
    EndFight(FightActionId); CancelStrike(); DownStartedAt=-1; RecoveryStartedAt=-1; HitReactionStartedAt=-1;
    RecentLightHits.Reset(); ReceivedLightHits=0;
    if (auto* Mesh=StrikeMesh.Get(); Mesh && StrikePoseHandle.IsValid())
        Mesh->UnregisterOnBoneTransformsFinalizedDelegate(StrikePoseHandle);
    StrikePoseHandle.Reset(); StrikeMesh.Reset();
    Super::EndPlay(Reason);
}

/// @brief 登记同步给客户端的倒地、起身、出拳动画和受击时间等属性
void UDeliveryNpcCombatComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps); DOREPLIFETIME(UDeliveryNpcCombatComponent,bDown); DOREPLIFETIME(UDeliveryNpcCombatComponent,bDeliberate); DOREPLIFETIME(UDeliveryNpcCombatComponent,bRecovering);
    DOREPLIFETIME(UDeliveryNpcCombatComponent,StrikeAnimationId); DOREPLIFETIME(UDeliveryNpcCombatComponent,StrikeStartedAt);
    DOREPLIFETIME(UDeliveryNpcCombatComponent,DownStartedAt); DOREPLIFETIME(UDeliveryNpcCombatComponent,RecoveryStartedAt);
    DOREPLIFETIME(UDeliveryNpcCombatComponent,DownEpisodeId);
    DOREPLIFETIME(UDeliveryNpcCombatComponent,StrikeActionId); DOREPLIFETIME(UDeliveryNpcCombatComponent,HitReactionStartedAt);
}
