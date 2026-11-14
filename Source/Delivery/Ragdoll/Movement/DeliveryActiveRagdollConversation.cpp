#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Vehicle/DeliveryTrunkPassengerComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"
#include "Engine/World.h"

double UDeliveryActiveRagdollComponent::ConversationFacingServerTime() const
{
    if (!GetWorld() || !GetOwner()) return -1;
    if (GetOwner()->HasAuthority()) return GetWorld()->GetTimeSeconds();
    const auto* State=GetWorld()->GetGameState();
    return State?State->GetServerWorldTimeSeconds():-1;
}

bool UDeliveryActiveRagdollComponent::CanUseConversationFacing(const AController* Requester) const
{
    const auto* Character=Cast<ADeliveryCharacter>(GetOwner());
    if (!IsValid(Requester) || !IsValid(Character) || Requester->GetWorld()!=GetWorld() ||
        Requester->GetPawn()!=Character || Character->GetController()!=Requester ||
        (!Character->HasAuthority() && !Character->IsLocallyControlled()) || Character->IsActorBeingDestroyed() ||
        Character->IsHidden() || Character->IsIncapacitated() || Character->GetCurrentVehicle() ||
        (Character->TrunkPassenger && Character->TrunkPassenger->IsPassenger()) ||
        !bIsActive || bIsLimp || ReplicatedControlMode!=EDeliveryRagdollControlMode::Active || Network.IsPlayingRecovery() ||
        !Movement.MoveInput.IsNearlyZero() || Movement.bJumping || Movement.bPelvisAirborne ||
        Punch.IsActive() || BoxingPose.HasPendingPunchRecovery() || SwingAxe.IsValid() || bHasHitFacing || bHitFeetPlanted ||
        (Character->GetGrabComponent() && Character->GetGrabComponent()->IsGrabbing())) return false;
    return true;
}

void UDeliveryActiveRagdollComponent::PrepareConversationFacing(AController* Requester,uint64 Sequence)
{
    const auto* Character=Cast<ADeliveryCharacter>(GetOwner());
    if (!Sequence || !IsValid(Requester) || !Character || Character->GetController()!=Requester || Requester->GetPawn()!=Character)
        return;
    if (ConversationFacingController.Get()==Requester && Sequence<=ConversationFacingSequence) return;
    ConversationFacingController=Requester;
    ConversationFacingSequence=Sequence;
    ConversationFacingTarget.Reset();
    ConversationFacingDeadline=0;
    bConversationFacingStarted=false;
    bConversationFacingCancelled=false;
    if (!CanUseConversationFacing(Requester)) CancelConversationFacing();
}

bool UDeliveryActiveRagdollComponent::ApplyConversationFacing(AActor* Target,AController* Requester,uint64 Sequence,double ServerDeadline)
{
    const double Now=ConversationFacingServerTime();
    if (Sequence!=ConversationFacingSequence || ConversationFacingController.Get()!=Requester ||
        bConversationFacingCancelled || bConversationFacingStarted || !CanUseConversationFacing(Requester) ||
        !IsValid(Target) || Target==GetOwner() || Target->GetWorld()!=GetWorld() || Target->IsActorBeingDestroyed() ||
        !FMath::IsFinite(Now) || Now<0 || !FMath::IsFinite(ServerDeadline) || ServerDeadline<=Now ||
        ServerDeadline>Now+2.5 || Target->GetActorLocation().ContainsNaN() ||
        FVector::DistSquared(GetOwner()->GetActorLocation(),Target->GetActorLocation())>FMath::Square(600.f)) return false;
    ConversationFacingTarget=Target;
    ConversationFacingDeadline=ServerDeadline;
    bConversationFacingStarted=true;
    return true;
}

bool UDeliveryActiveRagdollComponent::ConfirmConversationFacing(AActor* Target,AController* Requester,uint64 Sequence)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || !GetWorld() || !FMath::IsFinite(ConversationFacingDuration)) return false;
    const double Deadline=GetWorld()->GetTimeSeconds()+FMath::Clamp(ConversationFacingDuration,.1f,2.f);
    if (!ApplyConversationFacing(Target,Requester,Sequence,Deadline)) return false;
    ClientConversationFacing(Target,Requester,Sequence,Deadline);
    return true;
}

