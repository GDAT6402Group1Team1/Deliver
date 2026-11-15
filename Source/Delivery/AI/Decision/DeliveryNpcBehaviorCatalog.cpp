#include "AI/Decision/DeliveryNpcBehaviorCatalog.h"

namespace
{
    bool IsEssentialChoice(EDeliveryNpcAction Action)
    {
        return Action==EDeliveryNpcAction::Wait || Action==EDeliveryNpcAction::ContinueCurrent ||
            Action==EDeliveryNpcAction::CancelCompanionTask;
    }
}

bool FDeliveryNpcBehaviorCatalog::Load(const UDataTable& Table)
{
    if (Table.GetRowStruct()!=FDeliveryNpcBehaviorCatalogRow::StaticStruct()) return false;
    FDeliveryNpcBehaviorCatalog NewCatalog;
    NewCatalog.Definitions.Reserve(Table.GetRowMap().Num());
    for (const auto& Entry:Table.GetRowMap())
    {
        const FGameplayTag Tag=FGameplayTag::RequestGameplayTag(Entry.Key,false);
        const auto* Row=reinterpret_cast<const FDeliveryNpcBehaviorCatalogRow*>(Entry.Value);
        if (!Tag.IsValid() || !Row || Row->Action==EDeliveryNpcAction::None ||
            !StaticEnum<EDeliveryNpcAction>()->IsValidEnumValue(static_cast<int64>(Row->Action)) ||
            Row->DisplayName.IsEmpty() || Row->Category.IsNone() || NewCatalog.IndexByAction.Contains(Row->Action)) return false;
        const int32 Index=NewCatalog.Definitions.Add(*Row);
        NewCatalog.IndexByTag.Add(Tag,Index);
        NewCatalog.IndexByAction.Add(Row->Action,Index);
    }
    if (NewCatalog.Definitions.IsEmpty()) return false;
    *this=MoveTemp(NewCatalog);
    return true;
}

const FDeliveryNpcBehaviorCatalogRow* FDeliveryNpcBehaviorCatalog::Find(FGameplayTag Tag) const
{
    const int32* Index=IndexByTag.Find(Tag);
    return Index?&Definitions[*Index]:nullptr;
}

void FDeliveryNpcBehaviorCatalog::FilterModelOptions(FDeliveryNpcDecisionRequest& Request,FName Profile) const
{
    if (Definitions.IsEmpty()) return;
    const auto Excluded=[&](EDeliveryNpcAction Action)
    {
        const int32* Index=IndexByAction.Find(Action);
        if (!Index || IsEssentialChoice(Action)) return false;
        const auto& Row=Definitions[*Index];
        return !Row.bOfferToModel || (Row.bPlayerRequestOnly && !Request.bPlayerRequest) ||
            Row.ExcludedProfiles.Contains(Profile);
    };
    // 顺序、目标、证据及同一动作的多个方式保留，编号随后由原入口封存
    Request.AllowedActions.RemoveAll(Excluded);
    Request.Candidates.RemoveAll([&](const auto& Candidate) { return Excluded(Candidate.Action); });
}
