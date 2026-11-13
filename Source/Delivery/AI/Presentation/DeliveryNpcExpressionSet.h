#pragma once
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "DeliveryNpcExpressionSet.generated.h"
class UCurveFloat;

USTRUCT(BlueprintType)
struct FDeliveryNpcExpressionPose
{
    GENERATED_BODY()
    // 形态键名称对应角色网格中的表情，权重 0 为原样、1 为完整表情
    UPROPERTY(EditAnywhere,BlueprintReadOnly) TMap<FName,float> MorphWeights;
};

// 配置眼睛表情、眨眼，以及交谈时头部和身体的姿态
UCLASS(BlueprintType)
class DELIVERY_API UDeliveryNpcExpressionSet : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere,Category="表情") TMap<FName,FDeliveryNpcExpressionPose> Poses;
    UPROPERTY(EditAnywhere,Category="表情",meta=(ClampMin="1",ClampMax="30")) float BlendSpeed=9;
    UPROPERTY(EditAnywhere,Category="眨眼") FName BlinkMorph=TEXT("EyesBlink");
    // 横轴为一次眨眼的进度 0–1，纵轴为闭合程度 0–1
    UPROPERTY(EditAnywhere,Category="眨眼") TObjectPtr<UCurveFloat> BlinkCurve;
    UPROPERTY(EditAnywhere,Category="眨眼",meta=(ClampMin="0.1",ClampMax="1",Units="s")) float BlinkDuration=.28f;
    UPROPERTY(EditAnywhere,Category="眨眼") FVector2D BlinkInterval=FVector2D(2.8,5.5);
    UPROPERTY(EditAnywhere,Category="注视") FName LookLeftMorph=TEXT("EyesLookLeft");
    UPROPERTY(EditAnywhere,Category="注视") FName LookRightMorph=TEXT("EyesLookRight");
    UPROPERTY(EditAnywhere,Category="注视") FName LookUpMorph=TEXT("EyesLookUp");
    UPROPERTY(EditAnywhere,Category="注视") FName LookDownMorph=TEXT("EyesLookDown");
    UPROPERTY(EditAnywhere,Category="注视",meta=(ClampMin="1",ClampMax="60",Units="deg")) float MaxEyeYaw=25;
    UPROPERTY(EditAnywhere,Category="注视",meta=(ClampMin="1",ClampMax="45",Units="deg")) float MaxEyePitch=20;
    UPROPERTY(EditAnywhere,Category="交谈姿态") bool bConversationPose=true;
    // 刚听见玩家时先探身，之后保持注视并转为思考姿态
    UPROPERTY(EditAnywhere,Category="交谈姿态",meta=(ClampMin="0.1",ClampMax="2",Units="s")) float ListeningReactionDuration=.7f;
    UPROPERTY(EditAnywhere,Category="交谈姿态",meta=(ClampMin="0",ClampMax="12",Units="deg")) float ListeningLean=4;
    UPROPERTY(EditAnywhere,Category="交谈姿态",meta=(ClampMin="-12",ClampMax="12",Units="deg")) float ListeningHeadTilt=5;
    UPROPERTY(EditAnywhere,Category="交谈姿态",meta=(ClampMin="-12",ClampMax="12",Units="deg")) float ThinkingHeadTilt=8;
    UPROPERTY(EditAnywhere,Category="交谈姿态",meta=(ClampMin="1",ClampMax="60",Units="deg")) float HeadTurnLimit=35;
    UPROPERTY(EditAnywhere,Category="交谈姿态",meta=(ClampMin="1",ClampMax="20")) float PostureBlendSpeed=8;
    UPROPERTY(EditAnywhere,Category="交谈姿态",meta=(ClampMin="1",ClampMax="20")) float GazeFollowSpeed=10;
};
