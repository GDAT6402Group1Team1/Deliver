#include "AI/Companion/DeliveryCompanionBodyAnimInstance.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "Animation/AnimSequence.h"
#include "GameFramework/GameStateBase.h"
#include "Engine/World.h"

void UDeliveryCompanionBodyAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
    Super::NativeUpdateAnimation(DeltaSeconds);
    if (!IsInGameThread()) return;
    bHomePose=false;
    bHomeUpperBodyPose=false;
    HomeAnimationTime=0;
    HomeAnimation=nullptr;
    const auto* Npc=Cast<ADeliveryCompanionCharacter>(GetOwningActor());
    if (!IsValid(Npc) || !Npc->Companion || !Npc->Combat || !Npc->GetWorld() ||
        Npc->IsKnockedDown() || Npc->Combat->IsRecovering() || Npc->Combat->IsReactingToHit()) return;
    const auto& State=Npc->Companion->GetHomeState();
    if (!IsValid(State.PoseAnimation)) return;
    const auto* Clock=Npc->GetWorld()->GetGameState();
    if (!Clock && Npc->GetWorld()->GetNetMode()==NM_Client) return;
    const double Now=Clock?Clock->GetServerWorldTimeSeconds():Npc->GetWorld()->GetTimeSeconds();
    const double Length=State.PoseAnimation->GetPlayLength();
    if (!FMath::IsFinite(Now) || !FMath::IsFinite(State.PoseStartedServerTime) || !FMath::IsFinite(Length) || Length<=0) return;
    const double Elapsed=FMath::Max(0.0,Now-State.PoseStartedServerTime);
    HomeAnimation=State.PoseAnimation;
    HomeAnimationTime=static_cast<float>(State.bLoopPose?FMath::Fmod(Elapsed,Length):FMath::Min(Elapsed,Length));
    bHomePose=!State.bUpperBodyPose;
    bHomeUpperBodyPose=State.bUpperBodyPose;
}

void UDeliveryCompanionBodyAnimInstance::NativeUninitializeAnimation()
{
    HomeAnimation=nullptr;
    HomeAnimationTime=0;
    bHomePose=false;
    bHomeUpperBodyPose=false;
    Super::NativeUninitializeAnimation();
}
