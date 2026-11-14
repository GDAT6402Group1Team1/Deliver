#pragma once

#include "CoreMinimal.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Vehicle/DeliveryHornReceiver.h"
#include "DeliveryCompanionMotorbike.generated.h"

class ADeliveryNpcCharacter;
class UAnimSequence;
class UNavigationPath;

// 地图预放的同伴专属车辆，AI 仍控制原人物，车辆只接收服务器导航输入
UCLASS(Blueprintable)
class DELIVERY_API ADeliveryCompanionMotorbike : public ADeliveryMotorbike, public IDeliveryHornReceiver
{
    GENERATED_BODY()
public:
    ADeliveryCompanionMotorbike();
    bool InitializeForCompanion(ADeliveryNpcCharacter* Companion);
    bool TryMount(ADeliveryNpcCharacter* Companion);
    // Goal 是调用者选好的室外停车点，失败不留下旧油门或旧路线
    bool SetDriveDestination(const FVector& Goal,bool bKeepDrivingWhileReplanning=false);
    // 主人骑车时，在短距离畅通路段直接跟车
    bool TrySetDirectFollowDestination(const FVector& Goal);
    void SetFollowBike(ADeliveryMotorbike* Bike,float DesiredDistance);
    void ReportFollowBlocked();
    bool CanSoundFollowHorn() const;
    bool TrySoundFollowHorn();
    virtual void ReceiveHorn(float Multiplier) override;
    virtual void ReceiveHornFrom(float Multiplier,AActor* Vehicle,AActor* SourceDriver) override;
    void StopDriving();
    // 下车位置被占用时返回 false，原人物仍在车上，调用者可以稍后重试
    bool TryDismount();
    UFUNCTION(BlueprintPure,Category="Companion|车辆") bool IsMounted() const;
    UFUNCTION(BlueprintPure,Category="Companion|车辆") ADeliveryNpcCharacter* GetReservedCompanion() const;
    UFUNCTION(BlueprintPure,Category="Companion|车辆") bool HasReachedDestination() const { return bArrived; }
    UFUNCTION(BlueprintPure,Category="Companion|车辆") FName GetDriveFailureReason() const { return DriveFailure; }
    UFUNCTION(BlueprintPure,Category="Companion|车辆") float GetBoardingRadius() const { return BoardingRadius; }
    UFUNCTION(BlueprintPure,Category="Companion|车辆") float GetPreferredTravelDistance() const { return PreferredTravelDistance; }
    // 自动到站与跟随等候共用真实停车容差，单位厘米
    float GetParkingAcceptanceRadius() const { return FMath::Max(20.f,ParkingRadius); }
    UFUNCTION(BlueprintPure,Category="Companion|车辆") FVector GetBoardingLocation() const;
    bool CanParkAt(const FVector& Goal) const;

    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Companion|骑乘") TObjectPtr<USceneComponent> CompanionSeat;
    // 只改变携带表现，包裹 Owner 仍是同伴，车辆不参与任务交接
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Companion|货架") TObjectPtr<USceneComponent> CompanionCargoAnchor;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Companion|骑乘") TObjectPtr<USkeletalMeshComponent> CompanionRiderMesh;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Companion|骑乘") TObjectPtr<UAnimSequence> RideAnimation;
    // 叠加到人物原网格的相对位置，用独立坐姿资产校准臀部和车座
    UPROPERTY(EditAnywhere,Category="Companion|骑乘",meta=(Units="cm")) FVector RideMeshOffset=FVector::ZeroVector;
    UPROPERTY(EditAnywhere,Category="Companion|车辆",meta=(ClampMin="30",ClampMax="300",Units="cm")) float BoardingRadius=120;
    UPROPERTY(EditAnywhere,Category="Companion|车辆",meta=(ClampMin="300",Units="cm")) float PreferredTravelDistance=1200;
    UPROPERTY(EditAnywhere,Category="Companion|驾驶",meta=(ClampMin="100",ClampMax="1200",Units="cm/s")) float TravelSpeed=700;
    // 只有远离主人时才使用的追赶余量
    UPROPERTY(EditAnywhere,Category="Companion|驾驶",meta=(ClampMin="0",ClampMax="600",Units="cm/s")) float FollowCatchUpSpeed=350;
    UPROPERTY(EditAnywhere,Category="Companion|驾驶",meta=(ClampMin="50",ClampMax="500",Units="cm")) float LookAheadDistance=250;
    UPROPERTY(EditAnywhere,Category="Companion|驾驶",meta=(ClampMin="20",ClampMax="200",Units="cm")) float ParkingRadius=80;
    UPROPERTY(EditAnywhere,Category="Companion|驾驶",meta=(ClampMin="1",ClampMax="10",Units="s")) float BlockedTimeout=3;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
protected:
    virtual void BeginPlay() override;
    virtual bool AllowsPlayerUse() const override { return false; }
    virtual bool HasAutonomousDriver() const override;
    virtual float GetForwardSpeedLimit() const override;
    virtual AActor* GetAutonomousHornDriver() const override;
    virtual void UpdateAutonomousDriveInput(float DeltaSeconds,float& OutThrottle,float& OutSteer) override;
private:
    UPROPERTY(ReplicatedUsing=OnRep_CompanionRide) TObjectPtr<ADeliveryNpcCharacter> ReservedCompanion;
    UPROPERTY(ReplicatedUsing=OnRep_CompanionRide) bool bMounted=false;
    UFUNCTION() void OnRep_CompanionRide();
    UPROPERTY(Transient) TObjectPtr<UNavigationPath> DrivePath;
    TWeakObjectPtr<ADeliveryNpcCharacter> PresentedCompanion;
    bool bReservationInitialized=false;
    bool bDriving=false;
    bool bDrivingDirect=false;
    bool bArrived=false;
    bool bDismountPending=false;
    FName DriveFailure;
    TWeakObjectPtr<ADeliveryMotorbike> FollowBike;
    float FollowDistance=0.f;
    FVector DriveGoal=FVector::ZeroVector;
    FVector ProgressLocation=FVector::ZeroVector;
    int32 PathPointIndex=1;
    double LastProgressTime=0;
    double NextDismountAttempt=0;
    double LastFollowBlock=-100.;
    double NextBlockObservation=0.;
    double NextFollowHorn=0.;
    double FollowHornEndsAt=0.;
    bool CanCompanionRide() const;
    bool HasRidePose() const;
    void FailDrive(FName Reason);
    void ApplyCompanionPresentation();
    void RestoreCompanionPresentation();
    void RefreshCompanionPackage(ADeliveryNpcCharacter* Companion);
    bool FindDismountLocation(FVector& OutLocation) const;
    bool FindFooting(const FVector& Near,FVector& OutLocation) const;
    bool IsParkSpaceClear(const FVector& Goal,FVector* OutVehicleCenter=nullptr) const;
    void ReleaseForEndPlay(EEndPlayReason::Type Reason);
};
