#pragma once

#include "CoreMinimal.h"
#include "SmartObjectDefinition.h"
#include "DeliveryHomeBehaviorDefinition.generated.h"

class UAnimSequence;
class USoundBase;

UENUM(BlueprintType)
enum class EDeliveryHomeOperation : uint8 { None, Sit, PlayAudio, StopAudio, TakeProp, PlaceProp };

// 家具用途保存在 Smart Object 定义中，行为树和 GAS 读取配置执行，不从网格名猜用途
UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced, CollapseCategories)
class DELIVERY_API UDeliveryHomeBehaviorDefinition : public USmartObjectBehaviorDefinition
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|用途") FName SlotId;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|用途") FName UseId;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|用途") FText DisplayName;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|用途") EDeliveryHomeOperation Operation=EDeliveryHomeOperation::None;
    // 这些变换相对于 Smart Object 槽位，位置单位为厘米
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|位置",meta=(MakeEditWidget=true)) FTransform EntranceOffset;
    // Sit 表示角色脚下基准；PlaceProp 表示支撑面中心与物件朝向，root 高度由物件原尺寸计算
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|位置",meta=(MakeEditWidget=true)) FTransform UseOffset;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|位置",meta=(MakeEditWidget=true)) FTransform ExitOffset;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|位置",meta=(Units="cm")) FVector SightOffset=FVector(0,0,40);
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|距离",meta=(ClampMin="30",ClampMax="3000",Units="cm")) float ApproachRadius=1200;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|距离",meta=(ClampMin="20",ClampMax="400",Units="cm")) float UseRadius=180;
    // 坐姿保持到真实退出，进出过渡由 Companion 独立动画蓝图执行
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|表现") TObjectPtr<UAnimSequence> EnterAnimation;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|表现") TObjectPtr<UAnimSequence> Animation;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|表现") TObjectPtr<UAnimSequence> ExitAnimation;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|表现") TObjectPtr<USoundBase> Sound;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|表现",meta=(ClampMin="0.1",ClampMax="30",Units="s")) float UseSeconds=1;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|条件") bool bAllowWithTaskPackage=false;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|摆放") FGameplayTagQuery RequiredPropTags;
    // 仅用于 PlaceProp，道具先到桌沿外的中转点，再接近真实摆放位置
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|摆放") bool bUsePropTransferPoint=false;
    // 相对槽位的世界厘米偏移，不继承家具缩放；再次取走时沿同一点退出
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|摆放",meta=(EditCondition="bUsePropTransferPoint",MakeEditWidget=true,Units="cm")) FVector PropTransferOffset=FVector::ZeroVector;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|摆放",meta=(ClampMin="1",Units="cm")) FVector MaximumPropHalfExtent=FVector(100);
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|摆放",meta=(ClampMin="1",ClampMax="50",Units="cm")) float PlacementTolerance=15;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|摆放",meta=(ClampMin="1",ClampMax="45",Units="deg")) float PlacementAngleTolerance=15;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|摆放",meta=(ClampMin="1",ClampMax="50",Units="cm")) float MaximumSupportGap=12;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|摆放",meta=(ClampMin="0.1",ClampMax="1")) float MinimumSupportNormalZ=.7f;
    // 检查用途配置是否完整，不修改定义资产
    bool IsUsableConfiguration() const;
};
