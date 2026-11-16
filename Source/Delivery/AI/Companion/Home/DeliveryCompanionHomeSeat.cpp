#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "Animation/AnimSequence.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/RootMotionSource.h"
#include "Engine/World.h"

namespace
{
    bool HasOtherSeatMotion(const UCharacterMovementComponent* Movement,FName OwnName)
    {
        const auto Active=[OwnName](const TSharedPtr<FRootMotionSource>& Source)
        {
            return Source.IsValid() && (OwnName.IsNone() || Source->InstanceName!=OwnName) &&
                !Source->Status.HasFlag(ERootMotionSourceStatusFlags::Finished) &&
                !Source->Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval);
        };
        return Movement->CurrentRootMotion.RootMotionSources.ContainsByPredicate(Active) ||
            Movement->CurrentRootMotion.PendingAddRootMotionSources.ContainsByPredicate(Active);
    }

    FVector SeatRootGoal(const ADeliveryCompanionCharacter* Npc,const FTransform& FeetGoal)
    {
        // 保留 CMC 的站立胶囊基准，地面目标留出原步行的最小离地间隙
        return FeetGoal.GetLocation()+FVector(0,0,Npc->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()+2.f);
    }
}

// ── 座位 ──
// 坐下和起身用的座位位置

/// @brief 客户端收到家居状态时更新座位表现
void UDeliveryCompanionComponent::OnRep_HomeState()
{
    UpdateHomeSeatPresentation();
}

/// @brief 去掉坐下或起身的根运动并恢复移动设置；bClearState 为 true 时服务器同时清空复制的座位状态
void UDeliveryCompanionComponent::ReleaseHomeSeatMove(bool bClearState)
{
    auto* Npc=GetCompanion();
    auto* Movement=Npc?Npc->GetCharacterMovement():nullptr;
    const auto Old=HomeSeatRuntime;
    HomeSeatRuntime={};
    if (bClearState && Npc && Npc->HasAuthority()) HomeState.SeatMotion={};
    if (Movement && Old.MotionId)
    {
        const auto Source=Movement->GetRootMotionSourceByID(Old.MotionId);
        if (Source.IsValid() && Source->InstanceName==Old.MotionName)
            Movement->RemoveRootMotionSourceByID(Old.MotionId);
    }
    if (Old.bAddedMoveIgnore && Old.Capsule.IsValid() && Old.Seat.IsValid())
        Old.Capsule->IgnoreActorWhenMoving(Old.Seat.Get(),false);
    if (!IsValid(Npc) || !Movement || !Old.bSavedMovement) return;
    // 旧座位只交还自己仍持有的移动模式，受击或新动作已经接管时保持它们的状态
    if (Movement->MovementMode==MOVE_Flying &&
        !HasOtherSeatMotion(Movement,Old.MotionName) && !HomeSeatRuntime.ArrangementId.IsValid())
    {
        Movement->bOrientRotationToMovement=Old.bSavedOrientToMovement;
        Movement->bUseControllerDesiredRotation=Old.bSavedControllerRotation;
        Movement->StopMovementImmediately();
        // NPC 倒地不会自行关闭 Flying，退出空中座位仍须恢复重力
        const EMovementMode Previous=static_cast<EMovementMode>(Old.PreviousMovementMode);
        Movement->SetMovementMode(Npc->IsKnockedDown()?MOVE_Falling:Previous,Old.PreviousCustomMode);
    }
}

