// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryVehicleSummonComponent.generated.h"

class ADeliveryMotorbike;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDeliveryCooldownChanged);

/**
 * 在服务器检查召唤条件，生成或移动车辆并记录冷却结束时间
 * 将冷却时间同步给操作玩家，时间变化时通知提示组件
 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryVehicleSummonComponent : public UActorComponent
{
	GENERATED_BODY()

  public:
	UDeliveryVehicleSummonComponent();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// 召唤操作
	/** 玩家按键；客户端只是"我想召唤"，真正的判定和冷却都在服务器 */
	UFUNCTION(BlueprintCallable, Category = "Vehicle|Summon")
	void RequestSummon();

	// 冷却查询
	/** 还要等多少秒才能再召唤；0 = 现在就能按 */
	UFUNCTION(BlueprintPure, Category = "Vehicle|Summon")
	float GetCooldownRemaining() const;

	// 召唤配置
	/** 冷却秒数 */
	UPROPERTY(EditDefaultsOnly, Category = "Vehicle|Summon", meta = (ClampMin = "0.0", Units = "s"))
	float Cooldown = 10.0f;

	/**
	 * 车放在玩家前方多远；要大于车长的一半（车长 256cm）再留点余量，
	 * 否则召唤出来的车会直接压在玩家身上，把布娃娃顶翻
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Vehicle|Summon", meta = (ClampMin = "0.0", Units = "cm"))
	float PlaceDistance = 230.0f;

	/** 搜索范围，0 = 整张图都找；默认 0：召唤的意义就是车丢在很远的地方也能叫回来 */
	UPROPERTY(EditDefaultsOnly, Category = "Vehicle|Summon", meta = (ClampMin = "0.0", Units = "cm"))
	float SearchRadius = 0.0f;

	/** 地图没有摩托车时由服务器生成的车型；已有车时仍只召回空车；留空可禁用生成 */
	UPROPERTY(EditDefaultsOnly, Category = "Vehicle|Summon")
	TSoftClassPtr<ADeliveryMotorbike> DefaultVehicleClass;

    /** 有可召回的车辆，或允许在空地图生成默认车型时显示召唤操作 */
    bool CanOfferSummon() const;

    UPROPERTY(BlueprintAssignable, Category="Vehicle|Summon")
    FDeliveryCooldownChanged OnCooldownChanged;

  private:
	// 服务器召唤与车辆查询
	UFUNCTION(Server, Reliable)
	void ServerSummon();

	/** 服务器：找一辆没人骑的、离玩家最近的车 */
	ADeliveryMotorbike* FindSummonTarget() const;

    UFUNCTION()
    void OnRep_ReadyServerTime();

	// 同步冷却时间
	/** 服务器世界时间，到了这个时刻才能再召唤 */
	UPROPERTY(ReplicatedUsing=OnRep_ReadyServerTime)
	float ReadyServerTime = 0.0f;

};
