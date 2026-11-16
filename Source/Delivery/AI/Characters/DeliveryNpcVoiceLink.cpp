#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"

// 服务器：只接受主人的连麦，不检查距离，转交 HearSpeechInternal
bool ADeliveryNpcCharacter::HearCompanionVoice(APlayerController* Speaker, const FString& Text, FGuid TurnId)
{
    if (!HasAuthority() || !IsValid(Speaker) || Speaker->GetWorld() != GetWorld() || !IsValid(Speaker->GetPawn()) ||
        !TurnId.IsValid())
        return false;
    const auto* Companion = FindComponentByClass<UDeliveryCompanionComponent>();
    const auto* Conversation = Speaker->FindComponentByClass<UDeliveryConversationComponent>();
    // 客户端不能把任意远处 NPC 当作自己的随从，也不能借旧发言绕过服务器校验
    if (!Companion || !IsValid(Speaker->PlayerState) || Companion->GetOwnerPlayerState() != Speaker->PlayerState ||
        !Conversation || !Conversation->CanVoiceTalkTo(this) || !Conversation->IsVoiceReplyMessage(TurnId, this))
        return false;
    return HearSpeechInternal(Speaker->GetPawn(), Text, TurnId, 0, this, FGuid(), true, true);
}

// 主人始终能收到自家同伴的同步，其他玩家按距离
bool ADeliveryNpcCharacter::IsNetRelevantFor(const AActor* RealViewer, const AActor* ViewTarget, const FVector& SrcLocation) const
{
    // 主人的随从在跑腿时仍同步语音和人物状态，其他玩家继续使用原距离相关性
    const auto* Player = Cast<APlayerController>(RealViewer);
    const auto* Companion = FindComponentByClass<UDeliveryCompanionComponent>();
    if (IsValid(Player) && Player->GetWorld() == GetWorld() && IsValid(Player->PlayerState) && Companion &&
        Companion->GetOwnerPlayerState() == Player->PlayerState)
        return true;
    return Super::IsNetRelevantFor(RealViewer, ViewTarget, SrcLocation);
}
