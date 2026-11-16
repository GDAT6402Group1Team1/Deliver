#pragma once

#include "CoreMinimal.h"
#include "AI/Presentation/Animation/DeliveryNpcBodyAnimInstance.h"
#include "DeliveryCompanionBodyAnimInstance.generated.h"

// 同伴为家居、持物和低处查看选择姿势，混合与上半身蒙版由独立动画蓝图配置
UCLASS(Blueprintable)
class DELIVERY_API UDeliveryCompanionBodyAnimInstance : public UDeliveryNpcBodyAnimInstance
{
    GENERATED_BODY()
public:
    virtual void NativeUpdateAnimation(float DeltaSeconds) override;
    virtual void NativeUninitializeAnimation() override;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="同伴|家居") TObjectPtr<UAnimSequence> HomeAnimation;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="同伴|家居") float HomeAnimationTime=0;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="同伴|家居") bool bHomePose=false;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="同伴|家居") bool bHomeUpperBodyPose=false;
};
