// Copyright Epic Games, Inc. All Rights Reserved.

#include "World/DeliveryLocationRegistry.h"

#include "World/DeliveryLocationComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

// 从传入对象所在世界取得地点注册表
UDeliveryLocationRegistry* UDeliveryLocationRegistry::Get(const UObject* WorldContextObject)
{
	if (!WorldContextObject)
	{
		return nullptr;
	}

	const UWorld* World =
	    GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull)
		        : nullptr;

	return World ? World->GetSubsystem<UDeliveryLocationRegistry>() : nullptr;
}

// 按地点编号登记组件，避免重复记录
void UDeliveryLocationRegistry::Register(FName LocationId, UDeliveryLocationComponent* Component)
{
	if (LocationId.IsNone() || !Component)
	{
		return;
	}

	// 顺手清掉同 ID 下已经失效的弱引用；注册是个低频事件，挂在这里做就够了——
	// 正常路径上 EndPlay 会注销，但关卡切换、Actor 被强制销毁这些路径不保证走到
	CompactEntries(LocationId);

	TArray<TWeakObjectPtr<UDeliveryLocationComponent>>& Entries = Locations.FindOrAdd(LocationId);
	Entries.AddUnique(Component);
}

// 移除指定地点组件并清理空编号
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

// 清除编号下已经失效的地点引用
void UDeliveryLocationRegistry::CompactEntries(FName LocationId)
{
	TArray<TWeakObjectPtr<UDeliveryLocationComponent>>* Entries = Locations.Find(LocationId);
	if (!Entries)
	{
		return;
	}

	Entries->RemoveAll(
	    [](const TWeakObjectPtr<UDeliveryLocationComponent>& Entry)
	    {
		    return !Entry.IsValid();
	    });

	if (Entries->Num() == 0)
	{
		Locations.Remove(LocationId);
	}
}

// 取得指定编号下第一个有效地点对象
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

// 取得地点组件的位置，而非建筑模型的原点
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

// 收集指定编号对应的所有有效场景对象
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

// 收集指定编号对应的所有有效地点组件
TArray<UDeliveryLocationComponent*> UDeliveryLocationRegistry::ResolveAllComponents(FName LocationId) const
{
	TArray<UDeliveryLocationComponent*> Result;

	const TArray<TWeakObjectPtr<UDeliveryLocationComponent>>* Entries = Locations.Find(LocationId);
	if (!Entries)
	{
		return Result;
	}

	for (const TWeakObjectPtr<UDeliveryLocationComponent>& Entry : *Entries)
	{
		if (UDeliveryLocationComponent* Component = Entry.Get())
		{
			Result.AddUnique(Component);
		}
	}

	return Result;
}

// 返回当前登记的地点编号，供检查和编辑器工具使用
TArray<FName> UDeliveryLocationRegistry::GetRegisteredIds() const
{
	TArray<FName> Result;
	Locations.GetKeys(Result);
	Result.Sort(FNameLexicalLess());

	return Result;
}
