// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryInteractableComponent.generated.h"

class APawn;

UENUM(BlueprintType)
enum class EDeliveryInteractionKey : uint8
{
	GeneralF UMETA(DisplayName="F - General Interaction"),
	PickupE UMETA(DisplayName="E - Pickup / Delivery")
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliveryInteractSignature, APawn*, Interactor);

/**
 * 挂在"走近了按一下 F 会发生点什么"的 Actor 上（摩托车，以后的快递箱、门铃……）。
 *
 * 和 Grab 系统是两回事，别混：Grab 是双键按住、用物理约束把东西抓在手上的连续动作；
 * 这里是一次性的按键交互，只负责提示词、能不能按、按了通知谁。具体做什么由宿主 Actor
 * 绑定 OnInteract 自己实现——组件本身不认识摩托车。
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryInteractableComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UDeliveryInteractableComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** 浮窗上显示的字。摩托车默认是"按 F 驾驶"。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Interaction")
	FText PromptText;

	/** 玩家离得多近才开始提示。从宿主 Actor 原点算起。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Interaction", meta=(ClampMin="0.0", Units="cm"))
	float InteractRadius = 300.0f;

	/** 浮窗吊在宿主 Actor 原点往上多高。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Interaction")
	FVector PromptOffset = FVector(0.0f, 0.0f, 180.0f);

	/**
	 * 关掉之后既不提示也不能交互。
	 * 摩托车在有人骑的时候把它关掉——不然别人站旁边还能"上车"。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Interaction")
	bool bInteractEnabled = true;

	/** E and F are deliberately separate gameplay paths. Existing interactables default to F. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Interaction")
	EDeliveryInteractionKey InteractionKey = EDeliveryInteractionKey::GeneralF;

	/** Zero means tap. Delivery pickup/turn-in use 0.5 seconds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Interaction", meta=(ClampMin="0.0", Units="s"))
	float HoldDuration = 0.0f;

	UPROPERTY(BlueprintAssignable, Category="Interaction")
	FDeliveryInteractSignature OnInteract;

	UFUNCTION(BlueprintPure, Category="Interaction")
	bool CanInteract(const APawn* Interactor) const;

	/** 服务器复核通过之后真正执行一次交互。 */
	void Execute(APawn* Interactor);

	UFUNCTION(BlueprintPure, Category="Interaction")
	FVector GetPromptLocation() const;

	/** 离 Seeker 最近、且在各自 InteractRadius 之内的可交互物；没有就返回 null。 */
	static UDeliveryInteractableComponent* FindBest(
		const APawn* Seeker, EDeliveryInteractionKey Key = EDeliveryInteractionKey::GeneralF);

	/** 收集本世界、当前距离内可按 E 的目标；摄像机瞄准与遮挡由玩家探测器决定。 */
	static void GetReachablePickupCandidates(
		const APawn* Seeker, TArray<UDeliveryInteractableComponent*>& OutCandidates);

	/** 取 Actor 身上第一个可交互组件。服务器收到客户端请求后用它复核。 */
	static UDeliveryInteractableComponent* FindOn(const AActor* Actor);

private:

	/**
	 * 用注册表而不是球形 Overlap 查询。
	 *
	 * 可交互物全场就几个，遍历的代价可以忽略；而碰撞查询在这个项目里已经栽过一次
	 * ——DeliveryTrafficCarComponent 的前车探测因为通道配错恒为空，肉眼完全看不出来。
	 * 注册表没有"配错通道"这个失败模式。
	 */
	static TArray<TWeakObjectPtr<UDeliveryInteractableComponent>> Registry;
};
