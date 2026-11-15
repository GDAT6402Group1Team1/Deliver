// 管理服务器权威背包；槽位 0 为手持物，槽位 1 至 4 为背包
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryInventoryComponent.generated.h"

class ADeliveryCharacter;
class ADeliveryHandheldItem;
class ADeliveryPackageItem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDeliveryInventoryChanged);

/** 由服务器管理五格物品栏，零号槽为手持物，其余四格为背包 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryInventoryComponent : public UActorComponent
{
	GENERATED_BODY()

  public:
	UDeliveryInventoryComponent();
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// 槽位约定与变化通知
	static constexpr int32 SlotCount = 5;
	static constexpr int32 FirstBackpackSlot = 1;

	UPROPERTY(BlueprintAssignable, Category = "Inventory")
	FDeliveryInventoryChanged OnInventoryChanged;

	// 物品栏查询
	// 物品查询
	UFUNCTION(BlueprintPure, Category = "Inventory")
	ADeliveryHandheldItem* GetItemInSlot(int32 SlotIndex) const;

	UFUNCTION(BlueprintPure, Category = "Inventory")

	ADeliveryHandheldItem* GetHeldItem() const
	{
		return GetItemInSlot(0);
	}

	UFUNCTION(BlueprintPure, Category = "Inventory")

	bool HasHeldItem() const
	{
		return GetHeldItem() != nullptr;
	}

	UFUNCTION(BlueprintPure, Category = "Inventory")
	bool HasBackpackSpace() const;

	ADeliveryPackageItem* GetStoredTaskPackage() const;
	bool IsTaskPackageInBackpack(const ADeliveryPackageItem* Package) const;
	bool CanStoreTaskPackageInBackpack() const;

	// 只向物品栏所属玩家提供自己主动放下且仍在地面的首单原件
	const ADeliveryPackageItem* GetDroppedFirstOrderPackage() const;

    // 交接失败恢复实物时复用当前库存的掉落偏移
    float GetDropForwardDistance() const { return DropForwardDistance; }
    float GetDropUpOffset() const { return DropUpOffset; }

	UFUNCTION(BlueprintPure, Category = "Inventory|UI")
	float GetFullFeedbackAlpha() const;

	// 拾取与物品操作
	// 拾取、切换和移除
	/** 服务器拾取接口，检查物品和槽位条件后更新背包 */
	bool TryPickup(ADeliveryHandheldItem* Item);
	bool TryStoreTaskPackageInBackpack(ADeliveryPackageItem* Package);
	bool ConsumeTaskPackage(ADeliveryPackageItem* Package);

    // 普通交还沿用原容量规则，查询不会挤掉旧手持物或改变槽位
    bool CanReceiveFetchedItem(ADeliveryHandheldItem* Item) const;
    bool ContainsItem(const ADeliveryHandheldItem* Item) const;
    bool ReleaseFetchedItem(ADeliveryHandheldItem* Item);
    void RefreshItemPresentation(ADeliveryHandheldItem* Item);

	/** 本机输入入口，索引从 0 开始，0 对应数字键 1 */
	void RequestSlotAction(int32 SlotIndex);

	/** 仅丢弃数组下标 0 的手持物，由服务器执行；晕倒流程也调用此接口 */
	void DropHeldItem();
	void RemoveItem(ADeliveryHandheldItem* Item);
	/** 服务器移除指定手持物的背包关系，不执行丢弃，用于成功交付后的销毁流程 */
	bool ConsumeHeldItem(ADeliveryHandheldItem* ExpectedItem);

	void UseHeldItem();

  protected:
	// 同步槽位与丢弃参数
	UPROPERTY(ReplicatedUsing = OnRep_Slots, BlueprintReadOnly, Category = "Inventory")
	TArray<TObjectPtr<ADeliveryHandheldItem>> Slots;

	UPROPERTY(EditDefaultsOnly, Category = "Inventory|Drop")
	float DropForwardDistance = 75.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Inventory|Drop")
	float DropUpOffset = 35.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Inventory|Drop")
	float DropForwardSpeed = 80.0f;

  private:

	// 服务器槽位操作
	// 网络请求与同步回调
	UFUNCTION(Server, Reliable)
	void ServerSlotAction(int32 SlotIndex);

	UFUNCTION(Client, Reliable)
	void ClientInventoryFull();

	// 客户端物品栏刷新
	UFUNCTION()
	void OnRep_Slots();

	friend class FDeliveryMotorbikeTrunkTest;
	friend class ADeliveryPackageItem;
	void ForgetDroppedFirstOrderPackage(const ADeliveryPackageItem* Package);
	UPROPERTY(Replicated)
	TObjectPtr<ADeliveryPackageItem> DroppedFirstOrderPackage;

	// 槽位处理与物体显示
	void PerformSlotAction(int32 SlotIndex);
	// 主动放快递需要当前位置可放，失败时保留两格；晕倒仍可强制掉落
	bool TryDropHeldItem(bool bCheckPackagePlacement);
	bool TryPickupInSlot(ADeliveryHandheldItem* Item, bool bStorePackageInBackpack);
	bool ConsumeItemInSlot(int32 SlotIndex, ADeliveryHandheldItem* Item);
	void ApplyPresentation();
	void StoreItem(int32 SlotIndex, ADeliveryHandheldItem* Item);
	int32 FindEmptyBackpackSlot() const;
	bool CanMutateInventory() const;
	ADeliveryCharacter* GetCharacter() const;
	float FullFeedbackStartedAt = -1000.0f;
};
