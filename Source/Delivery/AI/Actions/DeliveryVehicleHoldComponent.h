#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryVehicleHoldComponent.generated.h"
class ADeliveryNpcCharacter;

/**
 * @brief 挂在摩托车上，记录哪位 NPC 正拽着这辆车，一辆车同时只能被一位拽住
 *
 * 拽车：行为树执行 HoldVehicle 时先 CanHold 再 TryHold，拽住后车速乘 GetSpeedScale
 * 挣脱：摩托车每帧把油门交给 UpdateThrottle，持续加油门够久就 Release；拽满 MaxHoldSeconds 也会松手
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryVehicleHoldComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UDeliveryVehicleHoldComponent();

    // ── 拽车 ──
    // CanHold 判断能不能拽，TryHold 真正拽住；之后车速变慢，NPC 站在车后的拽车位置

    /// @brief 能不能拽：车上有人、车速不快、NPC 离得近、车后有地方站；只判断不拽
    bool CanHold(const ADeliveryNpcCharacter* Resident) const;

    /// @brief 服务器再查一次后拽住车
    /// @return 已经拽住时返回 true
    bool TryHold(ADeliveryNpcCharacter* Resident, FGuid ActionId);

    // ── 挣脱 ──
    // 摩托车每帧报油门，持续加油门够 EscapeThrottleSeconds 就松手；松手后 NPC 恢复移动

    /// @brief 松开车，恢复 NPC 移动；已经松手时再调也没事
    void Release(FName Reason);

    /// @brief 摩托车每帧报油门，按持续加油门的时间判断是否挣脱，不看车速
    void UpdateThrottle(float Throttle /* 这一帧的油门，0 到 1 */, float DeltaSeconds /* 这一帧的时长 */);

    /// @brief 是否有人拽着
    UFUNCTION(BlueprintPure) bool IsHeld() const;

    /// @brief 拽车的 NPC
    UFUNCTION(BlueprintPure) ADeliveryNpcCharacter* GetHolder() const { return Holder; }

    /// @brief 摩托车的速度要乘的倍率，没人拽时为 1
    float GetSpeedScale() const { return IsHeld() ? SpeedScale : 1.f; }

    /// @brief NPC 拽车时站的位置，在车尾后面
    FVector GetHoldLocation(const ADeliveryNpcCharacter* Resident=nullptr) const;

    // ── 配置 ──

    /// 被拽时车速剩几成
    UPROPERTY(EditAnywhere, Category="Delivery|拽车", meta=(ClampMin="0.1", ClampMax="1")) float SpeedScale = .4f;

    /// 最多拽几秒
    UPROPERTY(EditAnywhere, Category="Delivery|拽车", meta=(Units="s", ClampMin="0.5", ClampMax="5")) float MaxHoldSeconds = 5.f;

    /// 持续加油门几秒能挣脱
    UPROPERTY(EditAnywhere, Category="Delivery|拽车", meta=(Units="s", ClampMin="0.5", ClampMax="4")) float EscapeThrottleSeconds = 2.f;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
protected:
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void TickComponent(float Dt, ELevelTick Type, FActorComponentTickFunction* Fn) override;
private:
    /// 在车尾后面找一个踩得到地的站位
    bool FindHoldLocation(const ADeliveryNpcCharacter* Resident, FVector& Location) const;

    /// 拽车的 NPC，同步给客户端
    UPROPERTY(ReplicatedUsing=OnRep_Holder) TObjectPtr<ADeliveryNpcCharacter> Holder;

    /// 客户端：拽车的人变了，设置车和 NPC 互相忽略碰撞；服务器和开车的客户端都要设
    UFUNCTION() void OnRep_Holder(ADeliveryNpcCharacter* Previous);

    /// 拽车动作的编号
    FGuid HoldActionId;

    /// 最晚什么时候松手
    double ExpiresAt = 0;

    /// 已经持续加油门多久
    float ThrottleSeconds = 0;
};
