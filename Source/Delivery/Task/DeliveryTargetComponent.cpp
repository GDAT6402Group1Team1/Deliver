// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryTargetComponent.h"

#include "DeliveryCharacter.h"
#include "Delivery.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Task/DeliveryLocationComponent.h"
#include "GameFramework/Actor.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Task/DeliveryItemComponent.h"
#include "Task/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"

UDeliveryTargetComponent::UDeliveryTargetComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UDeliveryTargetComponent::BeginPlay()
{
	Super::BeginPlay();

	EnsureInteractableConfigured();
}

void UDeliveryTargetComponent::EnsureInteractableConfigured()
{
	AActor* Owner = GetOwner();
	UDeliveryInteractableComponent* Interactable =
		Owner ? Owner->FindComponentByClass<UDeliveryInteractableComponent>() : nullptr;

	if (!Interactable)
	{
		/**
		 * 没有可交互组件就**自己建一个**，而不是只报错。
		 *
		 * 这是被实测逼出来的：手加组件时只加了地点 + 收件点 两个，漏了可交互组件，
		 * 于是这个点压根不在交互注册表里，走过去按 E **毫无反应、毫无报错**，
		 * 而 `Delivery.Pickup.Dump` 查的是任务状态、照样报"可领取"——
		 * 三个信号全是绿的，只有玩家按不动。
		 *
		 * 运行时建的组件不会存进关卡（也不该存），但每次 BeginPlay 都会重新建，
		 * 效果和摆进去一样。摆点的人因此只需要加两个组件：地点 + 收件点。
		 */
		Interactable = NewObject<UDeliveryInteractableComponent>(
			Owner, UDeliveryInteractableComponent::StaticClass(), TEXT("AutoInteractable"));
		if (!Interactable)
		{
			return;
		}
		// RegisterComponent 在世界已经 BeginPlay 之后会顺带调组件的 BeginPlay，
		// 而注册进交互注册表正是在那里做的——少这一句等于建了个不工作的组件
		Interactable->RegisterComponent();

		UE_LOG(LogDelivery, Log,
			TEXT("[收件点] %s 上没有 DeliveryInteractableComponent，已自动建一个"
				 "（按住 E / 半径 %.0fcm）。想调参数就在那个 Actor 上手加一个，"
				 "手加的优先、这里不会再建。"),
			*GetNameSafe(Owner), Interactable->InteractRadius);
	}

	/**
	 * 距离从地点组件算，不是 Actor 原点。
	 *
	 * 这两个点挂在整栋楼上时 Actor 原点可能在楼心甚至地下，而大型网格用
	 * "Use Complex Collision As Simple" 时连简单碰撞体都没有、碰撞表面也查不到，
	 * 于是**箭头指门口、交互判定从楼心算**——玩家站门口按不动，
	 * 而把半径调到够用时站楼背面也能按。接上地点组件之后两者是同一个点，
	 * 把那个 SceneComponent 拖到门口就一起对了。只在没手填时接。
	 */
	if (!Interactable->InteractAnchor)
	{
		if (UDeliveryLocationComponent* Location = Owner->FindComponentByClass<UDeliveryLocationComponent>())
		{
			Interactable->InteractAnchor = Location;
		}
	}

	// 半径**总是**以本组件为准，自动建的和手加的都盖掉。
	// 这样"怎么调半径"只有一个答案，不用先分清那个可交互组件是谁建的
	Interactable->InteractRadius = InteractRadius;

	const bool bDefaultKey = Interactable->InteractionKey == EDeliveryInteractionKey::GeneralF;
	const bool bNoHold = Interactable->HoldDuration <= KINDA_SMALL_NUMBER;

	if (bDefaultKey)
	{
		Interactable->InteractionKey = EDeliveryInteractionKey::PickupE;
	}
	if (bNoHold)
	{
		Interactable->HoldDuration = 0.5f;
	}
	if (Interactable->PromptText.IsEmpty())
	{
		Interactable->PromptText = NSLOCTEXT("DeliveryPickup", "HoldDeliverPackage", "[按住 E] 交付快递");
	}

	// 提示半径和交付半径必须一致，否则"提示出来了但按不动"
	if (!FMath::IsNearlyEqual(DeliveryRadius, Interactable->InteractRadius))
	{
		UE_LOG(LogDelivery, Warning,
			TEXT("[收件点] %s 的交付半径 %.0f 和提示半径 %.0f 不一致，已对齐到 %.0f。"
				 "不一致的表现是「提示出来了但按不动」。"),
			*GetNameSafe(Owner), DeliveryRadius, Interactable->InteractRadius,
			Interactable->InteractRadius);
		DeliveryRadius = Interactable->InteractRadius;
	}

	if (bDefaultKey || bNoHold)
	{
		UE_LOG(LogDelivery, Log,
			TEXT("[收件点] %s 的可交互组件还是默认值，已自动改成 按住 E / %.2f 秒。"),
			*GetNameSafe(Owner), Interactable->HoldDuration);
	}
}

