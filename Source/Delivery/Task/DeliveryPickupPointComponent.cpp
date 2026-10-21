// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryPickupPointComponent.h"

#include "Delivery.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "DeliveryCharacter.h"
#include "Engine/World.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Task/DeliveryItemComponent.h"
#include "Task/DeliveryLocationComponent.h"
#include "Task/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"

UDeliveryPickupPointComponent::UDeliveryPickupPointComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UDeliveryPickupPointComponent::BeginPlay()
{
	Super::BeginPlay();

	EnsureInteractableConfigured();
}

void UDeliveryPickupPointComponent::EnsureInteractableConfigured()
{
	AActor* Owner = GetOwner();
	UDeliveryInteractableComponent* Interactable =
		Owner ? Owner->FindComponentByClass<UDeliveryInteractableComponent>() : nullptr;

	if (!Interactable)
	{
		/**
		 * 没有可交互组件就**自己建一个**，而不是只报错。
		 *
		 * 这是被实测逼出来的：手加组件时只加了地点 + 取货点 两个，漏了可交互组件，
		 * 于是这个点压根不在交互注册表里，走过去按 E **毫无反应、毫无报错**，
		 * 而 `Delivery.Pickup.Dump` 查的是任务状态、照样报"可领取"——
		 * 三个信号全是绿的，只有玩家按不动。
		 *
		 * 运行时建的组件不会存进关卡（也不该存），但每次 BeginPlay 都会重新建，
		 * 效果和摆进去一样。摆点的人因此只需要加两个组件：地点 + 取货点。
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
			TEXT("[取货点] %s 上没有 DeliveryInteractableComponent，已自动建一个"
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

	// 只纠正还停在默认值的那套。GeneralF + HoldDuration 0 不可能是能用的取货点，
	// 所以改它不会覆盖任何有意的配置
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
		// 和快递箱子、收件点三处保持一致。玩家的肌肉记忆是"按住 E"
		Interactable->HoldDuration = 0.5f;
	}
	if (Interactable->PromptText.IsEmpty())
	{
		Interactable->PromptText = NSLOCTEXT("DeliveryPickup", "HoldTakePackage", "[按住 E] 取快递");
	}

	if (bDefaultKey || bNoHold)
	{
		UE_LOG(LogDelivery, Log,
			TEXT("[取货点] %s 的可交互组件还是默认值，已自动改成 按住 E / %.2f 秒。"),
			*GetNameSafe(Owner), Interactable->HoldDuration);
	}
}

UDeliveryPickupPointComponent* UDeliveryPickupPointComponent::FindOn(const AActor* Actor)
{
	return Actor ? Actor->FindComponentByClass<UDeliveryPickupPointComponent>() : nullptr;
}

FName UDeliveryPickupPointComponent::GetLocationId() const
{
	const AActor* Owner = GetOwner();
	const UDeliveryLocationComponent* Location =
		Owner ? Owner->FindComponentByClass<UDeliveryLocationComponent>() : nullptr;

	return Location ? Location->LocationId : NAME_None;
}

UDeliveryTaskDefinition* UDeliveryPickupPointComponent::ResolvePendingTask() const
{
	const UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);
	const FName MyId = GetLocationId();
	if (!Manager || MyId.IsNone())
	{
		return nullptr;
	}

	TArray<UDeliveryTaskDefinition*> Awaiting;
	Manager->GetTasksByStatus(EDeliveryTaskStatus::AwaitingPickup, Awaiting);

	for (UDeliveryTaskDefinition* Task : Awaiting)
	{
		if (Task && Task->PickupLocationId == MyId)
		{
			return Task;
		}
	}

	return nullptr;
}

bool UDeliveryPickupPointComponent::CanDispense() const
{
	const UDeliveryTaskDefinition* Task = ResolvePendingTask();
	const UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);

	// 还要过 CanAcquireItem：全世界同时只有一个已接单计时的任务，
	// 别的任务的快递要在这里被拦住（和旧流程捡箱子时是同一道门槛）
	return Task && Manager && Manager->CanAcquireItem(Task);
}

FText UDeliveryPickupPointComponent::GetBlockedReason() const
{
	if (GetLocationId().IsNone())
	{
		return NSLOCTEXT("DeliveryPickup", "PointNoId", "取货点没填地点 ID");
	}

	const UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);
	if (!Manager)
	{
		return NSLOCTEXT("DeliveryPickup", "NoManager", "当前关卡未配置任务管理器");
	}

	if (const UDeliveryTaskDefinition* Task = ResolvePendingTask())
	{
		if (Manager->CanAcquireItem(Task))
		{
			return FText::GetEmpty();
		}

		// 先分"没接电话"再分"有别的单"：这两种都会让 CanAcquireItem 返回 false，
		// 但玩家该做的事完全不同（去接电话 vs 先送完手上这单）
		if (!Manager->IsTaskAccepted(Task))
		{
			return NSLOCTEXT("DeliveryPickup", "NotAnswered", "先接电话才能接这一单");
		}

		return NSLOCTEXT("DeliveryPickup", "OtherTask", "请先完成正在进行的快递任务");
	}

	// 没有待取件任务指向这里。分两种情况说，因为玩家的下一步动作完全不同：
	// 手上已经有货 → 该去送；什么都没有 → 该等电话
	if (const UDeliveryTaskDefinition* Active = Manager->GetActiveTask())
	{
		if (Manager->GetTaskStatus(Active) == EDeliveryTaskStatus::InProgress)
		{
			return NSLOCTEXT("DeliveryPickup", "AlreadyCarrying", "快递已在身上，去收件点交付");
		}
	}

	return NSLOCTEXT("DeliveryPickup", "NoPendingHere", "这里暂时没有要取的快递");
}

bool UDeliveryPickupPointComponent::TryDispense(ADeliveryCharacter* Receiver)
{
	AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!Owner || !Owner->HasAuthority() || !IsValid(Receiver) || !World)
	{
		return false;
	}

	UDeliveryTaskDefinition* Task = ResolvePendingTask();
	if (!CanDispense() || !Task)
	{
		return false;
	}

	UClass* SpawnClass = PackageClass ? PackageClass.Get() : ADeliveryTestPackage::StaticClass();

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	// 放进根组件的空间算偏移：取货点可能挂在整栋楼上、原点在地下，
	// 用世界 Z 加偏移会把快递生成在半空或地底
	const FTransform Base = Owner->GetActorTransform();
	const FVector SpawnLocation = Base.TransformPosition(SpawnOffset);

	ADeliveryPackageItem* Package = World->SpawnActor<ADeliveryPackageItem>(
		SpawnClass, SpawnLocation, Base.Rotator(), Params);
	if (!Package)
	{
		UE_LOG(LogDelivery, Warning,
			TEXT("[取货点] %s 生成快递失败（类 %s）"),
			*Owner->GetName(), *GetNameSafe(SpawnClass));

		return false;
	}

	// 先把任务绑上，再交给玩家。顺序不能反：ReceiveTaskPackage 里会调
	// NotifyAcquired 让任务进入进行中，那时候 OwningTask 必须已经是对的
	if (UDeliveryItemComponent* Item = Package->GetDeliveryItemComponent())
	{
		Item->OwningTask = Task;
	}

	if (!Receiver->ReceiveTaskPackage(Package))
	{
		// 没人收得下就别把它留在世界里当野快递：玩家会以为捡得起来，
		// 而它身上的 OwningTask 已经指向当前任务，捡起来会走到另一条路上去
		Package->Destroy();

		return false;
	}

	UE_LOG(LogDelivery, Log, TEXT("[取货点] %s 发出任务 %s 的快递给 %s"),
		*Owner->GetName(), *Task->TaskId.ToString(), *Receiver->GetName());

	return true;
}
