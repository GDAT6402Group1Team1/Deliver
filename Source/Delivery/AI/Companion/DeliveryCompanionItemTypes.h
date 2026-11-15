#pragma once
#include "CoreMinimal.h"
class ADeliveryHandheldItem;
class APlayerState;
class AActor;
class ADeliveryPackageItem;
enum class EDeliveryNpcAction : uint8;

namespace DeliveryCompanionItemSearch
{
    inline constexpr float Radius=650.f;
    inline constexpr float LeadLifetime=15.f;
    inline constexpr float MemoryLifetime=30.f;
    inline const FName EvidenceKind=TEXT("WorldItemSearchLead");
}

// 保存本请求中的物品身份与邻近线索，拿取仍需二狗亲眼确认
struct FDeliveryCompanionItemChoice
{
    // 新操作显式保存用途，旧取回/交还候选仍兼容 bReturnItem
    EDeliveryNpcAction Action{};
    EDeliveryNpcAction GetAction() const;
    TWeakObjectPtr<ADeliveryHandheldItem> Item;
    TWeakObjectPtr<AActor> SourceCarrier;
    TWeakObjectPtr<ADeliveryPackageItem> UsePackage;
    FName Source=TEXT("World");
    bool IsOwnerInventorySource() const { return Source==TEXT("OwnerHand") || Source==TEXT("OwnerBackpack"); }
    bool UsesRequestEvidence() const { return bSearchLead || IsOwnerInventorySource(); }
    TWeakObjectPtr<APlayerState> Requester;
    FGuid SubjectId;
    FGuid EvidenceId;
    FGuid CandidateId;
    FGuid RequestId;
    FVector ObservedLocation=FVector::ZeroVector;
    double ObservedAt=0;
    bool bReturnItem=false;
    bool bSearchLead=false;
    bool bInsertIntoHeldPackage=false;
};
