#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryTrunkPassengerComponent.generated.h"

class ADeliveryMotorbike;
class ADeliveryCharacter;
class UCurveFloat;

USTRUCT(BlueprintType)
struct FDeliveryTrunkPoseSettings
{
    GENERATED_BODY()
    // 前/右/上方向，左右肢体会镜像 Y；只旋转骨段，不改骨长
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector Spine = FVector(.85,0,1);
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector Thigh = FVector(1,.2,.25);
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector Shin = FVector(-.15,0,-1);
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector UpperArm = FVector(.5,.25,-1);
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector Forearm = FVector(1,-.1,.35);
};

UENUM(BlueprintType)
enum class EDeliveryPassengerPhase : uint8 { None, Loading, Riding, WaitingToExit, Unloading };

USTRUCT(BlueprintType)
struct FDeliveryPassengerState
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) TObjectPtr<ADeliveryMotorbike> Vehicle;
    UPROPERTY(BlueprintReadOnly) EDeliveryPassengerPhase Phase = EDeliveryPassengerPhase::None;
    UPROPERTY(BlueprintReadOnly) float StartedAt = 0.f;
    UPROPERTY(BlueprintReadOnly) bool bWakeOnExit = false;
};

// 角色上的后箱乘员状态；服务器管理事务与恢复，身体目标交给原布娃娃组件
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryTrunkPassengerComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UDeliveryTrunkPassengerComponent();
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void TickComponent(float Dt, ELevelTick Type, FActorComponentTickFunction* Function) override;

    UFUNCTION(BlueprintPure, Category="Trunk") bool IsPassenger() const { return State.Phase != EDeliveryPassengerPhase::None; }
    UFUNCTION(BlueprintPure, Category="Trunk") bool IsTransitioning() const { return IsPassenger() && State.Phase != EDeliveryPassengerPhase::Riding; }
    UFUNCTION(BlueprintPure, Category="Trunk") ADeliveryMotorbike* GetVehicle() const { return State.Vehicle; }
    UFUNCTION(BlueprintPure, Category="Trunk") EDeliveryPassengerPhase GetPhase() const { return State.Phase; }
    UFUNCTION(BlueprintPure, Category="Trunk") float GetRecoveryProgress() const;
    UFUNCTION(BlueprintPure, Category="Trunk") float GetPhaseProgress() const;
    UFUNCTION(BlueprintPure, Category="Trunk") FText GetStatusText() const;
    // 请求被接收后仍需要动画和安全落点，不能当作已经出箱
    bool BeginRide(ADeliveryMotorbike* Vehicle);
    bool RequestExit(bool bWake);
    void Release(bool bEmergency = false);
    // 在布娃娃物理前更新调用，避免两套目标同时写身体
    void UpdatePhysics(float Dt);
    bool IsDamageProtected() const { return IsPassenger(); }
    FVector GetCarrierVelocity() const { return VehicleVelocity; }

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Trunk|Motion", meta=(ClampMin="0.2", Units="s")) float TransitionDuration = 1.2f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Trunk|Motion") TObjectPtr<UCurveFloat> TransitionCurve;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Trunk|Motion") FDeliveryTrunkPoseSettings FoldedPose;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Trunk|Motion", meta=(ClampMin="0", Units="s")) float SettleTimeout = 1.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Trunk|Motion", meta=(ClampMin="0", Units="cm/s")) float ExitMaxSpeed = 300.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Trunk|Motion", meta=(ClampMin="0.1", Units="s")) float BlockedExitTimeout = 2.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Trunk|Motion", meta=(ClampMin="5", Units="cm")) float ArrivalTolerance = 45.f;
    // 加速度驱动下的力度上限，保持独立配置，不提高步行电机强度
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Trunk|Physics", meta=(ClampMin="0.1", ClampMax="10")) float FollowStrength = 3.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Trunk|Physics", meta=(ClampMin="980")) float MaxLinearAcceleration = 2400.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Trunk|Physics", meta=(ClampMin="1")) float MaxAngularAcceleration = 1000.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Trunk|Physics", meta=(ClampMin="10", Units="cm/s")) float PoseMoveSpeed = 350.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Trunk|Physics", meta=(ClampMin="10")) float PoseDegreesPerSecond = 150.f;

private:
    UPROPERTY(ReplicatedUsing=OnRep_State) FDeliveryPassengerState State;
    UFUNCTION() void OnRep_State();
    float Now() const;
    void SetPhase(EDeliveryPassengerPhase Phase);
    void ApplyCollisionMode();
    void RememberSafeLocation(const FVector& Location);
    bool FindRecordedExit(FVector& Location) const;
    TWeakObjectPtr<ADeliveryMotorbike> CollisionVehicle;
    bool bSavedCollision = false;
    TEnumAsByte<ECollisionResponse> SavedPawnResponse = ECR_Block;
    FTransform StartFrame;
    FTransform ExitFrame;
    float ExitRequestedAt = 0.f;
    TArray<FVector> SafeLocations;
    FVector LastVehicleLocation = FVector::ZeroVector;
    FVector VehicleVelocity = FVector::ZeroVector;
    bool bReleasing = false;
};
