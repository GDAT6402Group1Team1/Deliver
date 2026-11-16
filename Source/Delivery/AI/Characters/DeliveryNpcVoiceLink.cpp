#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"

// ── 交谈 ──
// 主人开麦对二狗说话时，交谈组件的 DeliverFinalText 调用这里的 HearCompanionVoice
// 它先确认说话的是二狗的主人、这句话属于开麦轮次，再调用 HearSpeechInternal，和现场听到的话走同一套处理

/// @brief 服务器：主人开麦说的一句话交给二狗；不检查距离和视线
/// @return 说话人不是二狗的主人、CanVoiceTalkTo 或 IsVoiceReplyMessage 不通过时返回 false
bool ADeliveryNpcCharacter::HearCompanionVoice(APlayerController* Speaker, const FString& Text, FGuid TurnId)
{
    if (!HasAuthority() || !IsValid(Speaker) || Speaker->GetWorld() != GetWorld() || !IsValid(Speaker->GetPawn()) ||
        !TurnId.IsValid())
        return false;
    const auto* Companion = FindComponentByClass<UDeliveryCompanionComponent>();
    const auto* Conversation = Speaker->FindComponentByClass<UDeliveryConversationComponent>();

    // 服务器自己再查一遍：这位 NPC 的 GetOwnerPlayerState 必须是说话人的 PlayerState，TurnId 必须是这位玩家当前有效的开麦轮次
    // 这样客户端不能把远处的任意 NPC 当成自己的同伴，也不能拿旧的发言编号再交一次
    if (!Companion || !IsValid(Speaker->PlayerState) || Companion->GetOwnerPlayerState() != Speaker->PlayerState ||
        !Conversation || !Conversation->CanVoiceTalkTo(this) || !Conversation->IsVoiceReplyMessage(TurnId, this))
        return false;

    // Depth 为 0（玩家说的话），Addressee 是二狗自己，bRequestReply 为 true（要回话），最后的 true 表示这是主人开麦说的
    return HearSpeechInternal(Speaker->GetPawn(), Text, TurnId, 0, this, FGuid(), true, true);
}

/// @brief 引擎问这位 NPC 要不要同步给某个玩家时调用
/// 二狗对它的主人总是返回 true，跑腿走远了主人也能收到它的语音和状态；其他玩家仍按 Super 的距离规则
bool ADeliveryNpcCharacter::IsNetRelevantFor(const AActor* RealViewer, const AActor* ViewTarget, const FVector& SrcLocation) const
{
    // Player 的 PlayerState 是这位二狗的主人时返回 true
    const auto* Player = Cast<APlayerController>(RealViewer);
    const auto* Companion = FindComponentByClass<UDeliveryCompanionComponent>();
    if (IsValid(Player) && Player->GetWorld() == GetWorld() && IsValid(Player->PlayerState) && Companion &&
        Companion->GetOwnerPlayerState() == Player->PlayerState)
        return true;
    return Super::IsNetRelevantFor(RealViewer, ViewTarget, SrcLocation);
}
