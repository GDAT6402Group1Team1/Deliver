#pragma once

#include "CoreMinimal.h"
#include "AI/Presentation/Animation/DeliveryNpcBodyAnimInstance.h"
#include "DeliveryCompanionBodyAnimInstance.generated.h"

/**
 * @brief 同伴为家居、持物和低处查看选择姿势，混合与上半身蒙版由独立动画蓝图配置
 */
UCLASS(Blueprintable)
class DELIVERY_API UDeliveryCompanionBodyAnimInstance : public UDeliveryNpcBodyAnimInstance
{
    GENERATED_BODY()
public:
    /// @brief 每帧先清空家居姿势，再从二狗组件读取移动、受击、取物、放件和家居动作状态写给动画蓝图
    virtual void NativeUpdateAnimation(float DeltaSeconds) override;
    /// @brief 动画实例销毁前清掉家居动画引用和姿势开关
    virtual void NativeUninitializeAnimation() override;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="同伴|家居") TObjectPtr<UAnimSequence> HomeAnimation;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="同伴|家居") float HomeAnimationTime=0;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="同伴|家居") bool bHomePose=false;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="同伴|家居") bool bHomeUpperBodyPose=false;
};
