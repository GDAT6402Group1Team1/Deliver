#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"

// ── 开麦 ──
// 开麦说话只能对自己的同伴二狗，不看距离和视线，二狗跑腿途中也能听到
// ResolveVoiceCompanion 找到这位同伴，CanVoiceTalkTo 判断现在能不能开麦
// IsVoiceTurn 和 IsVoiceReplyMessage 判断某一轮、某条回复是不是开麦产生的，PruneVoiceTurns 删掉失效的开麦记录
namespace
{
/// @brief 判断这个 NPC 是不是这位玩家自己的同伴，玩家和同伴都要在这个世界里且有效
bool IsOwnedVoiceCompanion(const ADeliveryNpcCharacter* Npc, const APlayerController* PC, const UWorld* World)
{
    const auto* Companion = Cast<ADeliveryCompanionCharacter>(Npc);
    return IsValid(PC) && !PC->IsActorBeingDestroyed() && PC->GetWorld() == World && IsValid(PC->PlayerState) &&
           PC->PlayerState->GetWorld() == World && !PC->PlayerState->IsInactive() && IsValid(Companion) &&
           !Companion->IsActorBeingDestroyed() && Companion->GetWorld() == World && Companion->Companion &&
           Companion->Companion->GetOwnerPlayerState() == PC->PlayerState;
}
} // namespace

/// @brief 找到这位玩家自己的同伴；上次找到的还有效就直接用，否则遍历世界里的同伴重新找
ADeliveryNpcCharacter* UDeliveryConversationComponent::ResolveVoiceCompanion() const
{
    const auto* PC = Cast<APlayerController>(GetOwner());
    if (!GetWorld())
        return nullptr;
    if (IsOwnedVoiceCompanion(CachedVoiceCompanion.Get(), PC, GetWorld()))
        return CachedVoiceCompanion.Get();
    CachedVoiceCompanion.Reset();
    if (!IsValid(PC) || !IsValid(PC->PlayerState))
        return nullptr;
    for (TActorIterator<ADeliveryCompanionCharacter> It(GetWorld()); It; ++It)
        if (IsOwnedVoiceCompanion(*It, PC, GetWorld()))
        {
            CachedVoiceCompanion = *It;
            return *It;
        }
    return nullptr;
}

/// @brief 判断玩家现在能不能对这位 NPC 开麦：必须是自家同伴，玩家自己晕倒时不行
bool UDeliveryConversationComponent::CanVoiceTalkTo(const ADeliveryNpcCharacter* Resident) const
{
    auto* PC = Cast<APlayerController>(GetOwner());
    FDeliverySpeechPlayerContext Context;
    if (!IsOwnedVoiceCompanion(Resident, PC, GetWorld()) || !Context.Capture(PC))
        return false;

    // 玩家带 TAG_State_Stunned（晕倒）时不能开麦；同伴自己被打倒不影响，主人照样能开麦叫他停手
    // 骑车时原角色的 PlayerState 是空的，所以从 PC->PlayerState 取 AbilitySystemComponent
    if (const auto* AbilityOwner = Cast<IAbilitySystemInterface>(PC->PlayerState.Get()))
        if (const auto* ASC = AbilityOwner->GetAbilitySystemComponent(); ASC && ASC->HasMatchingGameplayTag(TAG_State_Stunned))
            return false;

    // 不检查距离和视线，也不改动同伴正在做的事和移动
    return true;
}

/// @brief 服务器：判断这一轮是不是这位玩家的开麦轮次，而且现在仍然有效
bool UDeliveryConversationComponent::IsVoiceTurn(const FDeliveryConversationTurnHandle& Handle) const
{
    auto* Turns = GetWorld() ? GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>() : nullptr;

    // 下面的 GetLease 会先调用 PruneInvalidLeases，过期轮次的通知可能让 VoiceTurns 删掉条目，所以这里复制一份 FVoiceTurn，不保存指向 VoiceTurns 里元素的指针
    const FVoiceTurn Voice = VoiceTurns.FindRef(Handle.Token);
    auto* PC = Cast<APlayerController>(GetOwner());
    if (!PC || !PC->HasAuthority() || !Turns || Handle.WorldEpoch != Turns->GetWorldEpoch() || !Voice.Handle.IsValid() ||
        !(Voice.Handle == Handle) || !IsOwnedVoiceCompanion(Voice.Npc.Get(), PC, GetWorld()))
        return false;
    FDeliveryConversationTurnLease Lease;

    // 这位 NPC 当前的轮次仍属于这个 PC、Handle 相同、bVoice 为 true，而且 VoiceContext.Check 不是 Invalid
    return Turns->GetLease(Voice.Npc.Get(), Lease) && Lease.Owner.Get() == PC && Lease.Handle == Handle && Lease.bVoice &&
           Lease.VoiceContext.Check(PC) != EDeliverySpeechPlayerContextState::Invalid;
}

/// @brief 判断这条回复是不是在回答玩家开麦说的话；是的话，离得远的主人也能听到同伴的声音
/// @param ExpectedNpc 要求是这位 NPC 回的，传空表示不限
bool UDeliveryConversationComponent::IsVoiceReplyMessage(FGuid MessageId, const ADeliveryNpcCharacter* ExpectedNpc) const
{
    const FVoiceTurn Voice = VoiceTurns.FindRef(MessageId);
    auto* Turns = GetWorld() ? GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>() : nullptr;
    return Voice.Handle.IsValid() && (!ExpectedNpc || Voice.Npc.Get() == ExpectedNpc) && IsVoiceTurn(Voice.Handle) && Turns &&
           Turns->GetVoiceContextState(Cast<APlayerController>(GetOwner()), Voice.Handle) ==
               EDeliverySpeechPlayerContextState::Valid;
}

/// @brief 删掉已经失效的开麦轮次记录：NPC 没了，或者那一轮已经不归这位玩家
void UDeliveryConversationComponent::PruneVoiceTurns()
{
    auto* Turns = GetWorld() ? GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>() : nullptr;
    TArray<FGuid> Ids;
    VoiceTurns.GenerateKeyArray(Ids);
    for (const FGuid Id : Ids)
    {
        FDeliveryConversationTurnLease Lease;
        const auto Voice = VoiceTurns.FindRef(Id);
        if (!Turns || !Voice.Npc.IsValid() || !Turns->GetLease(Voice.Npc.Get(), Lease) || !(Lease.Handle == Voice.Handle) ||
            Lease.Owner.Get() != GetOwner())
            VoiceTurns.Remove(Id);
    }
}
