// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryVehicleSummonComponent.generated.h"

class ADeliveryMotorbike;

/**
 * 挂在玩家 Pawn 上：按一个键把摩托车召唤到身边，带冷却，并在屏幕左下角常驻提示。
 *
 * 单独一个组件而不是往 ADeliveryCharacter 里塞：角色那个类已经很大了，
 * 而且这件事需要**自己按固定频率 Tick**（推左下角提示），角色的 Tick 是默认关掉的、
 * 只在被车撞的镜头拉远期间才临时打开（`bStartWithTickEnabled=false`）——
 * 借它来推 HUD 会把那套按需开关的逻辑搅乱。
 *
 * 冷却是**服务器权威**的：`ReadyServerTime` 由服务器写、只复制给拥有者，客户端只拿它显示。
 * 客户端自己不预测冷却，因为召唤本来就要等一个来回，抢那几十毫秒没有意义。
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryVehicleSummonComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UDeliveryVehicleSummonComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 玩家按键。客户端只是"我想召唤"，真正的判定和冷却都在服务器。 */
	UFUNCTION(BlueprintCallable, Category="Vehicle|Summon")
	void RequestSummon();

	/** 还要等多少秒才能再召唤。0 = 现在就能按。 */
	UFUNCTION(BlueprintPure, Category="Vehicle|Summon")
	float GetCooldownRemaining() const;

	/** 冷却秒数。 */
	UPROPERTY(EditDefaultsOnly, Category="Vehicle|Summon", meta=(ClampMin="0.0", Units="s"))
	float Cooldown = 10.0f;

	/**
	 * 车放在玩家前方多远。要大于车长的一半（车长 256cm）再留点余量，
	 * 否则召唤出来的车会直接压在玩家身上，把布娃娃顶翻。
	 */
	UPROPERTY(EditDefaultsOnly, Category="Vehicle|Summon", meta=(ClampMin="0.0", Units="cm"))
	float PlaceDistance = 230.0f;

	/** 搜索范围，0 = 整张图都找。默认 0：召唤的意义就是车丢在很远的地方也能叫回来。 */
	UPROPERTY(EditDefaultsOnly, Category="Vehicle|Summon", meta=(ClampMin="0.0", Units="cm"))
	float SearchRadius = 0.0f;

	/** 左下角提示文案。%s 会被替换成按键名。 */
	UPROPERTY(EditDefaultsOnly, Category="Vehicle|Summon")
	FText ReadyHintFormat;

	UPROPERTY(EditDefaultsOnly, Category="Vehicle|Summon")
	FText CooldownHintFormat;

	/** 提示里显示的按键名。由角色在绑定按键时写进来，保证显示和实际绑的键一致。 */
	void SetDisplayKeyName(const FText& InKeyName) { DisplayKeyName = InKeyName; }

private:

	UFUNCTION(Server, Reliable)
	void ServerSummon();

	/** 服务器：找一辆没人骑的、离玩家最近的车。 */
	ADeliveryMotorbike* FindSummonTarget() const;

	/** 本机：把左下角那条提示推给浮窗子系统。 */
	void PushHint();

	/** 服务器世界时间，到了这个时刻才能再召唤。 */
	UPROPERTY(Replicated)
	float ReadyServerTime = 0.0f;

	FText DisplayKeyName;
};
