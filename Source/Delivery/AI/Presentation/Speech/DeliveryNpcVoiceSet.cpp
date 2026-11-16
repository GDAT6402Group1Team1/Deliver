#include "AI/Presentation/Speech/DeliveryNpcVoiceSet.h"
#include "Sound/SoundBase.h"

/// @brief 按反应类型返回人物配置里的短反应音，没配置时返回 nullptr
USoundBase* UDeliveryNpcVoiceSet::GetSound(EDeliveryNpcVoiceReaction Reaction) const
{
    switch (Reaction)
    {
    case EDeliveryNpcVoiceReaction::HeardPlayer:return HeardPlayer;
    case EDeliveryNpcVoiceReaction::Question:return Question;
    case EDeliveryNpcVoiceReaction::Displeased:return Displeased;
    case EDeliveryNpcVoiceReaction::Laugh:return Laugh;
    case EDeliveryNpcVoiceReaction::Surprised:return Surprised;
    case EDeliveryNpcVoiceReaction::Hit:return Hit;
    default:return nullptr;
    }
}
