// 定义抓取许可、可抓取刚体及抓取者登记
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Grab/DeliveryObjectCarryProvider.h"
#include "DeliveryGrabbableComponent.generated.h"

class ADeliveryCharacter;
class UPrimitiveComponent;

/** 为物理道具和晕倒角色配置抓取许可 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryGrabbableComponent : public UActorComponent
{
	GENERATED_BODY()

  public:
	UDeliveryGrabbableComponent();
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// 抓取与搬运配置
	/** 指定可抓取的组件名称，留空时查找正在模拟物理的组件 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Grab")
	FName BodyComponentName;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Grab")
	int32 GrabberCount = 0;
	/** 普通道具由服务器更新搬运位置，晕倒角色继续进行物理模拟 */
	UPROPERTY(ReplicatedUsing = OnRep_KinematicCarry, BlueprintReadOnly, Category = "Grab")
	bool bKinematicCarry = false;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Grab|Carry", meta = (ClampMin = "0.1"))
	float CarryFollowSpeed = 20.0f;
	// NPC 与玩家互斥，人数仍只统计原有玩家抓取者
	UPROPERTY(ReplicatedUsing = OnRep_KinematicCarry, BlueprintReadOnly, Category = "Grab|Carry")
	bool bNpcCarry = false;
	// 实物根位置和托举目标均须在 NPC 当前身体附近
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Grab|Carry", meta = (ClampMin = "20", ClampMax = "300", Units = "cm"))
	float NpcCarryMaxDistance = 200.f;
	// 只限制 NPC 搬运的真实位置更新，不改变玩家搬运插值
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Grab|Carry", meta = (ClampMin = "10", ClampMax = "600", Units = "cm/s"))
	float NpcCarryMaxSpeed = 300.f;
	// NPC 各旋转轴每秒最多改变的角度
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Grab|Carry", meta = (ClampMin = "10", ClampMax = "360", Units = "deg/s"))
	float NpcCarryMaxTurnSpeed = 90.f;

	// 目标检查与抓取者登记
	UPrimitiveComponent* GetGrabBody(FName& OutBone) const;
	bool CanGrab(const ADeliveryCharacter* Requester) const;
	bool RegisterGrabber(ADeliveryCharacter* Requester);
	void UnregisterGrabber(ADeliveryCharacter* Requester);
	void ReleaseAllGrabbers();
	// 返回登记被接收，不表示已经搬到目标；只接受同世界的原生 provider 组件
	bool RegisterCarryProvider(UActorComponent* Provider, const FGuid& CarryId);
	bool UnregisterCarryProvider(UActorComponent* Provider, const FGuid& CarryId,
	    EDeliveryObjectCarryReleaseReason Reason = EDeliveryObjectCarryReleaseReason::Cancelled);
	bool IsCarriedByProvider(const UActorComponent* Provider, const FGuid& CarryId) const;
	UFUNCTION(BlueprintPure, Category = "Grab")
	bool HasAnyCarrier() const { return bNpcCarry || bKinematicCarry || GrabberCount > 0; }

  private:
	// 搬运状态同步
	UFUNCTION()
	void OnRep_KinematicCarry();
	void ApplyCarryMode();
	void RestoreCarryBody(UPrimitiveComponent* Body);
	bool ReadCarryProviderPose(UActorComponent* Provider, const FGuid& CarryId, UPrimitiveComponent* Body,
	    FVector& OutTarget, FVector& OutForward) const;
	bool IsNpcCarryMoveClear(UPrimitiveComponent* Body, const FVector& Location, const FQuat& Rotation) const;
	void ReleaseCarryProvider(EDeliveryObjectCarryReleaseReason Reason);
	UFUNCTION()
	void OnCarryProviderEndPlay(AActor* Actor, EEndPlayReason::Type Reason);
	TWeakObjectPtr<UActorComponent> CarryProvider;
	TWeakObjectPtr<AActor> CarryProviderActor;
	TWeakObjectPtr<UPrimitiveComponent> ProviderCarryBody;
	FGuid ProviderCarryId;
	bool bChangingCarryProvider = false;
	bool bReleasingAllGrabbers = false;
	bool bEndingPlay = false;
	// 抓取者与碰撞恢复记录
	TArray<TWeakObjectPtr<ADeliveryCharacter>> Grabbers;
	TEnumAsByte<ECollisionResponse> SavedPawnResponse = ECR_Block;
	TEnumAsByte<ECollisionResponse> SavedPhysicsBodyResponse = ECR_Block;
	bool bHasSavedPawnResponse = false;
	FVector PreviousCarryLocation = FVector::ZeroVector;
	FVector LastCarryVelocity = FVector::ZeroVector;
};
