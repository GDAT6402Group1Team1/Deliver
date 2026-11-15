#include "AI/Presentation/DeliveryNpcBodyAnimInstance.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "Engine/World.h"

namespace
{
float ClipTime(UAnimSequence* Animation,double Now,double StartedAt)
{
    if (!Animation || StartedAt<0 || !FMath::IsFinite(StartedAt) || !FMath::IsFinite(Now)) return 0;
    return static_cast<float>(FMath::Clamp(Now-StartedAt,0.0,static_cast<double>(Animation->GetPlayLength())));
}

UAnimSequence* SelectJumpAnimation(UAnimSequence* Override,UAnimSequence* BlueprintDefault,const USkeleton* Skeleton)
{
    const auto CanPlay=[Skeleton](const UAnimSequence* Clip)
    {
        // 这批剪辑的根在单位姿势，锁定零位让蓝图独立恢复身体尺寸
        return Skeleton && IsValid(Clip) && Clip->GetSkeleton()==Skeleton && !Clip->HasRootMotion() &&
            Clip->GetAdditiveAnimType()==AAT_None && Clip->bForceRootLock && Clip->RootMotionRootLock==ERootMotionRootLock::Zero &&
            FMath::IsFinite(Clip->GetPlayLength()) && Clip->GetPlayLength()>0;
    };
    if (CanPlay(Override)) return Override;
    return CanPlay(BlueprintDefault)?BlueprintDefault:nullptr;
}
}

void UDeliveryNpcBodyAnimInstance::NativeInitializeAnimation()
{
    Super::NativeInitializeAnimation();
    PoseIndex=static_cast<int32>(EPose::Idle);
    MovePlayRate=1;
    StrikeTime=DownTime=GetUpTime=HitTime=0;
    JumpStartTime=JumpLoopTime=JumpLandTime=0;
    bMoving=false;
    ResetJumpPose();
    SetRootMotionMode(ERootMotionMode::IgnoreRootMotion);
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwningActor());
    if (!Npc || !Npc->Presentation) return;
    const auto* P=Npc->Presentation.Get();
    IdleAnimation=P->IdleAnimation;
    TalkAnimation=P->TalkAnimation?P->TalkAnimation:P->IdleAnimation;
    WalkAnimation=P->WalkAnimation;
    PaceAnimation=P->PaceAnimation?P->PaceAnimation:P->WalkAnimation;
    HurryAnimation=P->HurryAnimation?P->HurryAnimation:P->WalkAnimation;
    HoldAnimation=P->HoldAnimation;
    AttackAnimation=P->AttackAnimation;
    DownAnimation=P->DownAnimation;
    GetUpAnimation=P->GetUpAnimation;
    HitAnimation=P->HitAnimation;
    const auto* Mesh=GetSkelMeshComponent();
    const auto* MeshAsset=Mesh?Mesh->GetSkeletalMeshAsset():nullptr;
    const auto* Skeleton=MeshAsset?MeshAsset->GetSkeleton():nullptr;
    const auto* Defaults=GetClass()->GetDefaultObject<UDeliveryNpcBodyAnimInstance>();
    JumpStartAnimation=SelectJumpAnimation(P->JumpStartAnimation,Defaults->JumpStartAnimation,Skeleton);
    JumpLoopAnimation=SelectJumpAnimation(P->JumpLoopAnimation,Defaults->JumpLoopAnimation,Skeleton);
    JumpLandAnimation=SelectJumpAnimation(P->JumpLandAnimation,Defaults->JumpLandAnimation,Skeleton);
}

void UDeliveryNpcBodyAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
    Super::NativeUpdateAnimation(DeltaSeconds);
    // 游戏线程先读取角色，动画图随后使用这些数值，不在工作线程访问玩法对象
    if (!IsInGameThread()) return;
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwningActor());
    if (!IsValid(Npc) || !Npc->Presentation || !Npc->Combat || !Npc->Agent || !Npc->GetWorld())
    {
        PoseIndex=static_cast<int32>(EPose::Idle);
        StrikeTime=DownTime=GetUpTime=HitTime=0;
        MovePlayRate=1;
        bMoving=false;
        ResetJumpPose();
        return;
    }
    const auto* World=Npc->GetWorld();
    const auto* GameState=World->GetGameState();
    const double Now=GameState?GameState->GetServerWorldTimeSeconds():World->GetTimeSeconds();
    const auto State=Npc->Agent->GetPublicState();
    const bool bRunning=State.Status==EDeliveryNpcActionStatus::Running;
    const bool bAttackAction=State.Action==EDeliveryNpcAction::StrikeOnce || State.Action==EDeliveryNpcAction::Fight;
    const double StrikeStartedAt=Npc->Combat->GetStrikeStartTime(State.ActionId);
    const double StrikeElapsed=StrikeStartedAt>=0?Now-StrikeStartedAt:-1;
    StrikeTime=ClipTime(AttackAnimation,Now,StrikeStartedAt);
    DownTime=ClipTime(DownAnimation,Now,Npc->Combat->GetDownStartTime());
    GetUpTime=ClipTime(GetUpAnimation,Now,Npc->Combat->GetRecoveryStartTime());
    const double HitStartedAt=Npc->Combat->GetHitReactionStartTime();
    HitTime=0;
    if (HitAnimation && HitStartedAt>=0 && FMath::IsFinite(HitStartedAt) && FMath::IsFinite(Now))
    {
        // 在短暂后仰期间播完整个受击片段，命中判定和角色位移仍由战斗组件处理
        const double Progress=FMath::Clamp((Now-HitStartedAt)/UDeliveryNpcCombatComponent::HitReactionDuration,0.0,1.0);
        HitTime=static_cast<float>(Progress*HitAnimation->GetPlayLength());
    }

    const float Speed=static_cast<float>(Npc->GetVelocity().Size2D());
    const float Start=FMath::IsFinite(MoveStartSpeed)?FMath::Max(1.f,MoveStartSpeed):24.f;
    const float Stop=FMath::IsFinite(MoveStopSpeed)?FMath::Clamp(MoveStopSpeed,0.f,Start):12.f;
    bMoving=FMath::IsFinite(Speed) && (bMoving?Speed>Stop:Speed>=Start);
    const auto* Companion=Cast<ADeliveryCompanionCharacter>(Npc);
    const bool bEncounterPose=Companion && (Companion->IsBeingHelpedUp() || Companion->IsWaitingForHelp() || Companion->IsEncounterAirborne());
    const bool bJumpAllowed=!bEncounterPose && !Npc->GetAttachParentActor() && !Npc->Combat->IsRecovering() &&
        !Npc->Combat->IsDown() && !Npc->Combat->IsReactingToHit() && !Npc->Combat->IsStriking();
    const EPose AirPose=UpdateJumpPose(bJumpAllowed?Npc->GetCharacterMovement():nullptr,DeltaSeconds);
    EPose Pose=EPose::Idle;
    float ReferenceSpeed=Npc->Presentation->WalkAnimationSpeed;
    if (Npc->Combat->IsRecovering()) Pose=EPose::GetUp;
    else if (Npc->Combat->IsDown()) Pose=EPose::Down;
    else if (Npc->Combat->IsReactingToHit() && HitAnimation) Pose=EPose::Hit;
    else if (bRunning && bAttackAction &&
        StrikeElapsed>=0 && StrikeElapsed<UDeliveryNpcCombatComponent::StrikeDuration) Pose=EPose::Strike;
    else if (bRunning && State.Action==EDeliveryNpcAction::HoldVehicle) Pose=EPose::Hold;
    else if (AirPose!=EPose::Idle) Pose=AirPose;
    else if (bMoving)
    {
        Pose=EPose::Walk;
        const bool bPacing=bRunning && State.Action==EDeliveryNpcAction::Pace;
        const bool bMovingAside=bRunning && State.Action==EDeliveryNpcAction::MoveAside;
        const FDeliveryNpcBehaviorPresentation* Variant=nullptr;
        if (bPacing || bMovingAside)
        {
            static const FGameplayTag PaceTag=FGameplayTag::RequestGameplayTag(TEXT("Npc.Behavior.Movement.Pace"),false);
            static const FGameplayTag MoveAsideTag=FGameplayTag::RequestGameplayTag(TEXT("Npc.Behavior.Movement.MoveAside"),false);
            Variant=Npc->FindBehaviorPresentation(bPacing?PaceTag:MoveAsideTag);
        }
        if (bPacing || Variant)
        {
            // 踱步和让路都使用蓝图已有的短步混合槽
            Pose=EPose::Pace;
            if (bPacing) ReferenceSpeed=Npc->Presentation->PaceAnimationSpeed;
            if (Variant)
            {
                PaceAnimation=Variant->Animation;
                if (Variant->MovementReferenceSpeed>0) ReferenceSpeed=Variant->MovementReferenceSpeed;
            }
            else PaceAnimation=Npc->Presentation->PaceAnimation?Npc->Presentation->PaceAnimation:Npc->Presentation->WalkAnimation;
        }
        else if (bRunning && (bAttackAction || State.Action==EDeliveryNpcAction::Flee))
        { Pose=EPose::Hurry; ReferenceSpeed=Npc->Presentation->HurryAnimationSpeed; }
    }
    // 缺音时暂停说话动作，声音接上后继续，字幕和注视仍保留当前句子
    else if (Npc->Presentation->GetDisplayedSpeech().bActive && !Npc->Presentation->IsWaitingForMoreAudio()) Pose=EPose::Talk;
    PoseIndex=static_cast<int32>(Pose);
    // 初遇等待扶起独立于战斗倒地，保持原动画蓝图的 Down / GetUp 输入
    if (Companion)
    {
        if (!Npc->Combat->IsDown() && Companion->IsBeingHelpedUp())
        {
            PoseIndex=static_cast<int32>(EPose::GetUp);
            GetUpTime=ClipTime(GetUpAnimation,Now,Companion->GetHelpStartTime());
        }
        else if (!Npc->Combat->IsDown() && (Companion->IsWaitingForHelp() || Companion->IsEncounterAirborne()))
        {
            PoseIndex=static_cast<int32>(EPose::Down);
            DownTime=ClipTime(DownAnimation,Now,Companion->GetEncounterFallStartTime());
        }
    }
    if (!FMath::IsFinite(ReferenceSpeed)) ReferenceSpeed=180;
    MovePlayRate=FMath::IsFinite(Speed)?FMath::Clamp(Speed/FMath::Max(10.f,ReferenceSpeed),.2f,2.f):1.f;
}