/// @brief 两端按复制的座位状态加上或去掉坐下、起身的根运动
void UDeliveryCompanionComponent::UpdateHomeSeatPresentation()
{
    auto* Npc=GetCompanion();
    auto* Movement=Npc?Npc->GetCharacterMovement():nullptr;
    if (!IsValid(Npc) || !Movement || !GetWorld()) return;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakNpc(Npc);
    const TWeakObjectPtr<UCharacterMovementComponent> WeakMovement(Movement);
    const auto State=HomeState.SeatMotion;
    const bool bActive=!bHomeEndingPlay && State.bActive && State.ArrangementId.IsValid() &&
        HomeState.ArrangementId==State.ArrangementId && IsValid(State.Seat) &&
        !State.Seat->IsActorBeingDestroyed() && State.Seat->GetWorld()==GetWorld() && !Npc->IsKnockedDown();
    if (!Npc->HasAuthority()) SetComponentTickEnabled(bActive);
    if (!bActive)
    {
        if (HomeSeatRuntime.ArrangementId.IsValid()) ReleaseHomeSeatMove(false);
        return;
    }
    const auto* Clock=GetWorld()->GetGameState();
    if (!Clock && !Npc->HasAuthority()) return;
    const double Now=Clock?Clock->GetServerWorldTimeSeconds():GetWorld()->GetTimeSeconds();
    if (!FMath::IsFinite(Now) || !FMath::IsFinite(State.StartedServerTime) ||
        !FMath::IsFinite(State.Duration) || State.Duration<=0 || State.Start.ContainsNaN() || State.Target.ContainsNaN()) return;
    if (HomeSeatRuntime.ArrangementId!=State.ArrangementId || HomeSeatRuntime.Seat.Get()!=State.Seat.Get())
    {
        ReleaseHomeSeatMove(false);
        // 该回调可能经移动模式事件换了安排，旧复制快照不能继续接管
        if (!WeakThis.IsValid() || !WeakNpc.IsValid() || !WeakMovement.IsValid() || Npc->GetCharacterMovement()!=Movement ||
            HomeState.SeatMotion.ArrangementId!=State.ArrangementId || !HomeState.SeatMotion.bActive) return;
        HomeSeatRuntime.ArrangementId=State.ArrangementId;
        HomeSeatRuntime.Seat=State.Seat;
        HomeSeatRuntime.Capsule=Npc->GetCapsuleComponent();
        HomeSeatRuntime.PreviousMovementMode=Movement->MovementMode;
        HomeSeatRuntime.PreviousCustomMode=Movement->CustomMovementMode;
        // 模拟代理可能先收到权威 Flying，退出仍交回普通 NPC 地面移动
        if (!Npc->HasAuthority() && Movement->MovementMode==MOVE_Flying)
            HomeSeatRuntime.PreviousMovementMode=MOVE_Walking;
        HomeSeatRuntime.bSavedOrientToMovement=Movement->bOrientRotationToMovement;
        HomeSeatRuntime.bSavedControllerRotation=Movement->bUseControllerDesiredRotation;
        HomeSeatRuntime.bSavedMovement=true;
        HomeSeatRuntime.bAddedMoveIgnore=!Npc->GetCapsuleComponent()->GetMoveIgnoreActors().Contains(State.Seat.Get());
        if (HomeSeatRuntime.bAddedMoveIgnore) Npc->GetCapsuleComponent()->IgnoreActorWhenMoving(State.Seat,true);
    }
    auto Source=Movement->GetRootMotionSourceByID(HomeSeatRuntime.MotionId);
    if (Source.IsValid() && Source->InstanceName!=HomeSeatRuntime.MotionName) Source.Reset();
    if (!State.bMoving || HomeSeatRuntime.AppliedRevision!=State.Revision)
    {
        if (Source.IsValid()) Movement->RemoveRootMotionSourceByID(HomeSeatRuntime.MotionId);
        HomeSeatRuntime.MotionId=0;
        Source.Reset();
    }
    if (HasOtherSeatMotion(Movement,HomeSeatRuntime.MotionName)) return;
    const bool bNewMove=HomeSeatRuntime.AppliedRevision!=State.Revision;
    if (bNewMove || !Npc->HasAuthority())
    {
        Movement->bOrientRotationToMovement=false;
        Movement->bUseControllerDesiredRotation=false;
        Movement->SetMovementMode(MOVE_Flying);
        if (!WeakThis.IsValid() || !WeakNpc.IsValid() || !WeakMovement.IsValid() || Npc->GetCharacterMovement()!=Movement ||
            HomeState.SeatMotion.ArrangementId!=State.ArrangementId ||
            HomeState.SeatMotion.Revision!=State.Revision || !HomeState.SeatMotion.bActive) return;
    }
    HomeSeatRuntime.AppliedRevision=State.Revision;
    if (!State.bMoving) return;
    if (Source.IsValid() && Source->Status.HasFlag(ERootMotionSourceStatusFlags::Finished))
        HomeSeatRuntime.FinishedRevision=State.Revision;
    const double Elapsed=FMath::Max(0.0,Now-State.StartedServerTime);
    if (Elapsed>=State.Duration || HomeSeatRuntime.FinishedRevision==State.Revision) return;
    if (Source.IsValid() && !Source->Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval)) return;
    auto Move=MakeShared<FRootMotionSource_MoveToForce>();
    Move->InstanceName=FName(*FString::Printf(TEXT("CompanionSeat_%s_%d"),*State.ArrangementId.ToString(EGuidFormats::Digits),State.Revision));
    Move->Priority=900;
    Move->AccumulateMode=ERootMotionAccumulateMode::Override;
    Move->bInLocalSpace=false;
    Move->StartLocation=State.Start;
    Move->TargetLocation=State.Target;
    Move->Duration=State.Duration;
    Move->bRestrictSpeedToExpected=true;
    Move->FinishVelocityParams.Mode=ERootMotionFinishVelocityMode::SetVelocity;
    Move->FinishVelocityParams.SetVelocity=FVector::ZeroVector;
    if (!Npc->HasAuthority()) Move->SetTime(static_cast<float>(Elapsed));
    HomeSeatRuntime.MotionName=Move->InstanceName;
    HomeSeatRuntime.MotionId=Movement->ApplyRootMotionSource(Move);
}

