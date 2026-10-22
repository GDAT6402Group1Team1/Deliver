// Copyright Epic Games, Inc. All Rights Reserved.

#include "Delivery.h"
#include "DeliveryCharacter.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Task/DeliveryItemComponent.h"
#include "Task/DeliveryPickupPointComponent.h"
#include "Task/DeliveryTargetComponent.h"
#include "Task/DeliveryTaskDefinition.h"
#include "Vehicle/DeliveryMotorbike.h"

#if !UE_BUILD_SHIPPING

/**
 * 取货点与搬运的验收命令。
 *
 * 为什么需要这两条：新流程里"快递在哪"有两个可能的位置（手上 / 车上），
 * 而它们在屏幕上长得差不多——骑车时快递贴在车尾，步行时在手上，
 * 换手那一瞬间要是漏了一步，表现是"快递看着在，但交付按不动"。
 * 把实际持有方打出来，这类问题从"靠眼睛猜"变成一行日志。
 */

namespace DeliveryPickupDebug
{
	ADeliveryCharacter* GetLocalCharacter(UWorld* World)
	{
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		if (!PC)
		{
			return nullptr;
		}

		// 骑车时 Possess 的是车，角色在车的 Driver 上。两条都要试，
		// 否则这两条命令一上车就不可用，而上车正是最需要查的时候
		if (ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(PC->GetPawn()))
		{
			return Character;
		}
		if (const ADeliveryMotorbike* Bike = Cast<ADeliveryMotorbike>(PC->GetPawn()))
		{
			return Bike->GetDriver();
		}

		return nullptr;
	}

	UDeliveryPickupPointComponent* FindNearestPoint(UWorld* World, const FVector& From, float& OutDistance)
	{
		UDeliveryPickupPointComponent* Best = nullptr;
		OutDistance = TNumericLimits<float>::Max();

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<UDeliveryPickupPointComponent*> Points;
			It->GetComponents(Points);
			for (UDeliveryPickupPointComponent* Point : Points)
			{
				const float Distance = FVector::Dist(It->GetActorLocation(), From);
				if (Distance < OutDistance)
				{
					OutDistance = Distance;
					Best = Point;
				}
			}
		}

		return Best;
	}
}

