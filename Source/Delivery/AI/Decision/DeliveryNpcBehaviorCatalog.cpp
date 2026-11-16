#include "AI/Decision/DeliveryNpcBehaviorCatalog.h"

// ── 行为表 ──
// 策划在 DT_NpcBehaviorCatalog 里配置每个动作给不给模型选、哪些人物不能用；Load 读这张表建索引
// Agent 问模型之前调用 FilterModelOptions，从 AllowedActions 和 Candidates 里删掉这位 NPC 不能用的动作
namespace
{
/// @brief Wait、ContinueCurrent、CancelCompanionTask 三个动作始终留给模型，行为表里怎么配都不会删掉
bool IsEssentialChoice(EDeliveryNpcAction Action)
{
    return Action == EDeliveryNpcAction::Wait || Action == EDeliveryNpcAction::ContinueCurrent ||
           Action == EDeliveryNpcAction::CancelCompanionTask;
}
} // namespace

/// @brief 读取 DT_NpcBehaviorCatalog，建立行为标签到行号、动作到行号两份索引
/// 行结构不对、标签无效、动作为 None 或重复、缺少 DisplayName 或 Category 时整张表作废，返回 false，原来的索引不变
bool FDeliveryNpcBehaviorCatalog::Load(const UDataTable& Table)
{
    if (Table.GetRowStruct() != FDeliveryNpcBehaviorCatalogRow::StaticStruct())
        return false;
    FDeliveryNpcBehaviorCatalog NewCatalog;
    NewCatalog.Definitions.Reserve(Table.GetRowMap().Num());
    for (const auto& Entry : Table.GetRowMap())
    {
        const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(Entry.Key, false);
        const auto* Row = reinterpret_cast<const FDeliveryNpcBehaviorCatalogRow*>(Entry.Value);
        if (!Tag.IsValid() || !Row || Row->Action == EDeliveryNpcAction::None ||
            !StaticEnum<EDeliveryNpcAction>()->IsValidEnumValue(static_cast<int64>(Row->Action)) || Row->DisplayName.IsEmpty() ||
            Row->Category.IsNone() || NewCatalog.IndexByAction.Contains(Row->Action))
            return false;
        const int32 Index = NewCatalog.Definitions.Add(*Row);
        NewCatalog.IndexByTag.Add(Tag, Index);
        NewCatalog.IndexByAction.Add(Row->Action, Index);
    }
    if (NewCatalog.Definitions.IsEmpty())
        return false;
    *this = MoveTemp(NewCatalog);
    return true;
}

/// @brief 按行为标签查一行，查不到返回 nullptr
const FDeliveryNpcBehaviorCatalogRow* FDeliveryNpcBehaviorCatalog::Find(FGameplayTag Tag) const
{
    const int32* Index = IndexByTag.Find(Tag);
    return Index ? &Definitions[*Index] : nullptr;
}

/// @brief 从发给模型的请求里删掉这位 NPC 不能用的动作和候选
/// 删除条件：bOfferToModel 为 false；bPlayerRequestOnly 为 true 但这次不是回应玩家；ExcludedProfiles 里有这位 NPC 的人物配置 Profile
/// 行为表里没有的动作和 IsEssentialChoice 的三个动作不删
void FDeliveryNpcBehaviorCatalog::FilterModelOptions(FDeliveryNpcDecisionRequest& Request, FName Profile) const
{
    if (Definitions.IsEmpty())
        return;
    const auto Excluded = [&](EDeliveryNpcAction Action)
    {
        const int32* Index = IndexByAction.Find(Action);
        if (!Index || IsEssentialChoice(Action))
            return false;
        const auto& Row = Definitions[*Index];
        return !Row.bOfferToModel || (Row.bPlayerRequestOnly && !Request.bPlayerRequest) ||
               Row.ExcludedProfiles.Contains(Profile);
    };

    // RemoveAll 只删不符合的项，其余候选的顺序、目标、证据不变；候选编号之后由调用方统一生成
    Request.AllowedActions.RemoveAll(Excluded);
    Request.Candidates.RemoveAll(
        [&](const auto& Candidate)
        {
            return Excluded(Candidate.Action);
        });
}
