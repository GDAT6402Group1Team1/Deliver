#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryVehicleHoldComponent.generated.h"
class ADeliveryNpcCharacter;

// 记录拽住这辆车的 NPC，一辆车同一时间只能被一名 NPC 拽住
// 车辆根据这里提供的倍率减速，松手后恢复正常速度
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryVehicleHoldComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UDeliveryVehicleHoldComponent();
    // 检查车速、距离、驾驶者和站位，通过检查还不会拽住车辆
    bool CanHold(const ADeliveryNpcCharacter* Resident) const;
    // 服务器再次检查后记录拽车者，返回 true 表示已经拽住
    bool TryHold(ADeliveryNpcCharacter* Resident, FGuid ActionId);
    // 松开车辆并恢复 NPC 移动，已经松手时可再次调用
    void Release(FName Reason);
    // 按持续加油门的时间判断是否挣脱
    void UpdateThrottle(float Throttle, float DeltaSeconds);
    UFUNCTION(BlueprintPure) bool IsHeld() const;
    UFUNCTION(BlueprintPure) ADeliveryNpcCharacter* GetHolder() const { return Holder; }
    float GetSpeedScale() const { return IsHeld() ? SpeedScale : 1.f; }
    FVector GetHoldLocation(const ADeliveryNpcCharacter* Resident=nullptr) const;
    UPROPERTY(EditAnywhere, Category="Delivery|拽车", meta=(ClampMin="0.1", ClampMax="1")) float SpeedScale = .4f;
    UPROPERTY(EditAnywhere, Category="Delivery|拽车", meta=(Units="s", ClampMin="0.5", ClampMax="5")) float MaxHoldSeconds = 5.f;
    UPROPERTY(EditAnywhere, Category="Delivery|拽车", meta=(Units="s", ClampMin="0.5", ClampMax="4")) float EscapeThrottleSeconds = 2.f;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
protected:
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void TickComponent(float Dt, ELevelTick Type, FActorComponentTickFunction* Fn) override;
private:
    bool FindHoldLocation(const ADeliveryNpcCharacter* Resident, FVector& Location) const;
    UPROPERTY(ReplicatedUsing=OnRep_Holder) TObjectPtr<ADeliveryNpcCharacter> Holder;
    UFUNCTION() void OnRep_Holder(ADeliveryNpcCharacter* Previous);
    FGuid HoldActionId;
    double ExpiresAt = 0;
    float ThrottleSeconds = 0;
};