static FAutoConsoleCommandWithWorld GDeliveryPickupDump(
	TEXT("Delivery.Pickup.Dump"),
	TEXT("列出关卡里所有取货点：地点 ID、此刻该发哪个任务、发不出来的原因。"),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* InWorld)
	{
		UWorld* World = InWorld ? InWorld : (GEngine ? GEngine->GetCurrentPlayWorld() : nullptr);
		if (!World)
		{
			UE_LOG(LogDelivery, Error, TEXT("[取货点] 拿不到世界，要在 PIE 里跑"));

			return;
		}

		const ADeliveryCharacter* Character = DeliveryPickupDebug::GetLocalCharacter(World);
		const FVector Origin = Character ? Character->GetActorLocation() : FVector::ZeroVector;

		int32 Count = 0;
		UE_LOG(LogDelivery, Log, TEXT("================ 取货点 ================"));
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<UDeliveryPickupPointComponent*> Points;
			It->GetComponents(Points);
			for (const UDeliveryPickupPointComponent* Point : Points)
			{
				++Count;
				const UDeliveryTaskDefinition* Task = Point->ResolvePendingTask();
				const FText Reason = Point->GetBlockedReason();

				UE_LOG(LogDelivery, Log,
					TEXT("  %-28s 地点=%-16s 待发任务=%-10s 距离=%.0fcm  %s"),
					*It->GetName(),
					*Point->GetLocationId().ToString(),
					Task ? *Task->TaskId.ToString() : TEXT("<无>"),
					FVector::Dist(It->GetActorLocation(), Origin),
					Reason.IsEmpty() ? TEXT("任务侧可领取") : *Reason.ToString());

				/**
				 * 可交互组件的配置也要打。
				 *
				 * **只报任务状态是不够的**：手加组件忘了把 `InteractionKey` 改成
				 * `PickupE`、`HoldDuration` 改成非零时，这一行会显示"可领取"
				 * （任务状态确实没问题），而玩家站在跟前按 E 毫无反应——
				 * 实测就是这么误导过一轮。能不能按和任务状态是两件独立的事，
				 * 必须分开报。
				 */
				const UDeliveryInteractableComponent* Interactable =
					It->FindComponentByClass<UDeliveryInteractableComponent>();
				if (!Interactable)
				{
					UE_LOG(LogDelivery, Error,
						TEXT("      !! 没有 DeliveryInteractableComponent，按不动"));
				}
				else
				{
					const bool bKeyOk =
						Interactable->InteractionKey == EDeliveryInteractionKey::PickupE;
					const bool bHoldOk = Interactable->HoldDuration > KINDA_SMALL_NUMBER;
					UE_LOG(LogDelivery, Log,
						TEXT("      交互：键=%s 长按=%.2fs 半径=%.0fcm 启用=%s  %s"),
						bKeyOk ? TEXT("E") : TEXT("F(错!)"),
						Interactable->HoldDuration,
						Interactable->InteractRadius,
						Interactable->bInteractEnabled ? TEXT("是") : TEXT("否(错!)"),
						(bKeyOk && bHoldOk && Interactable->bInteractEnabled)
							? TEXT("配置可按") : TEXT("<< 按不动，看上面标 (错!) 的那项"));

					/**
					 * 测量点和实际距离。
					 *
					 * **这是"按不动"最常见、也最看不见的原因**：距离不是从
					 * Actor 原点算的，而是锚点 > 根碰撞表面 > 原点。挂在整栋楼上
					 * 又没有简单碰撞体时会退回楼心，于是箭头指门口、判定从楼心算，
					 * 玩家站门口按不动，而这件事在游戏里一点迹象都没有。
					 * 把点打出来，和 Actor 原点一比就知道差在哪
					 */
					if (Character)
					{
						// 不能叫 Point——外层循环里 Point 已经是取货点组件了，遮蔽会当错误报
						const FVector Measured = Interactable->GetInteractionPoint(Origin);
						const float Gap = Interactable->GetInteractionDistance(Character);
						UE_LOG(LogDelivery, Log,
							TEXT("      测量点=(%.0f, %.0f, %.0f) [%s]  Actor原点=(%.0f, %.0f, %.0f)"),
							Measured.X, Measured.Y, Measured.Z,
							Interactable->InteractAnchor ? TEXT("锚点") : TEXT("碰撞表面/原点"),
							It->GetActorLocation().X, It->GetActorLocation().Y, It->GetActorLocation().Z);
						UE_LOG(LogDelivery, Log,
							TEXT("      你到测量点 %.0fcm / 半径 %.0fcm  %s"),
							Gap, Interactable->InteractRadius,
							(Gap >= 0.f && Gap <= Interactable->InteractRadius)
								? TEXT("在范围内")
								: TEXT("<< 超出范围，走近些或调大 Interact Radius"));
					}
				}
			}
		}

		if (Count == 0)
		{
			UE_LOG(LogDelivery, Warning,
				TEXT("[取货点] 关卡里一个取货点都没有。摆一个 ADeliveryPickupPoint 并把 "
					 "LocationId 填成任务的 PickupLocationId；或者在现成的 Actor 上加"
					 "DeliveryLocationComponent + DeliveryInteractableComponent + "
					 "DeliveryPickupPointComponent 三件套"));
		}
		UE_LOG(LogDelivery, Log, TEXT("========================================"));
	}));

