#include "World/DeliveryHomeBehaviorDefinition.h"
#include "Animation/AnimSequence.h"
#include "Sound/SoundBase.h"

bool UDeliveryHomeBehaviorDefinition::IsUsableConfiguration() const
{
    return !SlotId.IsNone() && !UseId.IsNone() && !DisplayName.IsEmpty() && Operation!=EDeliveryHomeOperation::None &&
        !EntranceOffset.ContainsNaN() && !UseOffset.ContainsNaN() && !ExitOffset.ContainsNaN() && !SightOffset.ContainsNaN() &&
        FMath::IsFinite(ApproachRadius) && FMath::IsFinite(UseRadius) && UseRadius>0 && ApproachRadius>=UseRadius &&
        FMath::IsFinite(UseSeconds) && UseSeconds>0 &&
        (Operation!=EDeliveryHomeOperation::Sit || (EnterAnimation && Animation && ExitAnimation)) &&
        (Operation!=EDeliveryHomeOperation::PlayAudio || Sound!=nullptr) &&
        !PropTransferOffset.ContainsNaN() && (!bUsePropTransferPoint || Operation==EDeliveryHomeOperation::PlaceProp) &&
        !MaximumPropHalfExtent.ContainsNaN() && MaximumPropHalfExtent.GetMin()>0 &&
        FMath::IsFinite(PlacementTolerance) && PlacementTolerance>0 &&
        FMath::IsFinite(PlacementAngleTolerance) && PlacementAngleTolerance>0 && PlacementAngleTolerance<=45 &&
        FMath::IsFinite(MaximumSupportGap) && MaximumSupportGap>0 &&
        FMath::IsFinite(MinimumSupportNormalZ) && MinimumSupportNormalZ>0 && MinimumSupportNormalZ<=1;
}
