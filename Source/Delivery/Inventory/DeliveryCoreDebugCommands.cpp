// Copyright Epic Games, Inc. All Rights Reserved.

#include "Delivery.h"
#include "DeliveryCharacter.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Task/DeliveryItemComponent.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/DeliveryTaskDefinition.h"
#include "Task/DeliveryTestTarget.h"
#include "TimerManager.h"

#if !UE_BUILD_SHIPPING
namespace
{
int32 DeliverySmokeWaits = 0;

UWorld* FindDeliveryGameWorld()
{
	if (!GEngine) return nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.World() && (Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game))
		{
			return Context.World();
		}
	}
	return nullptr;
}

void RunDeliverySmokeStep()
{
	UWorld* World = FindDeliveryGameWorld();
	ADeliveryCharacter* Character = nullptr;
	ADeliveryTestPackage* Package = nullptr;
	ADeliveryTestTarget* Target = nullptr;
	if (World)
	{
		for (TActorIterator<ADeliveryCharacter> It(World); It; ++It) { Character = *It; break; }
		for (TActorIterator<ADeliveryTestPackage> It(World); It; ++It) { Package = *It; break; }
		for (TActorIterator<ADeliveryTestTarget> It(World); It; ++It) { Target = *It; break; }
	}
	UDeliveryItemComponent* TaskItem = Package ? Package->GetDeliveryItemComponent() : nullptr;
	UDeliveryTaskManagerComponent* Manager = World ? UDeliveryTaskManagerComponent::Get(World) : nullptr;
	if (!World || !Character || !Package || !Target || !TaskItem || !TaskItem->OwningTask || !Manager)
	{
		UE_LOG(LogDelivery, Error, TEXT("Delivery smoke: FAIL setup world=%s character=%s package=%s target=%s task=%s manager=%s"),
			*GetNameSafe(World), *GetNameSafe(Character), *GetNameSafe(Package), *GetNameSafe(Target),
			*GetNameSafe(TaskItem ? TaskItem->OwningTask : nullptr), *GetNameSafe(Manager));
		return;
	}

	if (Manager->GetTaskStatus(TaskItem->OwningTask) == EDeliveryTaskStatus::Locked)
	{
		if (++DeliverySmokeWaits > 20)
		{
			UE_LOG(LogDelivery, Error, TEXT("Delivery smoke: FAIL task did not unlock within 20 seconds"));
			return;
		}
		FTimerHandle Retry;
		World->GetTimerManager().SetTimer(
			Retry, FTimerDelegate::CreateStatic(&RunDeliverySmokeStep), 1.0f, false);
		return;
	}
	if (Manager->GetTaskStatus(TaskItem->OwningTask) != EDeliveryTaskStatus::AwaitingPickup)
	{
		UE_LOG(LogDelivery, Error, TEXT("Delivery smoke: FAIL task must begin AwaitingPickup (status=%d)"),
			static_cast<int32>(Manager->GetTaskStatus(TaskItem->OwningTask)));
		return;
	}

	const FVector Forward = Character->GetControlRotation().Vector().GetSafeNormal();
	Package->SetActorLocation(Character->GetActorLocation() + Forward * 75.0f + FVector::UpVector * 45.0f,
		false, nullptr, ETeleportType::TeleportPhysics);
	Character->ServerBeginPickupHold(Package);

	TWeakObjectPtr<ADeliveryCharacter> WeakCharacter = Character;
	TWeakObjectPtr<ADeliveryTestPackage> WeakPackage = Package;
	TWeakObjectPtr<ADeliveryTestTarget> WeakTarget = Target;
	TWeakObjectPtr<UDeliveryTaskManagerComponent> WeakManager = Manager;
	FTimerHandle PickupTimer;
	World->GetTimerManager().SetTimer(PickupTimer, FTimerDelegate::CreateLambda(
		[WeakCharacter, WeakPackage, WeakTarget, WeakManager]()
		{
			ADeliveryCharacter* C = WeakCharacter.Get();
			ADeliveryTestPackage* P = WeakPackage.Get();
			ADeliveryTestTarget* T = WeakTarget.Get();
			UDeliveryTaskManagerComponent* M = WeakManager.Get();
			if (!C || !P || !T || !M) return;
			C->ServerCompletePickupHold(P);
			UDeliveryInventoryComponent* Inventory = C->GetInventoryComponent();
			UDeliveryItemComponent* ItemTask = P->GetDeliveryItemComponent();
			const bool bPickupPass = Inventory && Inventory->GetHeldItem() == P && ItemTask
				&& M->GetTaskStatus(ItemTask->OwningTask) == EDeliveryTaskStatus::InProgress;
			if (!bPickupPass)
			{
				UE_LOG(LogDelivery, Error, TEXT("Delivery smoke: FAIL hold pickup/task start"));
				return;
			}

			FVector EyeLocation;
			FRotator EyeRotation;
			C->GetActorEyesViewPoint(EyeLocation, EyeRotation);
			const FVector DeliveryTestLocation = EyeLocation + EyeRotation.Vector() * 75.0f;
			// The test uses the exact same distance, sight and aim checks as gameplay. Teleport
			// both held package and target onto the server view ray so ragdoll settling cannot
			// make this deterministic smoke test depend on the current hand pose.
			P->SetActorLocation(DeliveryTestLocation);
			T->SetActorLocation(DeliveryTestLocation);
			C->ServerBeginPickupHold(T);
			TWeakObjectPtr<UDeliveryTaskDefinition> WeakTask = ItemTask->OwningTask;
			FTimerHandle DeliverTimer;
			C->GetWorldTimerManager().SetTimer(DeliverTimer, FTimerDelegate::CreateLambda(
				[WeakCharacter, WeakPackage, WeakManager, WeakTask]()
				{
					ADeliveryCharacter* InnerC = WeakCharacter.Get();
					ADeliveryTestPackage* InnerP = WeakPackage.Get();
					UDeliveryTaskManagerComponent* InnerM = WeakManager.Get();
					UDeliveryTaskDefinition* Task = WeakTask.Get();
					if (!InnerC || !InnerP || !InnerM || !Task) return;
					FVector InnerEyeLocation;
					FRotator InnerEyeRotation;
					InnerC->GetActorEyesViewPoint(InnerEyeLocation, InnerEyeRotation);
					const FVector DeliveryTestLocation = InnerEyeLocation + InnerEyeRotation.Vector() * 75.0f;
					InnerP->SetActorLocation(DeliveryTestLocation);
					for (TActorIterator<ADeliveryTestTarget> It(InnerC->GetWorld()); It; ++It)
					{
						It->SetActorLocation(DeliveryTestLocation);
						InnerC->ServerCompletePickupHold(*It);
						break;
					}
					const bool bPass = InnerM->GetTaskStatus(Task) == EDeliveryTaskStatus::Completed
						&& InnerC->GetInventoryComponent()->GetHeldItem() == nullptr;
					UE_LOG(LogDelivery, Log, TEXT("Delivery smoke: %s (hold-pickup/task-start/hold-deliver/consume)"),
						bPass ? TEXT("PASS") : TEXT("FAIL"));
				}), 0.55f, false);
		}), 0.55f, false);
}

void StartDeliverySmoke()
{
	DeliverySmokeWaits = 0;
	RunDeliverySmokeStep();
}

FAutoConsoleCommand DeliverySmokeCommand(
	TEXT("Delivery.Delivery.Smoke"),
	TEXT("Wait for Task_001 to unlock, then test package hold pickup and hold delivery."),
	FConsoleCommandDelegate::CreateStatic(&StartDeliverySmoke));
}
#endif