static FAutoConsoleCommandWithWorld GDeliveryDropOffDump(
	TEXT("Delivery.DropOff.Dump"),
	TEXT("列出关卡里所有收件点：收哪个任务、交互配得对不对、你离测量点多远。"),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* InWorld)
	{
		UWorld* World = InWorld ? InWorld : (GEngine ? GEngine->GetCurrentPlayWorld() : nullptr);
		if (!World)
		{
			UE_LOG(LogDelivery, Error, TEXT("[收件点] 拿不到世界，要在 PIE 里跑"));

			return;
		}

		ADeliveryCharacter* Character = DeliveryPickupDebug::GetLocalCharacter(World);
		const FVector Origin = Character ? Character->GetActorLocation() : FVector::ZeroVector;
		const ADeliveryPackageItem* Carried = Character ? Character->GetCarriedTaskPackage() : nullptr;

		int32 Count = 0;
		UE_LOG(LogDelivery, Log, TEXT("================ 收件点 ================"));
		UE_LOG(LogDelivery, Log, TEXT("  身上的任务快递：%s"),
			Carried ? *Carried->GetName() : TEXT("<没带>（没带货的话所有收件点都按不动）"));

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<UDeliveryTargetComponent*> Targets;
			It->GetComponents(Targets);
			for (const UDeliveryTargetComponent* Target : Targets)
			{
				++Count;
				const UDeliveryTaskDefinition* Expected = Target->ExpectedTask;
				UE_LOG(LogDelivery, Log, TEXT("  %-28s 收件任务=%-12s 交付半径=%.0fcm"),
					*It->GetName(),
					Expected ? *Expected->TaskId.ToString() : TEXT("<没填!>"),
					Target->DeliveryRadius);

				if (!Expected)
				{
					UE_LOG(LogDelivery, Error,
						TEXT("      !! ExpectedTask 没填，这个点永远不接受任何交付"));
				}

				const UDeliveryInteractableComponent* Interactable =
					It->FindComponentByClass<UDeliveryInteractableComponent>();
				if (!Interactable)
				{
					UE_LOG(LogDelivery, Log,
						TEXT("      交互：没有组件（BeginPlay 会自动建一个，这行出现在建之前才算问题）"));
				}
				else
				{
					const bool bKeyOk = Interactable->InteractionKey == EDeliveryInteractionKey::PickupE;
					UE_LOG(LogDelivery, Log,
						TEXT("      交互：键=%s 长按=%.2fs 提示半径=%.0fcm 启用=%s"),
						bKeyOk ? TEXT("E") : TEXT("F(错!)"),
						Interactable->HoldDuration, Interactable->InteractRadius,
						Interactable->bInteractEnabled ? TEXT("是") : TEXT("否(错!)"));

					// 交付半径和提示半径不一致 = "提示出来了但按不动"，必须点名
					if (!FMath::IsNearlyEqual(Target->DeliveryRadius, Interactable->InteractRadius))
					{
						UE_LOG(LogDelivery, Warning,
							TEXT("      !! 交付半径 %.0f ≠ 提示半径 %.0f，会出现「提示出来了但按不动」"),
							Target->DeliveryRadius, Interactable->InteractRadius);
					}

					const FVector Measured = Interactable->GetInteractionPoint(Origin);
					UE_LOG(LogDelivery, Log,
						TEXT("      测量点=(%.0f, %.0f, %.0f) [%s]  Actor原点=(%.0f, %.0f, %.0f)"),
						Measured.X, Measured.Y, Measured.Z,
						Interactable->InteractAnchor ? TEXT("锚点") : TEXT("碰撞表面/原点"),
						It->GetActorLocation().X, It->GetActorLocation().Y, It->GetActorLocation().Z);

					if (Character)
					{
						const float Gap = Interactable->GetInteractionDistance(Character);
						UE_LOG(LogDelivery, Log,
							TEXT("      你到测量点 %.0fcm / 半径 %.0fcm  %s"),
							Gap, Interactable->InteractRadius,
							(Gap >= 0.f && Gap <= Interactable->InteractRadius)
								? TEXT("在范围内")
								: TEXT("<< 超出范围，走近些或调大 Interact Radius"));
					}
				}

				// 交付那一步判的是**快递**到测量点的距离，不是玩家——骑车时快递在车尾，
				// 和玩家差一个车身。两个数都打出来才解释得清"人到了货没到"
				if (Carried)
				{
					const FVector ItemLocation = Carried->GetActorLocation();
					const float ItemGap = FVector::Dist(ItemLocation, Target->GetDeliveryPoint(ItemLocation));
					UE_LOG(LogDelivery, Log,
						TEXT("      快递到测量点 %.0fcm / 交付半径 %.0fcm  %s"),
						ItemGap, Target->DeliveryRadius,
						ItemGap <= Target->DeliveryRadius ? TEXT("可交付") : TEXT("<< 货还不够近"));
				}
			}
		}

		if (Count == 0)
		{
			UE_LOG(LogDelivery, Warning,
				TEXT("[收件点] 关卡里一个收件点都没有，任务取到货也交不掉。"
					 "摆一个 ADeliveryDropOffPoint，填 LocationId 和 ExpectedTask。"));
		}
		UE_LOG(LogDelivery, Log, TEXT("========================================"));
	}));

