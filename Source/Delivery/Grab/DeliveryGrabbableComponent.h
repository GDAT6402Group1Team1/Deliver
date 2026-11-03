// 定义抓取许可、可抓取刚体及抓取者登记
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
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

	// 目标检查与抓取者登记
	UPrimitiveComponent* GetGrabBody(FName& OutBone) const;
	bool CanGrab(const ADeliveryCharacter* Requester) const;
	bool RegisterGrabber(ADeliveryCharacter* Requester);
	void UnregisterGrabber(ADeliveryCharacter* Requester);
	void ReleaseAllGrabbers();

  private:
	// 搬运状态同步
	UFUNCTION()
	void OnRep_KinematicCarry();
	void ApplyCarryMode();
	// 抓取者与碰撞恢复记录
	TArray<TWeakObjectPtr<ADeliveryCharacter>> Grabbers;
	TEnumAsByte<ECollisionResponse> SavedPawnResponse = ECR_Block;
	TEnumAsByte<ECollisionResponse> SavedPhysicsBodyResponse = ECR_Block;
	bool bHasSavedPawnResponse = false;
	FVector PreviousCarryLocation = FVector::ZeroVector;
	FVector LastCarryVelocity = FVector::ZeroVector;
};
