// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryInteractableComponent.generated.h"

class APawn;
class USceneComponent;

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

	/**
	 * 玩家离得多近才能交互。从下面那个"测量点"算起，不一定是 Actor 原点。
	 *
	 * 测量点的优先级见 `GetInteractionPoint()`：锚点 > 根碰撞表面 > Actor 原点。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Interaction", meta=(ClampMin="0.0", Units="cm"))
	float InteractRadius = 300.0f;

	/**
	 * 距离从这个组件的位置算，而不是 Actor 原点 / 碰撞表面。
	 *
	 * **为什么需要它**：可交互物挂在整栋楼上时，Actor 原点可能在楼中心甚至地下，
	 * 而大型网格如果用"Use Complex Collision As Simple"就没有简单碰撞体、
	 * `GetClosestPointOnCollision` 查不到，于是退回原点——结果是**箭头指门口、
	 * 交互判定从楼心算**，玩家站在门口按不动，而把半径调到够用时站在楼背面也能按。
	 *
	 * 取货点/收件点会自动把它指向同一个 Actor 上的 `DeliveryLocationComponent`
	 * （那个组件本来就是为了能摆到门口才做成 SceneComponent 的），
	 * 于是"箭头指哪"和"在哪能按"天然是同一个点。手填也可以，指任何场景组件都行。
	 */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category="Interaction")
	TObjectPtr<USceneComponent> InteractAnchor;

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

	/**
	 * 距离从哪个点算。锚点 > 根组件碰撞表面（只对 E 目标）> Actor 原点。
	 *
	 * 公开出来是为了让诊断命令能打这个点——"按不动"最常见的原因就是
	 * 它和玩家以为的位置不是一回事，而这件事在游戏里完全看不见。
	 */
	UFUNCTION(BlueprintPure, Category="Interaction")
	FVector GetInteractionPoint(const FVector& FromLocation) const;

	/** 玩家到测量点的实际距离。负数表示拿不到（没有 Owner）。诊断用。 */
	UFUNCTION(BlueprintPure, Category="Interaction")
	float GetInteractionDistance(const APawn* Interactor) const;

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
