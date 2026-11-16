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

/**
 * @brief 保存本请求中的物品身份与邻近线索，拿取仍需二狗亲眼确认
 */
struct FDeliveryCompanionItemChoice
{
    /// 新操作显式保存用途，旧取回/交还候选仍兼容 bReturnItem
    EDeliveryNpcAction Action{};
    /// @brief Action 有值就用它，否则按 bReturnItem 选 ReturnItem 或 FetchItem
    EDeliveryNpcAction GetAction() const;
    TWeakObjectPtr<ADeliveryHandheldItem> Item;
    TWeakObjectPtr<AActor> SourceCarrier;
    TWeakObjectPtr<ADeliveryPackageItem> UsePackage;
    FName Source=TEXT("World");
    /// @brief 物品来自主人手上或背包
    bool IsOwnerInventorySource() const { return Source==TEXT("OwnerHand") || Source==TEXT("OwnerBackpack"); }
    /// @brief 找物线索或主人物品栏的候选要引用本次请求的证据
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
