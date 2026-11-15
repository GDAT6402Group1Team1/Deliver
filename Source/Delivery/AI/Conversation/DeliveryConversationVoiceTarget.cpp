#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
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
    auto* PC=Cast<APlayerController>(GetOwner());
    FDeliverySpeechPlayerContext Context;
    if (!IsOwnedVoiceCompanion(Resident,PC,GetWorld()) || !Context.Capture(PC)) return false;
    // 同伴被打倒时主人照样能用语音叫他，例如喊停手
    // 骑车后原角色的 PlayerState 会清空，受击状态仍由同一玩家的 PlayerState 保存
    if (const auto* AbilityOwner=Cast<IAbilitySystemInterface>(PC->PlayerState.Get()))
        if (const auto* ASC=AbilityOwner->GetAbilitySystemComponent();ASC && ASC->HasMatchingGameplayTag(TAG_State_Stunned)) return false;
    // 队友连麦不依赖距离和视线，跑腿途中保留原来的活动与移动
    return true;
}

bool UDeliveryConversationComponent::IsVoiceTurn(const FDeliveryConversationTurnHandle& Handle) const
{
    auto* Turns=GetWorld()?GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>():nullptr;
    // 租约查询可能清理旧轮次并通知外层，先复制记录以免持有失效的映射指针
    const FVoiceTurn Voice=VoiceTurns.FindRef(Handle.Token);
    auto* PC=Cast<APlayerController>(GetOwner());
    if (!PC || !PC->HasAuthority() || !Turns || Handle.WorldEpoch!=Turns->GetWorldEpoch() ||
        !Voice.Handle.IsValid() || !(Voice.Handle==Handle) || !IsOwnedVoiceCompanion(Voice.Npc.Get(),PC,GetWorld())) return false;
    FDeliveryConversationTurnLease Lease;
    return Turns->GetLease(Voice.Npc.Get(),Lease) && Lease.Owner.Get()==PC && Lease.Handle==Handle && Lease.bVoice &&
        Lease.VoiceContext.Check(PC)!=EDeliverySpeechPlayerContextState::Invalid;
}

bool UDeliveryConversationComponent::IsVoiceReplyMessage(FGuid MessageId,const ADeliveryNpcCharacter* ExpectedNpc) const
{
    const FVoiceTurn Voice=VoiceTurns.FindRef(MessageId);
    auto* Turns=GetWorld()?GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>():nullptr;
    return Voice.Handle.IsValid() && (!ExpectedNpc || Voice.Npc.Get()==ExpectedNpc) && IsVoiceTurn(Voice.Handle) && Turns &&
        Turns->GetVoiceContextState(Cast<APlayerController>(GetOwner()),Voice.Handle)==EDeliverySpeechPlayerContextState::Valid;
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
