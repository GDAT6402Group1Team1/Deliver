#pragma once
#include "CoreMinimal.h"
class ADeliveryHandheldItem;
class APlayerState;

// 只保存本请求实际观察到的物品身份与位置，不能按名字查全图
struct FDeliveryCompanionItemChoice
{
    TWeakObjectPtr<ADeliveryHandheldItem> Item;
    TWeakObjectPtr<APlayerState> Requester;
    FGuid SubjectId;
    FGuid EvidenceId;
    FGuid CandidateId;
    FGuid RequestId;
    FVector ObservedLocation=FVector::ZeroVector;
    double ObservedAt=0;
    bool bReturnItem=false;
};
