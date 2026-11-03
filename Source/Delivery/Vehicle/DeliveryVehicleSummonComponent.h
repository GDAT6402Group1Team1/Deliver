// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryVehicleSummonComponent.generated.h"

class ADeliveryMotorbike;

/**
 * 管理载具召唤、冷却和本机按键提示
 * 服务器记录下次可召唤时间，只同步给所属玩家
 * 组件单独更新提示，不依赖角色按需开启的逐帧更新
 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryVehicleSummonComponent : public UActorComponent
{
	GENERATED_BODY()

  public:
	UDeliveryVehicleSummonComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// 召唤操作与冷却查询
	/** 玩家按键；客户端只是"我想召唤"，真正的判定和冷却都在服务器 */
	UFUNCTION(BlueprintCallable, Category = "Vehicle|Summon")
	void RequestSummon();

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

	// 本机提示
	/** 左下角提示文案；%s 会被替换成按键名 */
	UPROPERTY(EditDefaultsOnly, Category = "Vehicle|Summon")
	FText ReadyHintFormat;

	UPROPERTY(EditDefaultsOnly, Category = "Vehicle|Summon")
	FText CooldownHintFormat;

	/** 提示里显示的按键名；由角色在绑定按键时写进来，保证显示和实际绑的键一致 */
	void SetDisplayKeyName(const FText& InKeyName)
	{
		DisplayKeyName = InKeyName;
	}

  private:
	// 服务器召唤与车辆查询
	UFUNCTION(Server, Reliable)
	void ServerSummon();

	/** 服务器：找一辆没人骑的、离玩家最近的车 */
	ADeliveryMotorbike* FindSummonTarget() const;

	/** 本机：把左下角那条提示推给浮窗子系统 */
	void PushHint();

	// 同步冷却时间
	/** 服务器世界时间，到了这个时刻才能再召唤 */
	UPROPERTY(Replicated)
	float ReadyServerTime = 0.0f;

	FText DisplayKeyName;
};