void UDeliveryActiveRagdollComponent::ClientConversationFacing_Implementation(
    AActor* Target,AController* Requester,uint64 Sequence,double ServerDeadline)
{
    const auto* Character=Cast<ADeliveryCharacter>(GetOwner());
    if (!Character || !Character->IsLocallyControlled() || Character->GetController()!=Requester ||
        !Requester || Requester->GetPawn()!=Character || Sequence!=ConversationFacingSequence ||
        ConversationFacingController.Get()!=Requester) return;
    if (!Target)
    {
        bConversationFacingCancelled=true;
        ConversationFacingTarget.Reset(); ConversationFacingDeadline=0;
        return;
    }
    // 监听服务器已在确认入口写入；拥有者客户端用同一截止时间驱动自己的电机
    if (!Character->HasAuthority()) ApplyConversationFacing(Target,Requester,Sequence,ServerDeadline);
}

void UDeliveryActiveRagdollComponent::CancelConversationFacing()
{
    if (bConversationFacingCancelled) return;
    bConversationFacingCancelled=true;
    ConversationFacingTarget.Reset(); ConversationFacingDeadline=0;
    const auto* Character=Cast<ADeliveryCharacter>(GetOwner());
    auto* Requester=ConversationFacingController.Get();
    if (!Character || !Requester || Character->GetController()!=Requester || Requester->GetPawn()!=Character) return;
    if (Character->HasAuthority()) ClientConversationFacing(nullptr,Requester,ConversationFacingSequence,0);
    else if (Character->IsLocallyControlled()) ServerCancelConversationFacing(Requester,ConversationFacingSequence);
}

void UDeliveryActiveRagdollComponent::ServerCancelConversationFacing_Implementation(AController* Requester,uint64 Sequence)
{
    const auto* Character=Cast<ADeliveryCharacter>(GetOwner());
    if (!Character || !Character->HasAuthority() || !IsValid(Requester) ||
        Character->GetController()!=Requester || Requester->GetPawn()!=Character || !Sequence) return;
    // 取消可能比 PC 上的对话请求先到达，先记住该序号，后来的 Prepare 不得重置它
    PrepareConversationFacing(Requester,Sequence);
    if (ConversationFacingController.Get()==Requester && ConversationFacingSequence==Sequence) CancelConversationFacing();
}

void UDeliveryActiveRagdollComponent::UpdateConversationFacing()
{
    if (bConversationFacingCancelled) return;
    if (!CanUseConversationFacing(ConversationFacingController.Get())) { CancelConversationFacing(); return; }
    if (!bConversationFacingStarted) return;
    const auto* Target=ConversationFacingTarget.Get();
    const double Now=ConversationFacingServerTime();
    if (!IsValid(Target) || Target->IsActorBeingDestroyed() || Target->GetWorld()!=GetWorld() ||
        Now<0 || !FMath::IsFinite(Now) || Now>=ConversationFacingDeadline || Target->GetActorLocation().ContainsNaN() ||
        FVector::DistSquared(GetOwner()->GetActorLocation(),Target->GetActorLocation())>FMath::Square(600.f))
    { CancelConversationFacing(); return; }
    FHitResult Hit;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(ConversationFacing),false,GetOwner());
    if (GetWorld()->LineTraceSingleByChannel(Hit,GetHeadLocation(),Target->GetActorLocation(),ECC_Visibility,Params) && Hit.GetActor()!=Target)
        CancelConversationFacing();
}

bool UDeliveryActiveRagdollComponent::GetConversationFacingYaw(float& OutYaw) const
{
    const auto* Target=ConversationFacingTarget.Get();
    if (bConversationFacingCancelled || !bConversationFacingStarted || !IsValid(Target)) return false;
    const FVector Direction=(Target->GetActorLocation()-GetPhysicalPelvisLocation()).GetSafeNormal2D();
    if (Direction.IsNearlyZero() || Direction.ContainsNaN()) return false;
    OutYaw=Direction.Rotation().Yaw;
    return true;
}

void UDeliveryActiveRagdollComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    bConversationFacingCancelled=true;
    ConversationFacingTarget.Reset(); ConversationFacingController.Reset(); ConversationFacingDeadline=0;
    Super::EndPlay(Reason);
}
