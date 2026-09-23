#include "Delivery.h"
#include "DeliveryCharacter.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryInventoryComponent.h"

#if !UE_BUILD_SHIPPING
namespace
{
UWorld* FindGameWorld()
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

void InventorySmokeTest()
{
	UWorld* World = FindGameWorld();
	ADeliveryCharacter* Character = nullptr;
	TArray<ADeliveryHandheldItem*> Items;
	if (World)
	{
		for (TActorIterator<ADeliveryCharacter> It(World); It; ++It) { Character = *It; break; }
		for (TActorIterator<ADeliveryHandheldItem> It(World); It; ++It) Items.Add(*It);
	}
	Items.Sort([](const ADeliveryHandheldItem& A, const ADeliveryHandheldItem& B)
	{
		return A.GetName() < B.GetName();
	});
	UDeliveryInventoryComponent* Inventory = Character ? Character->GetInventoryComponent() : nullptr;
	if (!Inventory || Items.Num() < 6)
	{
		UE_LOG(LogDelivery, Error, TEXT("Inventory smoke: FAIL setup character=%s items=%d"), *GetNameSafe(Character), Items.Num());
		return;
	}

	bool bPass = true;
	for (int32 Index = 0; Index < 5; ++Index) bPass &= Inventory->TryPickup(Items[Index]);
	bPass &= !Inventory->TryPickup(Items[5]);
	for (int32 Index = 0; Index < 5; ++Index) bPass &= Inventory->GetItemInSlot(Index) != nullptr;
	ADeliveryHandheldItem* OldHand = Inventory->GetHeldItem();
	ADeliveryHandheldItem* OldSlot2 = Inventory->GetItemInSlot(1);
	Inventory->RequestSlotAction(1);
	bPass &= Inventory->GetHeldItem() == OldSlot2 && Inventory->GetItemInSlot(1) == OldHand;
	Inventory->RequestSlotAction(0);
	bPass &= Inventory->GetHeldItem() == nullptr;
	for (int32 Index = 1; Index < 5; ++Index) bPass &= Inventory->GetItemInSlot(Index) != nullptr;
	UE_LOG(LogDelivery, Log, TEXT("Inventory smoke: %s (pickup5/full-reject/swap/drop/backpack-retain)"), bPass ? TEXT("PASS") : TEXT("FAIL"));
}

void InventoryVisualCapture()
{
	UWorld* World = FindGameWorld();
	if (!World)
	{
		UE_LOG(LogDelivery, Error, TEXT("Inventory visual: no game world"));
		return;
	}
	FTimerHandle ScreenshotTimer;
	World->GetTimerManager().SetTimer(ScreenshotTimer, []
	{
		const FString Filename = FPaths::ProjectSavedDir() / TEXT("Screenshots/InventoryHotbar.png");
		FScreenshotRequest::RequestScreenshot(Filename, true, false);
		UE_LOG(LogDelivery, Log, TEXT("Inventory visual requested: %s"), *Filename);
	}, 1.0f, false);
	FTimerHandle ExitTimer;
	World->GetTimerManager().SetTimer(ExitTimer, []
	{
		FPlatformMisc::RequestExit(false);
	}, 2.5f, false);
}

void InventoryFullVisualCapture()
{
	UWorld* World = FindGameWorld();
	ADeliveryCharacter* Character = nullptr;
	TArray<ADeliveryHandheldItem*> Items;
	if (World)
	{
		for (TActorIterator<ADeliveryCharacter> It(World); It; ++It) { Character = *It; break; }
		for (TActorIterator<ADeliveryHandheldItem> It(World); It; ++It) Items.Add(*It);
	}
	UDeliveryInventoryComponent* Inventory = Character ? Character->GetInventoryComponent() : nullptr;
	if (!World || !Inventory || Items.Num() < 6) return;
	for (int32 Index = 0; Index < 5; ++Index) Inventory->TryPickup(Items[Index]);
	Inventory->TryPickup(Items[5]);

	FTimerHandle ScreenshotTimer;
	World->GetTimerManager().SetTimer(ScreenshotTimer, []
	{
		const FString Filename = FPaths::ProjectSavedDir() / TEXT("Screenshots/InventoryHotbarFull.png");
		FScreenshotRequest::RequestScreenshot(Filename, true, false);
	}, 0.15f, false);
	FTimerHandle ExitTimer;
	World->GetTimerManager().SetTimer(ExitTimer, [] { FPlatformMisc::RequestExit(false); }, 2.0f, false);
}

FAutoConsoleCommand InventorySmokeCommand(
	TEXT("Delivery.Inventory.Smoke"),
	TEXT("Run the server-authoritative five-slot inventory smoke test in PIE/game."),
	FConsoleCommandDelegate::CreateStatic(&InventorySmokeTest));

FAutoConsoleCommand InventoryVisualCommand(
	TEXT("Delivery.Inventory.Visual"),
	TEXT("Capture the empty hotbar in a rendered game window, then exit."),
	FConsoleCommandDelegate::CreateStatic(&InventoryVisualCapture));

FAutoConsoleCommand InventoryFullVisualCommand(
	TEXT("Delivery.Inventory.VisualFull"),
	TEXT("Fill the hotbar, trigger full feedback, capture UI, then exit."),
	FConsoleCommandDelegate::CreateStatic(&InventoryFullVisualCapture));
}
#endif
