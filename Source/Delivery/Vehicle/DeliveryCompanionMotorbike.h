#pragma once

#include "CoreMinimal.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Vehicle/DeliveryHornReceiver.h"
#include "DeliveryCompanionMotorbike.generated.h"

class ADeliveryNpcCharacter;
class ADeliveryHome;
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
    // 接受停车点前可检查从预计下车位置到步行目标的路线，重选失败时可保留已提交的车路
    bool SetDriveDestination(const FVector& Goal,bool bKeepDrivingWhileReplanning=false,const FVector* WalkAfterParking=nullptr);
    // 按连续实地支撑检查短路段，真实车迹核对终点高度，临时水平探路可不指定终点高度
    bool TrySetDirectFollowDestination(const FVector& Goal,bool bRequireGoalHeight=true);
    bool HasActiveDriveRoute(bool bRequireWalkExit=false) const;
    void SetFollowBike(ADeliveryMotorbike* Bike,float DesiredDistance);
    void ReportFollowBlocked();
    bool CanSoundFollowHorn() const;
    bool TrySoundFollowHorn();
    virtual void ReceiveHorn(float Multiplier) override;
    virtual void ReceiveHornFrom(float Multiplier,AActor* Vehicle,AActor* SourceDriver) override;
    void StopDriving();
    // 下车位置被占用时返回 false，原人物仍在车上，调用者可以稍后重试
    bool TryDismount();
    // 检查当前安全出口，失败时允许行为树继续调整停车位置
    bool TryDismountIfClear();
    // 步行目标已投影到同伴的导航网格，有完整路线的安全出口才下车
    bool TryDismountForWalk(const FVector& WalkGoal);
    UFUNCTION(BlueprintPure,Category="Companion|车辆") bool IsMounted() const;
    UFUNCTION(BlueprintPure,Category="Companion|车辆") ADeliveryNpcCharacter* GetReservedCompanion() const;
    UFUNCTION(BlueprintPure,Category="Companion|车辆") bool HasReachedDestination() const { return bArrived; }
    UFUNCTION(BlueprintPure,Category="Companion|车辆") FName GetDriveFailureReason() const { return DriveFailure; }
    UFUNCTION(BlueprintPure,Category="Companion|车辆") float GetBoardingRadius() const { return BoardingRadius; }
    UFUNCTION(BlueprintPure,Category="Companion|车辆") float GetPreferredTravelDistance() const { return PreferredTravelDistance; }
    // 自动到站与跟随等候共用真实停车容差，单位厘米
    float GetParkingAcceptanceRadius() const { return FMath::Max(20.f,ParkingRadius); }
    UFUNCTION(BlueprintPure,Category="Companion|车辆") FVector GetBoardingLocation() const;
    FVector GetOppositeBoardingLocation() const;
    // 核对身体中心到座椅的距离、高差和遮挡，供寻路及实际上车共用
    bool CanBoardFromLocation(const FVector& PawnLocation) const;
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
    virtual bool IsNetRelevantFor(const AActor* RealViewer,const AActor* ViewTarget,const FVector& SrcLocation) const override;
protected:
    virtual void BeginPlay() override;
    virtual bool AllowsPlayerUse() const override { return false; }
    virtual bool HasAutonomousDriver() const override;
    virtual float GetForwardSpeedLimit() const override;
    virtual AActor* GetAutonomousHornDriver() const override;
    virtual void UpdateAutonomousDriveInput(float DeltaSeconds,float& OutThrottle,float& OutSteer) override;
private:
    friend class FDeliveryCompanionRulesTest;
    UPROPERTY(ReplicatedUsing=OnRep_CompanionRide) TObjectPtr<ADeliveryNpcCharacter> ReservedCompanion;
    UPROPERTY(ReplicatedUsing=OnRep_CompanionRide) bool bMounted=false;
    UFUNCTION() void OnRep_CompanionRide();
    UPROPERTY(Transient) TObjectPtr<UNavigationPath> DrivePath;
    TWeakObjectPtr<ADeliveryHome> DrivePedestrianHome;
    TWeakObjectPtr<ADeliveryNpcCharacter> PresentedCompanion;
    bool bReservationInitialized=false;
    bool bDriving=false;
    bool bDrivingDirect=false;
    bool bDrivingToWalkExit=false;
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
    bool TryDismountAtExit(const FVector* WalkGoal,bool bCheckExitBeforeStopping=false);
    bool FindDismountLocation(FVector& OutLocation,const FVector* WalkGoal=nullptr) const;
    bool FindDismountLocationAtPose(const FVector& VehicleCenter,const FQuat& VehicleHeading,const FVector& SeatLocation,
        FVector& OutLocation,const FVector* WalkGoal=nullptr) const;
    bool FindFooting(const FVector& Near,FVector& OutLocation) const;
    bool IsParkSpaceClear(const FVector& Goal,const FQuat& Heading,FVector* OutVehicleCenter=nullptr) const;
    bool IsSupportedVehicleSpace(const FVector& Goal,const FQuat& Heading,float MinimumGroundNormalZ,
        FVector* OutVehicleCenter=nullptr,bool bFitTravelClearance=false) const;
    bool HasClearDrivingSegment(const FVector& From,const FVector& To,const FQuat& Heading) const;
    void ReleaseForEndPlay(EEndPlayReason::Type Reason);
    friend class FDeliveryCompanionRulesTest;
};