bool UDeliveryTargetComponent::CanAcceptDelivery(AActor* ItemActor, APlayerState* /*Player*/) const
{
	if (!ExpectedTask || !ItemActor || !GetOwner())
	{
		return false;
	}

	// 必须是这个任务的快递，不能拿别的包裹来交差
	const UDeliveryItemComponent* Item = ItemActor->FindComponentByClass<UDeliveryItemComponent>();
	if (!Item || Item->OwningTask != ExpectedTask)
	{
		return false;
	}

	const UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);
	if (!Manager || Manager->GetTaskStatus(ExpectedTask) != EDeliveryTaskStatus::InProgress)
	{
		return false;
	}

	// 判距离用快递本身的位置：它此刻在谁手上、在不在车里都由交互系统决定。
	// 收件点那一侧取 GetDeliveryPoint——和提示/按键判定同一个点，不是 Actor 原点
	const FVector ItemLocation = ItemActor->GetActorLocation();

	return FVector::Dist(ItemLocation, GetDeliveryPoint(ItemLocation)) <= DeliveryRadius;
}

FVector UDeliveryTargetComponent::GetDeliveryPoint(FVector FromLocation) const
{
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return FromLocation;
	}

	// 交给可交互组件算，而不是在这里再实现一遍"锚点 > 碰撞表面 > 原点"。
	// 两处各写一份迟早分叉，而分叉的表现正是这个函数要防的那个 bug
	if (const UDeliveryInteractableComponent* Interactable =
		Owner->FindComponentByClass<UDeliveryInteractableComponent>())
	{
		return Interactable->GetInteractionPoint(FromLocation);
	}

	return Owner->GetActorLocation();
}

bool UDeliveryTargetComponent::TryDeliver(AActor* ItemActor, APlayerState* Player)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !CanAcceptDelivery(ItemActor, Player))
	{
		return false;
	}

	UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);
	if (!Manager || !Manager->TryCompleteDelivery(ExpectedTask, Player))
	{
		return false;
	}

	if (ADeliveryHandheldItem* Handheld = Cast<ADeliveryHandheldItem>(ItemActor))
	{
		// 快递此刻挂在谁名下，就从那边摘干净再销毁。
		// **必须摘**：直接 Destroy 的话持有方还留着一个指向已销毁 Actor 的指针，
		// `HasCargo()` / 背包槽位会继续报"有货"，下一件快递就领不到了
		if (ADeliveryCharacter* Holder = Cast<ADeliveryCharacter>(Handheld->GetOwner()))
		{
			if (UDeliveryInventoryComponent* Inventory = Holder->GetInventoryComponent())
			{
				Inventory->ConsumeHeldItem(Handheld);
			}
		}
		else if (ADeliveryMotorbike* Bike = Cast<ADeliveryMotorbike>(Handheld->GetOwner()))
		{
			Bike->ReleaseCargo();
		}
	}

	ItemActor->Destroy();

	return true;
}
