#include "World/DeliveryHomeBehaviorDefinition.h"
#include "Animation/AnimSequence.h"
#include "Sound/SoundBase.h"

bool UDeliveryHomeBehaviorDefinition::IsUsableConfiguration() const
{
    const auto UsableSeatClip=[](const UAnimSequence* Clip)
    { return IsValid(Clip) && FMath::IsFinite(Clip->GetPlayLength()) && Clip->GetPlayLength()>=.1f && Clip->GetPlayLength()<=30.f; };
    const bool bElevatedSeat=FMath::Abs(UseOffset.GetLocation().Z-EntranceOffset.GetLocation().Z)>30.f ||
        FMath::Abs(UseOffset.GetLocation().Z-ExitOffset.GetLocation().Z)>30.f;
    // 高差明显时必须有真正的上下座位动画，入口、坐稳与出口各自保留作者配置的朝向
    const bool bSeatReady=Operation!=EDeliveryHomeOperation::Sit ||
        (UsableSeatClip(EnterAnimation) && UsableSeatClip(Animation) && UsableSeatClip(ExitAnimation) &&
         (!SeatMountAnimation || UsableSeatClip(SeatMountAnimation)) && (!SeatDismountAnimation || UsableSeatClip(SeatDismountAnimation)) &&
         (!bElevatedSeat || (UsableSeatClip(SeatMountAnimation) && UsableSeatClip(SeatDismountAnimation))));
    return !SlotId.IsNone() && !UseId.IsNone() && !DisplayName.IsEmpty() && Operation!=EDeliveryHomeOperation::None &&
        !EntranceOffset.ContainsNaN() && !UseOffset.ContainsNaN() && !ExitOffset.ContainsNaN() && !SightOffset.ContainsNaN() &&
        FMath::IsFinite(ApproachRadius) && FMath::IsFinite(UseRadius) && UseRadius>0 && ApproachRadius>=UseRadius &&
        FMath::IsFinite(UseSeconds) && UseSeconds>0 &&
        bSeatReady &&
        (Operation!=EDeliveryHomeOperation::PlayAudio || Sound!=nullptr) &&
        !PropTransferOffset.ContainsNaN() && (!bUsePropTransferPoint || Operation==EDeliveryHomeOperation::PlaceProp) &&
        !MaximumPropHalfExtent.ContainsNaN() && MaximumPropHalfExtent.GetMin()>0 &&
        FMath::IsFinite(PlacementTolerance) && PlacementTolerance>0 &&
        FMath::IsFinite(PlacementAngleTolerance) && PlacementAngleTolerance>0 && PlacementAngleTolerance<=45 &&
        FMath::IsFinite(MaximumSupportGap) && MaximumSupportGap>0 &&
        FMath::IsFinite(MinimumSupportNormalZ) && MinimumSupportNormalZ>0 && MinimumSupportNormalZ<=1;
}
