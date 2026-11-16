#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "GameplayTagContainer.h"
#include "AI/Decision/DeliveryNpcTypes.h"
#include "DeliveryNpcBehaviorCatalog.generated.h"

/// 行为表的一行：行名是登记过的 Gameplay Tag，写明对应哪个动作、怎么介绍给模型、谁能看到
/// 动作本身仍由行为树和服务器接口执行
USTRUCT(BlueprintType)
struct DELIVERY_API FDeliveryNpcBehaviorCatalogRow : public FTableRowBase
{
    GENERATED_BODY()

    /// 这一行对应游戏里哪个动作
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="行为") EDeliveryNpcAction Action=EDeliveryNpcAction::None;

    /// 给模型看的动作名
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="行为") FText DisplayName;

    /// 分类，给模型看候选时分组用
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="行为") FName Category;

    /// 给模型看的说明：这个动作做什么、什么时候适合用
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="行为",meta=(MultiLine="true")) FString Description;

    /// 是否列给模型；只能隐藏候选，不会让角色多出权限
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="选择") bool bOfferToModel=true;

    /// 只在回答玩家时才列出来，NPC 自己闲着时不会选
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="选择") bool bPlayerRequestOnly=false;

    /// 列在这里的人物看不到这个动作
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="选择") TSet<FName> ExcludedProfiles;
};

/**
 * @brief 共享行为表，每个世界读一次：按标签或动作枚举查一行，并按人物筛掉不给模型看的候选
 *
 * 网关 Initialize 时 Load，Agent 的 BuildDecisionRequest 末尾调 FilterModelOptions
 */
class DELIVERY_API FDeliveryNpcBehaviorCatalog
{
public:
    /// @brief 读入行为表，检查每行的标签和动作都对得上
    /// @return 表里有错时返回 false，这时沿用角色自己的规则
    bool Load(const UDataTable& Table);

    /// @brief 读进来几行
    int32 Num() const { return Definitions.Num(); }

    /// @brief 按标签找一行，找不到返回空
    const FDeliveryNpcBehaviorCatalogRow* Find(FGameplayTag Tag) const;

    /// @brief 从这次请求的候选动作里，删掉这位人物不该看到的
    /// @param Profile 人物表里的行名，对照 ExcludedProfiles
    void FilterModelOptions(FDeliveryNpcDecisionRequest& Request,FName Profile) const;

private:
    /// 所有行
    TArray<FDeliveryNpcBehaviorCatalogRow> Definitions;

    /// 标签 → 行号
    TMap<FGameplayTag,int32> IndexByTag;

    /// 动作 → 行号
    TMap<EDeliveryNpcAction,int32> IndexByAction;
};
