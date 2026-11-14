#pragma once

#include "CoreMinimal.h"
#include "AI/Presentation/DeliveryNpcBodyAnimInstance.h"
#include "DeliveryCompanionBodyAnimInstance.generated.h"

// 同伴在原居民姿势上增加家居输入，混合与上半身蒙版保存在独立动画蓝图
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
