#pragma once
#include "CoreMinimal.h"
#include "NativeGameplayTags.h"
#include "DeliveryCombatEvents.generated.h"
class APlayerState;

UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Event_Combat_ConfirmedHit);

/**
 * @brief 一次确认命中的记录，作为 Event.Combat.ConfirmedHit 事件的 OptionalObject 发给双方玩家的 ASC
 *
 * 只在命中结算完成时由 ReportHit 创建；双方的 PlayerState 在那一刻记下，NPC 出手时 AttackerPlayerState 为空，不算到它主人头上
 */
UCLASS(Transient)
class DELIVERY_API UDeliveryCombatHitEvent : public UObject
{
    GENERATED_BODY()
public:
    /// 这一拳的编号
    UPROPERTY(Transient) FGuid PunchId;
    /// 出拳的角色
    UPROPERTY(Transient) TWeakObjectPtr<AActor> Attacker;
    /// 被打的角色
    UPROPERTY(Transient) TWeakObjectPtr<AActor> Target;
    /// 出拳的玩家；出拳的是 NPC 时为空
    UPROPERTY(Transient) TWeakObjectPtr<APlayerState> AttackerPlayerState;
    /// 被打的玩家；被打的是 NPC 时为空
    UPROPERTY(Transient) TWeakObjectPtr<APlayerState> TargetPlayerState;
    /// 命中点，世界坐标
    UPROPERTY(Transient) FVector ImpactPoint=FVector::ZeroVector;
};

namespace DeliveryCombatEvents
{
    /// @brief Actor 是玩家正在控制的角色时返回他的 PlayerState，其他情况（NPC、已经离开的玩家）返回 nullptr
    DELIVERY_API APlayerState* FindPlayerState(const AActor* Actor);
    /// @brief 服务器：一拳真的打中时调用，给出拳和被打的玩家的 ASC 各发一次 Event.Combat.ConfirmedHit
    /// 只在命中结算的地方调用；没打中、只是说狠话、刚开始挥拳都不调用
    DELIVERY_API void ReportHit(AActor* Attacker,AActor* Target,FGuid PunchId,const FVector& Point);
}
