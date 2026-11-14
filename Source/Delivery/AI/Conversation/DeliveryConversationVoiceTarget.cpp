#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "AbilitySystemComponent.h"
#include "Character/DeliveryCharacter.h"
#include "UI/DeliveryHUD.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"

namespace
{
bool IsOwnedVoiceCompanion(const ADeliveryNpcCharacter* Npc,const APlayerController* PC,const UWorld* World)
{
    const auto* Companion=Cast<ADeliveryCompanionCharacter>(Npc);
    return IsValid(PC) && !PC->IsActorBeingDestroyed() && PC->GetWorld()==World &&
        IsValid(PC->PlayerState) && PC->PlayerState->GetWorld()==World && !PC->PlayerState->IsInactive() &&
        IsValid(Companion) && !Companion->IsActorBeingDestroyed() && Companion->GetWorld()==World &&
        Companion->Companion && Companion->Companion->GetOwnerPlayerState()==PC->PlayerState;
}
}

ADeliveryNpcCharacter* UDeliveryConversationComponent::ResolveVoiceCompanion() const
{
    const auto* PC=Cast<APlayerController>(GetOwner());
    if (!GetWorld()) return nullptr;
    if (IsOwnedVoiceCompanion(CachedVoiceCompanion.Get(),PC,GetWorld())) return CachedVoiceCompanion.Get();
    CachedVoiceCompanion.Reset();
    if (!IsValid(PC) || !IsValid(PC->PlayerState)) return nullptr;
    for (TActorIterator<ADeliveryCompanionCharacter> It(GetWorld());It;++It)
        if (IsOwnedVoiceCompanion(*It,PC,GetWorld()))
        {
            CachedVoiceCompanion=*It;
            return *It;
        }
    return nullptr;
}

bool UDeliveryConversationComponent::CanVoiceTalkTo(const ADeliveryNpcCharacter* Resident) const
{
    const auto* PC=Cast<APlayerController>(GetOwner());
    APawn* Pawn=PC?PC->GetPawn():nullptr;
    if (!IsOwnedVoiceCompanion(Resident,PC,GetWorld()) || !IsValid(Pawn) || Pawn->IsActorBeingDestroyed() ||
        Pawn->GetWorld()!=GetWorld() || Pawn->GetController()!=PC || Resident->IsKnockedDown()) return false;
    if (const auto* Character=ADeliveryHUD::ResolveCharacter(Pawn))
        if (const auto* ASC=Character->GetAbilitySystemComponent();ASC && ASC->HasMatchingGameplayTag(TAG_State_Stunned)) return false;
    // 队友连麦不依赖距离和视线，跑腿途中保留原来的活动与移动
    return true;
}

bool UDeliveryConversationComponent::IsVoiceTurn(const FDeliveryConversationTurnHandle& Handle) const
{
    const auto* Turns=GetWorld()?GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>():nullptr;
    const auto* Voice=Handle.IsValid()?VoiceTurns.Find(Handle.Token):nullptr;
    return GetOwner() && GetOwner()->HasAuthority() && Turns && Handle.WorldEpoch==Turns->GetWorldEpoch() &&
        Voice && Voice->Handle==Handle && IsOwnedVoiceCompanion(Voice->Npc.Get(),Cast<APlayerController>(GetOwner()),GetWorld());
}

bool UDeliveryConversationComponent::IsVoiceReplyMessage(FGuid MessageId,const ADeliveryNpcCharacter* ExpectedNpc) const
{
    const auto* Voice=VoiceTurns.Find(MessageId);
    return Voice && (!ExpectedNpc || Voice->Npc.Get()==ExpectedNpc) && IsVoiceTurn(Voice->Handle);
}

void UDeliveryConversationComponent::PruneVoiceTurns()
{
    auto* Turns=GetWorld()?GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>():nullptr;
    TArray<FGuid> Ids;
    VoiceTurns.GenerateKeyArray(Ids);
    for (const FGuid Id:Ids)
    {
        FDeliveryConversationTurnLease Lease;
        const auto Voice=VoiceTurns.FindRef(Id);
        if (!Turns || !Voice.Npc.IsValid() || !Turns->GetLease(Voice.Npc.Get(),Lease) ||
            !(Lease.Handle==Voice.Handle) || Lease.Owner.Get()!=GetOwner()) VoiceTurns.Remove(Id);
    }
}
