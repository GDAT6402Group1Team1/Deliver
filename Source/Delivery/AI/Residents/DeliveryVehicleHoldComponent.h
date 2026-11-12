#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryVehicleHoldComponent.generated.h"
class ADeliveryResidentCharacter;

// 车辆独占拽车事务，居民申请占用，驾驶更新速度时读取复制后的限速倍率
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryVehicleHoldComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UDeliveryVehicleHoldComponent();
    bool CanHold(const ADeliveryResidentCharacter* Resident) const;
    bool TryHold(ADeliveryResidentCharacter* Resident, FGuid ActionId);
    void Release(FName Reason);
    void UpdateThrottle(float Throttle, float DeltaSeconds);
    UFUNCTION(BlueprintPure) bool IsHeld() const;
    UFUNCTION(BlueprintPure) ADeliveryResidentCharacter* GetHolder() const { return Holder; }
    float GetSpeedScale() const { return IsHeld() ? SpeedScale : 1.f; }
    FVector GetHoldLocation(const ADeliveryResidentCharacter* Resident=nullptr) const;
    UPROPERTY(EditAnywhere, Category="Delivery|拽车", meta=(ClampMin="0.1", ClampMax="1")) float SpeedScale = .4f;
    UPROPERTY(EditAnywhere, Category="Delivery|拽车", meta=(Units="s", ClampMin="0.5", ClampMax="5")) float MaxHoldSeconds = 5.f;
    UPROPERTY(EditAnywhere, Category="Delivery|拽车", meta=(Units="s", ClampMin="0.5", ClampMax="4")) float EscapeThrottleSeconds = 2.f;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
protected:
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void TickComponent(float Dt, ELevelTick Type, FActorComponentTickFunction* Fn) override;
private:
    bool FindHoldLocation(const ADeliveryResidentCharacter* Resident, FVector& Location) const;
    UPROPERTY(ReplicatedUsing=OnRep_Holder) TObjectPtr<ADeliveryResidentCharacter> Holder;
    UFUNCTION() void OnRep_Holder(ADeliveryResidentCharacter* Previous);
    FGuid HoldActionId;
    double ExpiresAt = 0;
    float ThrottleSeconds = 0;
};
