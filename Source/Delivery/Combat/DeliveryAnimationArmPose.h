#pragma once

#include "CoreMinimal.h"

class UAnimSequence;

/** Source animation directions in a canonical +X forward, +Y right, +Z up frame.
 * Only rotations reach the physical character: source translations/limb lengths never do. */
struct FDeliveryAnimationArmPose
{
	FVector Upper[2];
	FVector Lower[2];
	float Weight = 0;
};

namespace DeliveryAnimationArmPose
{
	bool Sample(const UAnimSequence* Animation, float Time, FDeliveryAnimationArmPose& Out);
	float BlendWeight(float Time, float Duration, float BlendIn, float BlendOut);
}
