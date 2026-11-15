#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "GameplayTagContainer.h"
#include "AI/Decision/DeliveryNpcTypes.h"
#include "DeliveryNpcBehaviorCatalog.generated.h"

// 行名是已登记的 Gameplay Tag，每行动作仍由原来的行为树和服务器接口执行
USTRUCT(BlueprintType)
struct DELIVERY_API FDeliveryNpcBehaviorCatalogRow : public FTableRowBase
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="行为") EDeliveryNpcAction Action=EDeliveryNpcAction::None;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="行为") FText DisplayName;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="行为") FName Category;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="行为",meta=(MultiLine="true")) FString Description;
    // 只限制模型可见的现成候选，不授予角色额外权限
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="选择") bool bOfferToModel=true;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="选择") bool bPlayerRequestOnly=false;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="选择") TSet<FName> ExcludedProfiles;
};

// 每个世界加载一次动作配置，安排行名和原执行枚举的索引
class DELIVERY_API FDeliveryNpcBehaviorCatalog
{
public:
    bool Load(const UDataTable& Table);
    int32 Num() const { return Definitions.Num(); }
    const FDeliveryNpcBehaviorCatalogRow* Find(FGameplayTag Tag) const;
    void FilterModelOptions(FDeliveryNpcDecisionRequest& Request,FName Profile) const;

private:
    TArray<FDeliveryNpcBehaviorCatalogRow> Definitions;
    TMap<FGameplayTag,int32> IndexByTag;
    TMap<EDeliveryNpcAction,int32> IndexByAction;
};
