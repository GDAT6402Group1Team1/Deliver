#pragma once
#include "CoreMinimal.h"
#include "NativeGameplayTags.h"
#include "DeliveryCombatEvents.generated.h"
class APlayerState;

UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Event_Combat_ConfirmedHit);

// 同步发送给实际玩家的 ASC，身份在受击结算完成时捕获，不把 NPC 的主人当作攻击者
UCLASS(Transient)
class DELIVERY_API UDeliveryCombatHitEvent : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY(Transient) FGuid PunchId;
    UPROPERTY(Transient) TWeakObjectPtr<AActor> Attacker;
    UPROPERTY(Transient) TWeakObjectPtr<AActor> Target;
    UPROPERTY(Transient) TWeakObjectPtr<APlayerState> AttackerPlayerState;
    UPROPERTY(Transient) TWeakObjectPtr<APlayerState> TargetPlayerState;
    UPROPERTY(Transient) FVector ImpactPoint=FVector::ZeroVector;
};

namespace DeliveryCombatEvents
{
    DELIVERY_API APlayerState* FindPlayerState(const AActor* Actor);
    // 只由实际接触结算出口调用，未命中、台词和开始挥拳不发送
    DELIVERY_API void ReportHit(AActor* Attacker,AActor* Target,FGuid PunchId,const FVector& Point);
}
