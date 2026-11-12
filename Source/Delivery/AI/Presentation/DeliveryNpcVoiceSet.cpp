#include "AI/Presentation/DeliveryNpcVoiceSet.h"
#include "Sound/SoundBase.h"

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
