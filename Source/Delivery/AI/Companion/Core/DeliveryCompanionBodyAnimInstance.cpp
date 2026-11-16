#include "AI/Companion/Core/DeliveryCompanionBodyAnimInstance.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Companion/Core/DeliveryCompanionTuning.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Inventory/Items/DeliveryHandheldItem.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "Vehicle/Companion/DeliveryCompanionMotorbike.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/AnimSequence.h"
#include "GameFramework/CharacterMovementComponent.h"
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
    if (IsPlayingJumpPose()) return;
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
    // 家居、受击与正在执行的交战动作优先于打量或普通持物
    if (Npc->Companion->HasActiveHomeArrangement() || Npc->Combat->IsStriking() || !IsValid(Npc->Agent)) return;
    const auto& Action=Npc->Agent->GetPublicState();
    if ((Action.Status==EDeliveryNpcActionStatus::Accepted || Action.Status==EDeliveryNpcActionStatus::Running) &&
        (Action.Action==EDeliveryNpcAction::Fight || Action.Action==EDeliveryNpcAction::StrikeOnce ||
         Action.Action==EDeliveryNpcAction::HoldVehicle)) return;
    const auto* Mesh=Npc->GetMesh();
    const auto* MeshAsset=IsValid(Mesh)?Mesh->GetSkeletalMeshAsset():nullptr;
    const auto* Skeleton=IsValid(MeshAsset)?MeshAsset->GetSkeleton():nullptr;
    const auto Errand=Npc->Companion->GetErrand();
    if (Errand.Status==EDeliveryNpcActionStatus::Running && Errand.Action==EDeliveryNpcAction::DropPackage &&
        Errand.GroundPlacementServerTime>=0 && FMath::IsFinite(Errand.GroundPlacementServerTime))
    {
        auto* Place=Npc->Companion->GetTuning().GroundPickupAnimation.Get();
        const double Elapsed=Now-Errand.GroundPlacementServerTime;
        if (IsValid(Place) && Skeleton && Place->GetSkeleton()==Skeleton && !Place->HasRootMotion() &&
            Elapsed>=0 && Elapsed<Place->GetPlayLength())
        {
            HomeAnimation=Place;
            HomeAnimationTime=static_cast<float>(Elapsed);
            bHomePose=true;
            return;
        }
    }
    if (Errand.Status==EDeliveryNpcActionStatus::Running &&
        Errand.GroundPickupServerTime>=0 && FMath::IsFinite(Errand.GroundPickupServerTime))
    {
        auto* Pickup=Npc->Companion->GetTuning().GroundPickupAnimation.Get();
        const double Elapsed=Now-Errand.GroundPickupServerTime;
        if (IsValid(Pickup) && Skeleton && Pickup->GetSkeleton()==Skeleton && !Pickup->HasRootMotion() &&
            Elapsed<Pickup->GetPlayLength())
        {
            HomeAnimation=Pickup;
            HomeAnimationTime=static_cast<float>(FMath::Max(0.0,Elapsed));
            bHomePose=true;
            return;
        }
    }
    if (Errand.Status==EDeliveryNpcActionStatus::Running && Errand.Phase==EDeliveryCompanionErrandPhase::SearchNearby &&
        Errand.LowItemInspectionServerTime>=0 && FMath::IsFinite(Errand.LowItemInspectionServerTime) &&
        Npc->GetVelocity().SizeSquared2D()<=FMath::Square(30.f))
    {
        auto* Inspect=Npc->Companion->GetTuning().LowItemInspectionAnimation.Get();
        if (IsValid(Inspect) && Skeleton && Inspect->GetSkeleton()==Skeleton && !Inspect->HasRootMotion())
        {
            const double Length=Inspect->GetPlayLength();
            if (FMath::IsFinite(Length) && Length>0)
            {
                // 复用现有全身姿势混合，复制时间戳对齐观看方的取样
                HomeAnimation=Inspect;
                HomeAnimationTime=static_cast<float>(FMath::Clamp(Now-Errand.LowItemInspectionServerTime,0.0,Length));
                bHomePose=true;
                return;
            }
        }
    }
    // 普通道具和任务包裹共用真实持物槽，有任一实物时保留原持物表现
    const auto* CarriedItem=Npc->Companion->GetCarriedItem();
    if (Action.Action==EDeliveryNpcAction::LookAt && Action.Status==EDeliveryNpcActionStatus::Running &&
        Action.Origin==EDeliveryNpcActionOrigin::Model && Action.ActionId.IsValid() &&
        PoseIndex==static_cast<int32>(EPose::Idle) && !CarriedItem && !State.CarriedProp && !Npc->GetHeldVehicle() &&
        !Npc->Companion->HasActiveErrand() && !Npc->Agent->IsListening() && !Npc->Agent->IsDecisionPending() &&
        Npc->Presentation && !Npc->Presentation->GetDisplayedSpeech().bActive && !Npc->Presentation->IsWaitingForSpeechPlayback())
    {
        // 只给服务器已批准的实见动作选姿势，动作权限和物品校验仍由原执行流程负责
        const auto* LookItem=Cast<ADeliveryHandheldItem>(Action.Target.Get());
        const auto* LookMesh=IsValid(LookItem)?LookItem->GetItemMesh():nullptr;
        const auto* Movement=Npc->GetCharacterMovement();
        const double Elapsed=Now-Action.ServerStartTime;
        if (IsValid(LookMesh) && DeliveryHandheldTransfer::IsOrdinaryItem(LookItem) && LookItem->GetWorld()==Npc->GetWorld() &&
            !LookItem->GetOwner() && !LookItem->GetAttachParentActor() && !LookItem->IsHidden() && LookItem->GetActorEnableCollision() &&
            Movement && Movement->IsMovingOnGround() && Npc->GetVelocity().SizeSquared2D()<=FMath::Square(20.f) &&
            Action.ServerStartTime>=0 && FMath::IsFinite(Elapsed) && FMath::IsFinite(Action.Duration) &&
            Elapsed>=0 && Elapsed<Action.Duration)
        {
            const FVector Offset=LookMesh->Bounds.Origin-Npc->GetNavAgentLocation();
            const FVector Direction=Offset.GetSafeNormal2D();
            if (!Offset.ContainsNaN() && Offset.Z>=-20.f && Offset.Z<=75.f && Offset.SizeSquared2D()<=FMath::Square(180.f) &&
                (Direction.IsNearlyZero() || FVector::DotProduct(Npc->GetActorForwardVector(),Direction)>=0.5))
            {
                static const FGameplayTag LookAtTag=FGameplayTag::RequestGameplayTag(TEXT("Npc.Behavior.Attention.LookAt"),false);
                const auto* Variant=Npc->FindBehaviorPresentation(LookAtTag);
                auto* Inspect=Variant?Variant->Animation.Get():Npc->Companion->GetTuning().LowItemInspectionAnimation.Get();
                const double Rate=Variant?Variant->PlayRate:1.0;
                const double Length=IsValid(Inspect)?Inspect->GetPlayLength():0;
                if (FMath::IsFinite(Length) && Length>0 && Skeleton && Inspect->GetSkeleton()==Skeleton && !Inspect->HasRootMotion())
                {
                    // 按服务器起始时间取循环相位，暂被其他姿势覆盖时也不会重播开头
                    HomeAnimation=Inspect;
                    HomeAnimationTime=static_cast<float>(FMath::Fmod(Elapsed*Rate,Length));
                    bHomePose=true;
                    return;
                }
            }
        }
    }
    if (!DeliveryHandheldTransfer::IsOrdinaryItem(CarriedItem) || CarriedItem->GetOwner()!=Npc || CarriedItem->GetWorld()!=Npc->GetWorld() ||
        !IsValid(Npc->HandheldAnchor) || !IsValid(CarriedItem->GetRootComponent()) ||
        CarriedItem->GetRootComponent()->GetAttachParent()!=Npc->HandheldAnchor ||
        Npc->HandheldAnchor->GetAttachParent()!=Mesh || Npc->HandheldAnchor->GetAttachSocketName()!=CarriedItem->NpcHandSocket ||
        !IsValid(CarriedItem->NpcCarryAnimation) || !Skeleton || CarriedItem->NpcCarryAnimation->GetSkeleton()!=Skeleton) return;
    const double Length=CarriedItem->NpcCarryAnimation->GetPlayLength();
    if (!FMath::IsFinite(Length) || Length<=0 || CarriedItem->NpcCarryAnimation->HasRootMotion()) return;
    HomeAnimation=CarriedItem->NpcCarryAnimation;
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
