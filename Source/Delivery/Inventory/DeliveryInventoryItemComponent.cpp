// 保存物品类型、图标与耐久，物体的场景表现由所属 Actor 处理
// Copyright Epic Games, Inc. All Rights Reserved.

#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Net/UnrealNetwork.h"

UDeliveryInventoryItemComponent::UDeliveryInventoryItemComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
	DisplayName = NSLOCTEXT("DeliveryItems", "UnnamedItem", "道具");
}

// 向客户端同步物品耐久
void UDeliveryInventoryItemComponent::GetLifetimeReplicatedProps(
    TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UDeliveryInventoryItemComponent, Durability);
}

// 由服务器扣除耐久，耗尽后执行物品销毁流程
void UDeliveryInventoryItemComponent::ApplyDurabilityDamage(float Amount)
{
	if (!bUsesDurability || !GetOwner() || !GetOwner()->HasAuthority() || Amount <= 0.0f)
	{
		return;
	}
	Durability = FMath::Max(0.0f, Durability - Amount);
	OnRep_Durability();
	if (Durability <= 0.0f)
	{
		if (ADeliveryHandheldItem* Item = Cast<ADeliveryHandheldItem>(GetOwner()))
		{
			if (ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(Item->GetOwner()))
			{
				if (UDeliveryInventoryComponent* Inventory = Character->GetInventoryComponent())
				{
					Inventory->RemoveItem(Item);
				}
			}
		}
		GetOwner()->Destroy();
	}
}

// 把当前耐久转换为进度条使用的比例
float UDeliveryInventoryItemComponent::GetDurabilityFraction() const
{
	return bUsesDurability && MaxDurability > KINDA_SMALL_NUMBER
	           ? FMath::Clamp(Durability / MaxDurability, 0.0f, 1.0f)
			   : 0.0f;
}

void UDeliveryInventoryItemComponent::OnRep_Durability()
{
	Durability = bUsesDurability ? FMath::Clamp(Durability, 0.0f, FMath::Max(0.0f, MaxDurability)) : 0.0f;
}
