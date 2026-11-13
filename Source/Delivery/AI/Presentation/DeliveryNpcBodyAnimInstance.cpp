#include "AI/Presentation/DeliveryNpcBodyAnimInstance.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Animation/AnimSequence.h"
#include "GameFramework/GameStateBase.h"
#include "Engine/World.h"

namespace
{
float ClipTime(UAnimSequence* Animation,double Now,double StartedAt)
{
    if (!Animation || StartedAt<0 || !FMath::IsFinite(StartedAt) || !FMath::IsFinite(Now)) return 0;
    return static_cast<float>(FMath::Clamp(Now-StartedAt,0.0,static_cast<double>(Animation->GetPlayLength())));
}
}

void UDeliveryNpcBodyAnimInstance::NativeInitializeAnimation()
{
    Super::NativeInitializeAnimation();
    PoseIndex=static_cast<int32>(EPose::Idle);
    MovePlayRate=1;
    StrikeTime=DownTime=GetUpTime=HitTime=0;
    bMoving=false;
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
    EPose Pose=EPose::Idle;
    float ReferenceSpeed=Npc->Presentation->WalkAnimationSpeed;
    if (Npc->Combat->IsRecovering()) Pose=EPose::GetUp;
    else if (Npc->Combat->IsDown()) Pose=EPose::Down;
    else if (Npc->Combat->IsReactingToHit() && HitAnimation) Pose=EPose::Hit;
    else if (bRunning && bAttackAction &&
        StrikeElapsed>=0 && StrikeElapsed<UDeliveryNpcCombatComponent::StrikeDuration) Pose=EPose::Strike;
    else if (bRunning && State.Action==EDeliveryNpcAction::HoldVehicle) Pose=EPose::Hold;
    else if (bMoving)
    {
        Pose=EPose::Walk;
        if (bRunning && State.Action==EDeliveryNpcAction::Pace)
        { Pose=EPose::Pace; ReferenceSpeed=Npc->Presentation->PaceAnimationSpeed; }
        else if (bRunning && bAttackAction)
        { Pose=EPose::Hurry; ReferenceSpeed=Npc->Presentation->HurryAnimationSpeed; }
    }
    else if (Npc->Presentation->GetDisplayedSpeech().bActive) Pose=EPose::Talk;
    PoseIndex=static_cast<int32>(Pose);
    if (!FMath::IsFinite(ReferenceSpeed)) ReferenceSpeed=180;
    MovePlayRate=FMath::IsFinite(Speed)?FMath::Clamp(Speed/FMath::Max(10.f,ReferenceSpeed),.2f,2.f):1.f;
}

void UDeliveryNpcBodyAnimInstance::NativeUninitializeAnimation()
{
    PoseIndex=static_cast<int32>(EPose::Idle);
    StrikeTime=DownTime=GetUpTime=HitTime=0;
    MovePlayRate=1;
    bMoving=false;
    Super::NativeUninitializeAnimation();
}
