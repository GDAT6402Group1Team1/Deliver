#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "DeliveryNpcConversationAnimInstance.generated.h"

/**
 * @brief 交谈叠加动画 ABP_NpcConversation 的父类：算出头、胸、眼睛该朝哪
 *
 * 每帧 NativeUpdateAnimation 从倾听、视线组件和说话状态找出要看的点，算出转头和挺胸的角度与权重
 * 骨骼怎么动在动画蓝图里做，这里只出数值
 */
UCLASS(Blueprintable)
class DELIVERY_API UDeliveryNpcConversationAnimInstance : public UAnimInstance
{
    GENERATED_BODY()
public:
    virtual void NativeInitializeAnimation() override;
    virtual void NativeUpdateAnimation(float DeltaSeconds) override;
    virtual void NativeUninitializeAnimation() override;

    // ── 视线 ──
    // 蓝图读下面的值转头、挺胸、转眼睛

    /// @brief 眼睛在头已经转过的基础上还要再偏多少，免得头和眼睛重复转同一个角度
    FVector GetHeadRelativeLookDirection(const FVector& WorldPoint) const;

    /// 要看的点，世界坐标
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="交谈姿态") FVector LookTargetWorld=FVector::ZeroVector;

    /// 看过去的程度，0 是不看，平滑变化
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="交谈姿态") float LookWeight=0;

    /// 交谈时挺胸转身的程度
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="交谈姿态") float PostureWeight=0;

    /// 头最多转多少度，剩下的交给眼睛
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="交谈姿态") float HeadTurnLimit=35;

    /// 胸口要转的角度
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="交谈姿态") FRotator ChestRotation=FRotator::ZeroRotator;

    /// 头要转的角度
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="交谈姿态") FRotator HeadRotation=FRotator::ZeroRotator;
private:
    /// 上一帧在听谁说话，换人时重新过渡
    TWeakObjectPtr<AActor> PreviousSpeaker;

    /// 有没有要看的点
    bool bHasLookTarget=false;

    /// 头骨的前方轴，初始化时从骨架量出来
    FVector HeadForwardAxis=FVector::ForwardVector;

    /// 头骨的上方轴
    FVector HeadUpAxis=FVector::UpVector;
};
