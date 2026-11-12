#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "DeliveryNpcConversationAnimInstance.generated.h"

// 从 NPC 的交谈状态取得视线和姿态参数，骨骼变化由动画蓝图完成
UCLASS(Blueprintable)
class DELIVERY_API UDeliveryNpcConversationAnimInstance : public UAnimInstance
{
    GENERATED_BODY()
public:
    virtual void NativeInitializeAnimation() override;
    virtual void NativeUpdateAnimation(float DeltaSeconds) override;
    virtual void NativeUninitializeAnimation() override;
    // 按头部当前朝向计算眼睛还需要偏转多少，避免头和眼睛重复转同一角度
    FVector GetHeadRelativeLookDirection(const FVector& WorldPoint) const;

    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="交谈姿态") FVector LookTargetWorld=FVector::ZeroVector;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="交谈姿态") float LookWeight=0;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="交谈姿态") float PostureWeight=0;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="交谈姿态") float HeadTurnLimit=35;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="交谈姿态") FRotator ChestRotation=FRotator::ZeroRotator;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="交谈姿态") FRotator HeadRotation=FRotator::ZeroRotator;
private:
    TWeakObjectPtr<AActor> PreviousSpeaker;
    bool bHasLookTarget=false;
    FVector HeadForwardAxis=FVector::ForwardVector;
    FVector HeadUpAxis=FVector::UpVector;
};
