#include "AI/Presentation/Animation/DeliveryNpcBodyAnimInstance.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "Engine/World.h"

// ── 选姿势 ──
// 主动画蓝图 ABP_NpcBody 每帧调用 NativeUpdateAnimation：按优先顺序挑出 PoseIndex，并算出出拳、倒地、起身、受击各播到第几秒
// 这些动作的开始时间来自战斗组件，用服务器时间记录，晚收到消息的客户端算出的进度和服务器一样
// 动画蓝图只读这些数值，按 PoseIndex 在 Blend Poses 里切换片段
namespace
{
/// @brief 从 StartedAt 到 Now 过了多久，限制在片段长度以内；没有开始时间时返回 0
float ClipTime(UAnimSequence* Animation,double Now,double StartedAt)
{
    if (!Animation || StartedAt<0 || !FMath::IsFinite(StartedAt) || !FMath::IsFinite(Now)) return 0;
    return static_cast<float>(FMath::Clamp(Now-StartedAt,0.0,static_cast<double>(Animation->GetPlayLength())));
}

/// @brief 选跳跃片段：先用人物配置的 Override，不能播再用蓝图默认值，都不能播返回 nullptr
/// 能播的条件：骨架相同、没有根运动、不是叠加动画、根骨锁在零位、长度大于 0
UAnimSequence* SelectJumpAnimation(UAnimSequence* Override,UAnimSequence* BlueprintDefault,const USkeleton* Skeleton)
{
    const auto CanPlay=[Skeleton](const UAnimSequence* Clip)
    {
        // 这批跳跃片段的根骨是单位变换，要求 bForceRootLock 并锁在 Zero，身体的尺寸和位置由蓝图另外处理
        return Skeleton && IsValid(Clip) && Clip->GetSkeleton()==Skeleton && !Clip->HasRootMotion() &&
            Clip->GetAdditiveAnimType()==AAT_None && Clip->bForceRootLock && Clip->RootMotionRootLock==ERootMotionRootLock::Zero &&
            FMath::IsFinite(Clip->GetPlayLength()) && Clip->GetPlayLength()>0;
    };
    if (CanPlay(Override)) return Override;
    return CanPlay(BlueprintDefault)?BlueprintDefault:nullptr;
}
}

/// @brief 动画开始时清空状态，关闭根运动（IgnoreRootMotion），并从 NPC 的表现组件读取各个动画片段
/// 说话、踱步、快走没有配置时分别用站立、走路代替；三段跳跃片段经 SelectJumpAnimation 检查后才用
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

/// @brief 每帧在游戏线程挑姿势：起身 → 倒地 → 受击 → 出拳 → 拽车 → 跳跃 → 走路（踱步、快走）→ 说话 → 站立
/// 同时算各片段播到第几秒，以及走路动画的播放速率 MovePlayRate（实际速度除以片段的参考速度，限制在 0.2 到 2）
void UDeliveryNpcBodyAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
    Super::NativeUpdateAnimation(DeltaSeconds);

    // 只在游戏线程读角色和组件；动画在工作线程更新时直接返回，用上一帧算好的值
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
        // 受击片段按 HitReactionDuration 拉伸，后仰期间正好播完一遍；这里只算播放进度，命中和位移由战斗组件处理
        const double Progress=FMath::Clamp((Now-HitStartedAt)/UDeliveryNpcCombatComponent::HitReactionDuration,0.0,1.0);
        HitTime=static_cast<float>(Progress*HitAnimation->GetPlayLength());
    }

    // 速度超过 MoveStartSpeed 才算开始走，低于 MoveStopSpeed 才算停下；两个门槛不同，速度在中间时保持上一帧的状态，不会来回切
    const float Speed=static_cast<float>(Npc->GetVelocity().Size2D());
    const float Start=FMath::IsFinite(MoveStartSpeed)?FMath::Max(1.f,MoveStartSpeed):24.f;
    const float Stop=FMath::IsFinite(MoveStopSpeed)?FMath::Clamp(MoveStopSpeed,0.f,Start):12.f;
    bMoving=FMath::IsFinite(Speed) && (bMoving?Speed>Stop:Speed>=Start);

    // 二狗初遇时被扶起、等人扶、被撞飞在空中，以及挂在别的物体上、起身、倒地、受击、出拳时，不播跳跃
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
            // 踱步（Pace）和让路（MoveAside）都用 Pace 槽位；行为表里配了这个行为的专用动画就换上它和它的参考速度
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

    // 台词正在显示时播说话动作；IsWaitingForMoreAudio 为 true（声音没收完、播放暂停）时改回站立，字幕和看人不受影响
    else if (Npc->Presentation->GetDisplayedSpeech().bActive && !Npc->Presentation->IsWaitingForMoreAudio()) Pose=EPose::Talk;
    PoseIndex=static_cast<int32>(Pose);

    // 二狗初遇时的倒地和被扶起不经过战斗组件，这里直接改用 Down、GetUp 槽位，进度按二狗记录的开始时间算
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

/// @brief 按角色移动组件推进跳跃三段：离地后上升播 JumpStart，下落播 JumpLoop（循环），落地后播完 JumpLand 再回到 Idle
/// Movement 为空或缺少跳跃片段时回到 Idle；返回当前所在的段
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

        // 刚离地时上升就播 JumpStart；走下坡沿时一开始就在下落，或者客户端第一次看到时已经在下落，直接从 JumpLoop 开始
        if (JumpPose==EPose::Idle || JumpPose==EPose::JumpLand) SetPhase(bRising?EPose::JumpStart:EPose::JumpLoop);
        else if (JumpPose==EPose::JumpStart && (!bRising || JumpPhaseSeconds>=JumpStartAnimation->GetPlayLength()))
            SetPhase(EPose::JumpLoop);

        // 只更新当前段的播放时间，其他段的时间保持原值：Blend Poses 过渡时还会采样上一段，停在最后的位置才不会跳帧
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

/// @brief 回到不跳的状态，清掉这一段已播的时间
void UDeliveryNpcBodyAnimInstance::ResetJumpPose()
{
    JumpPose=EPose::Idle;
    JumpPhaseSeconds=0;
}

/// @brief 动画结束时把姿势和各段时间清回初始值
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