UDeliveryNpcBodyAnimInstance::EPose UDeliveryNpcBodyAnimInstance::UpdateJumpPose(
    const UCharacterMovementComponent* Movement,float DeltaSeconds)
{
    if (!Movement || !JumpStartAnimation || !JumpLoopAnimation || !JumpLandAnimation)
    {
        ResetJumpPose();
        return JumpPose;
    }

    const float Step=FMath::IsFinite(DeltaSeconds)?FMath::Max(0.f,DeltaSeconds):0.f;
    const auto SetPhase=[this](EPose Phase)
    {
        JumpPose=Phase;
        JumpPhaseSeconds=0;
    };
    JumpPhaseSeconds+=Step;
    if (Movement->IsFalling())
    {
        const bool bRising=FMath::IsFinite(Movement->Velocity.Z) && Movement->Velocity.Z>0;
        if (JumpPose==EPose::Idle || JumpPose==EPose::JumpLand) SetPhase(bRising?EPose::JumpStart:EPose::JumpLoop);
        else if (JumpPose==EPose::JumpStart && (!bRising || JumpPhaseSeconds>=JumpStartAnimation->GetPlayLength()))
            SetPhase(EPose::JumpLoop);
        // 走下坡沿、远端首次观察到下落时，直接进入空中循环
        // 混合仍会采样上一个输入，切换时保留上一段的最后一次位置
        if (JumpPose==EPose::JumpStart) JumpStartTime=JumpPhaseSeconds;
        else JumpLoopTime=FMath::Fmod(JumpPhaseSeconds,JumpLoopAnimation->GetPlayLength());
    }
    else if (Movement->IsMovingOnGround())
    {
        if (JumpPose==EPose::JumpStart || JumpPose==EPose::JumpLoop) SetPhase(EPose::JumpLand);
        if (JumpPose==EPose::JumpLand && JumpPhaseSeconds<JumpLandAnimation->GetPlayLength()) JumpLandTime=JumpPhaseSeconds;
        else ResetJumpPose();
    }
    else ResetJumpPose();
    return JumpPose;
}

void UDeliveryNpcBodyAnimInstance::ResetJumpPose()
{
    JumpPose=EPose::Idle;
    JumpPhaseSeconds=0;
}

void UDeliveryNpcBodyAnimInstance::NativeUninitializeAnimation()
{
    PoseIndex=static_cast<int32>(EPose::Idle);
    StrikeTime=DownTime=GetUpTime=HitTime=0;
    JumpStartTime=JumpLoopTime=JumpLandTime=0;
    MovePlayRate=1;
    bMoving=false;
    ResetJumpPose();
    Super::NativeUninitializeAnimation();
}
