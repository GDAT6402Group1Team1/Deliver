#pragma once
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "DeliveryNpcExpressionSet.generated.h"
class UCurveFloat;

/// 一种表情：几个形态键各用多大权重
USTRUCT(BlueprintType)
struct FDeliveryNpcExpressionPose
{
    GENERATED_BODY()

    /// 形态键名和权重，0 是原样，1 是完整表情
    UPROPERTY(EditAnywhere,BlueprintReadOnly) TMap<FName,float> MorphWeights;
};

/**
 * @brief 一位人物的表情配置：各情绪的形态键、眨眼、眼睛转动和交谈时的身体姿势
 *
 * 表现组件的 TickExpression 读表情和眨眼，交谈动画实例读注视和交谈姿态
 */
UCLASS(BlueprintType)
class DELIVERY_API UDeliveryNpcExpressionSet : public UDataAsset
{
    GENERATED_BODY()
public:
    /// 情绪名到表情的对照，情绪名和模型选的 Emotion 一致
    UPROPERTY(EditAnywhere,Category="表情") TMap<FName,FDeliveryNpcExpressionPose> Poses;

    /// 换表情的速度
    UPROPERTY(EditAnywhere,Category="表情",meta=(ClampMin="1",ClampMax="30")) float BlendSpeed=9;

    /// 闭眼的形态键名
    UPROPERTY(EditAnywhere,Category="眨眼") FName BlinkMorph=TEXT("EyesBlink");

    /// 一次眨眼的曲线：横轴是进度 0–1，纵轴是闭眼程度 0–1
    UPROPERTY(EditAnywhere,Category="眨眼") TObjectPtr<UCurveFloat> BlinkCurve;

    /// 眨一次眼多久
    UPROPERTY(EditAnywhere,Category="眨眼",meta=(ClampMin="0.1",ClampMax="1",Units="s")) float BlinkDuration=.28f;

    /// 两次眨眼之间的随机间隔范围，秒
    UPROPERTY(EditAnywhere,Category="眨眼") FVector2D BlinkInterval=FVector2D(2.8,5.5);

    /// 眼睛往左看的形态键名
    UPROPERTY(EditAnywhere,Category="注视") FName LookLeftMorph=TEXT("EyesLookLeft");

    /// 眼睛往右看的形态键名
    UPROPERTY(EditAnywhere,Category="注视") FName LookRightMorph=TEXT("EyesLookRight");

    /// 眼睛往上看的形态键名
    UPROPERTY(EditAnywhere,Category="注视") FName LookUpMorph=TEXT("EyesLookUp");

    /// 眼睛往下看的形态键名
    UPROPERTY(EditAnywhere,Category="注视") FName LookDownMorph=TEXT("EyesLookDown");

    /// 眼睛左右最多转多少度
    UPROPERTY(EditAnywhere,Category="注视",meta=(ClampMin="1",ClampMax="60",Units="deg")) float MaxEyeYaw=25;

    /// 眼睛上下最多转多少度
    UPROPERTY(EditAnywhere,Category="注视",meta=(ClampMin="1",ClampMax="45",Units="deg")) float MaxEyePitch=20;

    /// 交谈时是否转身挺胸
    UPROPERTY(EditAnywhere,Category="交谈姿态") bool bConversationPose=true;

    /// 刚听见玩家时先往前探身多久，之后保持注视、换成思考的样子
    UPROPERTY(EditAnywhere,Category="交谈姿态",meta=(ClampMin="0.1",ClampMax="2",Units="s")) float ListeningReactionDuration=.7f;

    /// 倾听时往前探多少度
    UPROPERTY(EditAnywhere,Category="交谈姿态",meta=(ClampMin="0",ClampMax="12",Units="deg")) float ListeningLean=4;

    /// 倾听时歪头多少度
    UPROPERTY(EditAnywhere,Category="交谈姿态",meta=(ClampMin="-12",ClampMax="12",Units="deg")) float ListeningHeadTilt=5;

    /// 思考时歪头多少度
    UPROPERTY(EditAnywhere,Category="交谈姿态",meta=(ClampMin="-12",ClampMax="12",Units="deg")) float ThinkingHeadTilt=8;

    /// 头最多转多少度
    UPROPERTY(EditAnywhere,Category="交谈姿态",meta=(ClampMin="1",ClampMax="60",Units="deg")) float HeadTurnLimit=35;

    /// 姿势过渡速度
    UPROPERTY(EditAnywhere,Category="交谈姿态",meta=(ClampMin="1",ClampMax="20")) float PostureBlendSpeed=8;

    /// 视线跟随速度
    UPROPERTY(EditAnywhere,Category="交谈姿态",meta=(ClampMin="1",ClampMax="20")) float GazeFollowSpeed=10;
};