/// @brief 开始坐下或起身的移动，时长取动画长度；失败时 OutReason 写入原因
bool UDeliveryCompanionComponent::BeginHomeSeatMove(const FTransform& FeetGoal,UAnimSequence* Animation,FName& OutReason)
{
    OutReason=NAME_None;
    auto* Npc=GetCompanion();
    auto* Movement=Npc?Npc->GetCharacterMovement():nullptr;
    auto* Object=HomeUseClaim.Object.Get();
    const FGuid Id=HomeState.ArrangementId;
    const float Duration=IsValid(Animation)?Animation->GetPlayLength():0;
    if (!Npc || !Npc->HasAuthority() || Npc->IsKnockedDown() || !Movement || !IsCurrentHome(Id) || !Object ||
        !Object->ValidateClaim(HomeUseClaim,false,OutReason) || !IsValid(Animation) ||
        !FMath::IsFinite(Duration) || Duration<.1f || Duration>30.f || FeetGoal.ContainsNaN() ||
        !FeetGoal.GetRotation().IsNormalized() || FVector::DotProduct(FeetGoal.GetRotation().GetUpVector(),FVector::UpVector)<.9998f ||
        HasOtherSeatMotion(Movement,HomeSeatRuntime.MotionName))
    { if (OutReason.IsNone()) OutReason=TEXT("HomeSeatMovementUnavailable"); return false; }
    const auto* Mesh=Npc->GetMesh()?Npc->GetMesh()->GetSkeletalMeshAsset():nullptr;
    if (!Mesh || Animation->GetSkeleton()!=Mesh->GetSkeleton())
    { OutReason=TEXT("HomeSeatAnimationSkeletonMismatch"); return false; }
    if ((HomeSeatRuntime.bSavedMovement && Movement->MovementMode!=MOVE_Flying) ||
        (!HomeSeatRuntime.bSavedMovement && !Movement->IsMovingOnGround()))
    { OutReason=TEXT("HomeSeatMovementChanged"); return false; }
    auto* Seat=Object->GetOwner();
    auto* Capsule=Npc->GetCapsuleComponent();
    const FVector Target=SeatRootGoal(Npc,FeetGoal);
    const FVector Start=Npc->GetActorLocation();
    if (!IsValid(Seat) || Seat->IsActorBeingDestroyed() || FVector::Dist(Start,Target)>500.f)
    { OutReason=TEXT("HomeSeatTransitionTooFar"); return false; }
    FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionSeatMove),false,Npc);
    Query.AddIgnoredActor(Seat);
    FHitResult Hit;
    if (GetWorld()->SweepSingleByChannel(Hit,Start,Target,Npc->GetActorQuat(),Capsule->GetCollisionObjectType(),
        FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(),Capsule->GetScaledCapsuleHalfHeight()),
        Query,FCollisionResponseParams(Capsule->GetCollisionResponseToChannels())))
    { OutReason=TEXT("HomeSeatTransitionBlocked"); return false; }
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakNpc(Npc);
    const TWeakObjectPtr<UCharacterMovementComponent> WeakMovement(Movement);
    const TWeakObjectPtr<UDeliveryHomeObjectComponent> WeakObject(Object);
    const TWeakObjectPtr<AActor> WeakSeat(Seat);
    const FDeliveryHomeUseClaim Claim=HomeUseClaim;
    StopHomeMovement();
    if (!WeakThis.IsValid() || !WeakNpc.IsValid() || !WeakMovement.IsValid() || !WeakObject.IsValid() || !WeakSeat.IsValid() ||
        Npc->GetCharacterMovement()!=Movement || !IsCurrentHome(Id) || HomeUseClaim.ActionId!=Claim.ActionId ||
        HomeUseClaim.Object!=WeakObject || !Object->ValidateClaim(Claim,false,OutReason))
    { OutReason=TEXT("HomeArrangementChanged"); return false; }
    auto& State=HomeState.SeatMotion;
    State.ArrangementId=Id; State.Seat=Seat;
    State.Revision=State.Revision==MAX_int32?1:State.Revision+1;
    State.Start=Start; State.Target=Target; State.StartedServerTime=GetHomeServerTime(); State.Duration=Duration;
    State.bActive=true; State.bMoving=true;
    UpdateHomeSeatPresentation();
    if (!WeakThis.IsValid() || !WeakNpc.IsValid() || !WeakMovement.IsValid() || !IsCurrentHome(Id) || !HomeSeatRuntime.MotionId)
    { OutReason=TEXT("HomeSeatMovementUnavailable"); return false; }
    SetHomePose(Animation,false,false);
    OutReason=NAME_None;
    return true;
}

