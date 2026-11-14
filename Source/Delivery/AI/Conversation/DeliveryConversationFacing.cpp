#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "Character/DeliveryCharacter.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "GameFramework/PlayerController.h"

uint64 UDeliveryConversationComponent::PrepareConversationFacingInput()
{
    auto* PC=Cast<APlayerController>(GetOwner());
    if (bEndingPlay || !PC || !PC->IsLocalController()) return 0;
    if (auto* Previous=Cast<ADeliveryCharacter>(LocalFacingPawn.Get()); Previous && Previous!=PC->GetPawn())
        if (auto* Body=Previous->GetActiveRagdoll()) Body->CancelConversationFacing();
    ++LastLocalFacingSequence;
    if (!LastLocalFacingSequence) ++LastLocalFacingSequence;
    LocalFacingPawn=PC->GetPawn();
    // 骑车仍可交谈，朝向只交给当前真正被控制的步行主角
    if (auto* Character=Cast<ADeliveryCharacter>(PC->GetPawn()))
        if (auto* Body=Character->GetActiveRagdoll()) Body->PrepareConversationFacing(PC,LastLocalFacingSequence);
    return LastLocalFacingSequence;
}

void UDeliveryConversationComponent::RegisterConversationFacingInput(ADeliveryNpcCharacter* Resident,FGuid ClientToken,uint64 Sequence,APawn* PawnAtInput)
{
    auto* PC=Cast<APlayerController>(GetOwner());
    if (bEndingPlay || !PC || !PC->HasAuthority() || !Sequence || Sequence<=LastServerFacingSequence ||
        !IsValid(PawnAtInput) || PawnAtInput!=PC->GetPawn() || PawnAtInput->GetWorld()!=GetWorld() ||
        PawnAtInput->GetController()!=PC) return;
    // 先记住新的朝向输入，即使目标被遮挡或发言被拒绝，旧 final 也不能转回旧目标
    LastServerFacingSequence=Sequence;
    ServerFacingClientToken=ClientToken;
    ServerFacingTarget.Reset();
    if (IsValid(Resident) && Resident->GetWorld()==GetWorld() && !Resident->IsActorBeingDestroyed())
        ServerFacingTarget=Resident;
    if (auto* Previous=Cast<ADeliveryCharacter>(ServerFacingPawn.Get()); Previous && Previous!=PC->GetPawn())
        if (auto* Body=Previous->GetActiveRagdoll()) Body->CancelConversationFacing();
    ServerFacingPawn=PC->GetPawn();
    // Prepare 清掉旧目标并保留新请求的确认机会，同序号已有取消标记仍由身体保留
    if (auto* Character=Cast<ADeliveryCharacter>(PC->GetPawn()))
        if (auto* Body=Character->GetActiveRagdoll()) Body->PrepareConversationFacing(PC,Sequence);
}

void UDeliveryConversationComponent::ConfirmConversationFacingInput(ADeliveryNpcCharacter* Resident,FGuid ClientToken,uint64 Sequence)
{
    auto* PC=Cast<APlayerController>(GetOwner());
    if (bEndingPlay || !PC || !PC->HasAuthority() || !LastServerFacingSequence ||
        !IsValid(Resident) || Resident->GetWorld()!=GetWorld() || Resident->IsActorBeingDestroyed() ||
        (Sequence && Sequence!=LastServerFacingSequence) || ServerFacingTarget.Get()!=Resident ||
        ServerFacingClientToken!=ClientToken || ServerFacingPawn.Get()!=PC->GetPawn() || !CanTalkTo(Resident)) return;
    if (auto* Character=Cast<ADeliveryCharacter>(PC->GetPawn()))
        if (auto* Body=Character->GetActiveRagdoll()) Body->ConfirmConversationFacing(Resident,PC,LastServerFacingSequence);
}

void UDeliveryConversationComponent::CancelConversationFacingInput()
{
    if (auto* Character=Cast<ADeliveryCharacter>(LocalFacingPawn.Get()))
        if (auto* Body=Character->GetActiveRagdoll()) Body->CancelConversationFacing();
    if (auto* Character=Cast<ADeliveryCharacter>(ServerFacingPawn.Get()))
        if (auto* Body=Character->GetActiveRagdoll()) Body->CancelConversationFacing();
    LocalFacingPawn.Reset(); ServerFacingPawn.Reset(); ServerFacingTarget.Reset(); ServerFacingClientToken.Invalidate();
}
