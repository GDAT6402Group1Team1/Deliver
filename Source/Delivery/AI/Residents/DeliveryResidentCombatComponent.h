#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryResidentCombatComponent.generated.h"

// 服务器维护一次挥拳的命中时刻和去重，倒地与恢复通过复制状态驱动表现
UCLASS(ClassGroup=(Delivery),meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryResidentCombatComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UDeliveryResidentCombatComponent();
    bool CanStrike(AActor* Target) const;
    bool StartStrike(AActor* Target,FGuid ActionId);
    void CancelStrike();
    void ReceiveImpact(const FVector& Impulse,AActor* Source);
    bool SetDeliberateProne(bool bProne);
    UFUNCTION(BlueprintPure) bool IsDown() const { return bDown; }
    UFUNCTION(BlueprintPure) bool IsRecovering() const { return bRecovering; }
    UFUNCTION(BlueprintPure) bool IsStriking() const { return AttackId.IsValid(); }
    UPROPERTY(EditAnywhere,Category="Delivery|居民战斗",meta=(ClampMin="50",ClampMax="250",Units="cm")) float StrikeRange=160;
    UPROPERTY(EditAnywhere,Category="Delivery|居民战斗",meta=(ClampMin="1",ClampMax="10",Units="s")) float RecoverySeconds=4;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
protected:
    virtual void TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Fn) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    UPROPERTY(ReplicatedUsing=OnRep_Down) bool bDown=false;
    UPROPERTY(Replicated) bool bDeliberate=false;
    UPROPERTY(Replicated) bool bRecovering=false;
    UFUNCTION() void OnRep_Down();
    FGuid AttackId;
    TWeakObjectPtr<AActor> AttackTarget;
    double HitAt=0;
    double RecoverAt=0;
    double NextStrikeAt=0;
};
