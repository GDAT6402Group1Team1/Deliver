#include "AI/Companion/DeliveryCompanionBodyAnimInstance.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "Vehicle/DeliveryCompanionMotorbike.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
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
    if (!IsValid(Npc) || Npc->IsActorBeingDestroyed() || !IsValid(Npc->Companion) || !Npc->Combat || !Npc->GetWorld() ||
        Npc->IsKnockedDown() || Npc->Combat->IsRecovering() || Npc->Combat->IsReactingToHit()) return;
    const auto* Bike=Cast<ADeliveryCompanionMotorbike>(Npc->GetAttachParentActor());
    if (Npc->Companion->IsUsingErrandVehicle() || (IsValid(Bike) && Bike->GetWorld()==Npc->GetWorld() &&
        Bike->IsMounted() && Bike->GetReservedCompanion()==Npc)) return;
    const auto* Clock=Npc->GetWorld()->GetGameState();
    if (!Clock && Npc->GetWorld()->GetNetMode()==NM_Client) return;
    const double Now=Clock?Clock->GetServerWorldTimeSeconds():Npc->GetWorld()->GetTimeSeconds();
    if (!FMath::IsFinite(Now)) return;
    const auto& State=Npc->Companion->GetHomeState();
    if (IsValid(State.PoseAnimation))
    {
        const double Length=State.PoseAnimation->GetPlayLength();
        if (!FMath::IsFinite(State.PoseStartedServerTime) || !FMath::IsFinite(Length) || Length<=0) return;
        const double Elapsed=FMath::Max(0.0,Now-State.PoseStartedServerTime);
        HomeAnimation=State.PoseAnimation;
        HomeAnimationTime=static_cast<float>(State.bLoopPose?FMath::Fmod(Elapsed,Length):FMath::Min(Elapsed,Length));
        bHomePose=!State.bUpperBodyPose;
        bHomeUpperBodyPose=State.bUpperBodyPose;
        return;
    }
    // 普通持物只读取真实单槽，不制造家居安排或占用；家居和战斗动作优先
    if (Npc->Companion->HasActiveHomeArrangement() || Npc->Combat->IsStriking() || !IsValid(Npc->Agent)) return;
    const auto& Action=Npc->Agent->GetPublicState();
    if ((Action.Status==EDeliveryNpcActionStatus::Accepted || Action.Status==EDeliveryNpcActionStatus::Running) &&
        (Action.Action==EDeliveryNpcAction::Fight || Action.Action==EDeliveryNpcAction::StrikeOnce ||
         Action.Action==EDeliveryNpcAction::HoldVehicle)) return;
    const auto* Item=Npc->Companion->GetCarriedItem();
    const auto* Mesh=Npc->GetMesh();
    const auto* MeshAsset=IsValid(Mesh)?Mesh->GetSkeletalMeshAsset():nullptr;
    if (!DeliveryHandheldTransfer::IsOrdinaryItem(Item) || Item->GetOwner()!=Npc || Item->GetWorld()!=Npc->GetWorld() ||
        !IsValid(Npc->HandheldAnchor) || !IsValid(Item->GetRootComponent()) ||
        Item->GetRootComponent()->GetAttachParent()!=Npc->HandheldAnchor ||
        Npc->HandheldAnchor->GetAttachParent()!=Mesh || Npc->HandheldAnchor->GetAttachSocketName()!=Item->NpcHandSocket ||
        !IsValid(Item->NpcCarryAnimation) || !IsValid(MeshAsset) || !MeshAsset->GetSkeleton() ||
        Item->NpcCarryAnimation->GetSkeleton()!=MeshAsset->GetSkeleton()) return;
    const double Length=Item->NpcCarryAnimation->GetPlayLength();
    if (!FMath::IsFinite(Length) || Length<=0 || Item->NpcCarryAnimation->HasRootMotion()) return;
    HomeAnimation=Item->NpcCarryAnimation;
    // 同一服务器时钟只决定循环相位，不增加另一份复制持物状态
    HomeAnimationTime=static_cast<float>(FMath::Fmod(FMath::Max(0.0,Now),Length));
    bHomeUpperBodyPose=true;
}

void UDeliveryCompanionBodyAnimInstance::NativeUninitializeAnimation()
{
    HomeAnimation=nullptr;
    HomeAnimationTime=0;
    bHomePose=false;
    bHomeUpperBodyPose=false;
    Super::NativeUninitializeAnimation();
}