/// @brief 检查坐下或起身的移动是否还对应当前安排，移动结束时返回 true
bool UDeliveryCompanionComponent::UpdateHomeSeatMove(const FTransform& FeetGoal,FName& OutReason)
{
    OutReason=NAME_None;
    auto* Npc=GetCompanion();
    auto* Movement=Npc?Npc->GetCharacterMovement():nullptr;
    const auto State=HomeState.SeatMotion;
    if (!Npc || !Movement || !Npc->HasAuthority() || !State.bActive || State.ArrangementId!=HomeState.ArrangementId ||
        FeetGoal.ContainsNaN() || !State.Target.Equals(SeatRootGoal(Npc,FeetGoal),2.f) ||
        Movement->MovementMode!=MOVE_Flying || HasOtherSeatMotion(Movement,HomeSeatRuntime.MotionName))
    { OutReason=TEXT("HomeSeatMovementChanged"); return false; }
    if (GetHomeServerTime()-State.StartedServerTime<State.Duration) return false;
    // RMS 到期只是时序条件，真实胶囊未到位不能记录已经坐好或已经离开
    if (FVector::Dist(Npc->GetActorLocation(),State.Target)>FMath::Min(HomeUseReachTolerance,6.f))
    { OutReason=TEXT("HomeSeatDidNotReachGoal"); return false; }
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakNpc(Npc);
    const TWeakObjectPtr<UCharacterMovementComponent> WeakMovement(Movement);
    HomeState.SeatMotion.bMoving=false;
    UpdateHomeSeatPresentation();
    if (!WeakThis.IsValid() || !WeakNpc.IsValid() || !WeakMovement.IsValid() || !IsCurrentHome(State.ArrangementId))
    { OutReason=TEXT("HomeArrangementChanged"); return false; }
    Movement->StopMovementImmediately();
    GetOwner()->ForceNetUpdate();
    return true;
}
