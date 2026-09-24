// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryLocationRegistry.h"

#include "DeliveryLocationComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

UDeliveryLocationRegistry* UDeliveryLocationRegistry::Get(const UObject* WorldContextObject)
{
	if (!WorldContextObject)
	{
		return nullptr;
	}

	const UWorld* World = GEngine
		? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull)
		: nullptr;

	return World ? World->GetSubsystem<UDeliveryLocationRegistry>() : nullptr;
}

void UDeliveryLocationRegistry::Register(FName LocationId, UDeliveryLocationComponent* Component)
{
	if (LocationId.IsNone() || !Component)
	{
		return;
	}

	// 顺手清掉同 ID 下已经失效的弱引用。注册是个低频事件，挂在这里做就够了——
	// 正常路径上 EndPlay 会注销，但关卡切换、Actor 被强制销毁这些路径不保证走到
	CompactEntries(LocationId);

	TArray<TWeakObjectPtr<UDeliveryLocationComponent>>& Entries = Locations.FindOrAdd(LocationId);
	Entries.AddUnique(Component);
}

void UDeliveryLocationRegistry::Unregister(FName LocationId, UDeliveryLocationComponent* Component)
{
	TArray<TWeakObjectPtr<UDeliveryLocationComponent>>* Entries = Locations.Find(LocationId);
	if (!Entries)
	{
		return;
	}

	Entries->Remove(Component);
	if (Entries->Num() == 0)
	{
		Locations.Remove(LocationId);
	}
}

void UDeliveryLocationRegistry::CompactEntries(FName LocationId)
{
	TArray<TWeakObjectPtr<UDeliveryLocationComponent>>* Entries = Locations.Find(LocationId);
	if (!Entries)
	{
		return;
	}

	Entries->RemoveAll([](const TWeakObjectPtr<UDeliveryLocationComponent>& Entry)
	{
		return !Entry.IsValid();
	});

	if (Entries->Num() == 0)
	{
		Locations.Remove(LocationId);
	}
}

AActor* UDeliveryLocationRegistry::ResolveActor(FName LocationId) const
{
	const TArray<TWeakObjectPtr<UDeliveryLocationComponent>>* Entries = Locations.Find(LocationId);
	if (!Entries)
	{
		return nullptr;
	}

	for (const TWeakObjectPtr<UDeliveryLocationComponent>& Entry : *Entries)
	{
		if (Entry.IsValid())
		{
			return Entry->GetOwner();
		}
	}

	return nullptr;
}

bool UDeliveryLocationRegistry::ResolveLocation(FName LocationId, FVector& OutLocation) const
{
	const TArray<TWeakObjectPtr<UDeliveryLocationComponent>>* Entries = Locations.Find(LocationId);
	if (!Entries)
	{
		return false;
	}

	for (const TWeakObjectPtr<UDeliveryLocationComponent>& Entry : *Entries)
	{
		if (Entry.IsValid())
		{
			// 取组件位置而不是 Actor 原点：收件点挂在整栋楼上时，
			// Actor 原点可能在楼中心甚至地下，指引箭头该指门口
			OutLocation = Entry->GetComponentLocation();

			return true;
		}
	}

	return false;
}

TArray<AActor*> UDeliveryLocationRegistry::ResolveAllActors(FName LocationId) const
{
	TArray<AActor*> Result;

	const TArray<TWeakObjectPtr<UDeliveryLocationComponent>>* Entries = Locations.Find(LocationId);
	if (!Entries)
	{
		return Result;
	}

	for (const TWeakObjectPtr<UDeliveryLocationComponent>& Entry : *Entries)
	{
		if (Entry.IsValid())
		{
			if (AActor* Owner = Entry->GetOwner())
			{
				Result.AddUnique(Owner);
			}
		}
	}

	return Result;
}

TArray<FName> UDeliveryLocationRegistry::GetRegisteredIds() const
{
	TArray<FName> Result;
	Locations.GetKeys(Result);
	Result.Sort(FNameLexicalLess());

	return Result;
}