static FAutoConsoleCommandWithWorld GDeliveryCarryDump(
	TEXT("Delivery.Carry.Dump"),
	TEXT("打印身上这件任务快递到底在手上还是在车上。"),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* InWorld)
	{
		UWorld* World = InWorld ? InWorld : (GEngine ? GEngine->GetCurrentPlayWorld() : nullptr);
		ADeliveryCharacter* Character = DeliveryPickupDebug::GetLocalCharacter(World);
		if (!Character)
		{
			UE_LOG(LogDelivery, Error, TEXT("[搬运] 拿不到本机角色"));

			return;
		}

		const ADeliveryMotorbike* Bike = Character->GetCurrentVehicle();
		const ADeliveryPackageItem* Carried = Character->GetCarriedTaskPackage();
		const UDeliveryInventoryComponent* Inventory = Character->GetInventoryComponent();

		UE_LOG(LogDelivery, Log, TEXT("================ 搬运状态 ================"));
		UE_LOG(LogDelivery, Log, TEXT("  在骑车      : %s"), Bike ? *Bike->GetName() : TEXT("否（步行）"));
		UE_LOG(LogDelivery, Log, TEXT("  车上的货    : %s"),
			Bike ? (Bike->GetCargo() ? *Bike->GetCargo()->GetName() : TEXT("<空>")) : TEXT("—"));
		UE_LOG(LogDelivery, Log, TEXT("  手上的东西  : %s"),
			(Inventory && Inventory->GetHeldItem()) ? *Inventory->GetHeldItem()->GetName() : TEXT("<空>"));

		if (Carried)
		{
			const UDeliveryItemComponent* Item = Carried->GetDeliveryItemComponent();
			const UDeliveryTaskDefinition* Task = Item ? Item->OwningTask : nullptr;
			UE_LOG(LogDelivery, Log, TEXT("  >> 任务快递 : %s（任务 %s）"),
				*Carried->GetName(), Task ? *Task->TaskId.ToString() : TEXT("<未绑定>"));
		}
		else
		{
			UE_LOG(LogDelivery, Log, TEXT("  >> 任务快递 : 没带"));
		}
		UE_LOG(LogDelivery, Log, TEXT("=========================================="));
	}));

static FAutoConsoleCommandWithWorld GDeliveryPickupTake(
	TEXT("Delivery.Pickup.Take"),
	TEXT("从最近的取货点领一件快递，不用走到跟前按 E。"),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* InWorld)
	{
		UWorld* World = InWorld ? InWorld : (GEngine ? GEngine->GetCurrentPlayWorld() : nullptr);
		ADeliveryCharacter* Character = DeliveryPickupDebug::GetLocalCharacter(World);
		if (!Character)
		{
			UE_LOG(LogDelivery, Error, TEXT("[取货点] 拿不到本机角色"));

			return;
		}

		float Distance = 0.f;
		UDeliveryPickupPointComponent* Point =
			DeliveryPickupDebug::FindNearestPoint(World, Character->GetActorLocation(), Distance);
		if (!Point)
		{
			UE_LOG(LogDelivery, Warning, TEXT("[取货点] 关卡里没有取货点，先跑 Delivery.Pickup.Dump 看看"));

			return;
		}

		// 这条命令**故意跳过距离判定**（调试要的就是不用跑过去），
		// 但不跳任务状态——那是真正要验的东西，跳了就等于没测
		if (Point->TryDispense(Character))
		{
			UE_LOG(LogDelivery, Log, TEXT("[取货点] 领到了（距离 %.0fcm）。跑 Delivery.Carry.Dump 看它在哪"),
				Distance);
		}
		else
		{
			UE_LOG(LogDelivery, Warning, TEXT("[取货点] 没领到：%s"), *Point->GetBlockedReason().ToString());
		}
	}));

#endif // !UE_BUILD_SHIPPING
