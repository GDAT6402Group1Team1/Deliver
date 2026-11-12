// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "DeliveryImpactReceiver.generated.h"

UINTERFACE(MinimalAPI, BlueprintType)
class UDeliveryImpactReceiver : public UInterface
{
	GENERATED_BODY()
};

/**
 * 能被打飞/撞飞但不走 GAS 的东西（当前是路人）
 *
 * 为什么是接口而不是在出拳和撞车两处各写一个 Cast：
 * 那两处都在 Combat/ 和 Traffic/ 里，而路人在 NPC/ 下——加 Cast 等于让战斗代码
 * 反向依赖 NPC 模块，而且**同一个判断会出现在两个地方**。
 * 本项目在"同一个概念两处判断"上吃过亏（箭头指送达点、图钉还是箱子），
 * 所以这里让"谁能被打飞"由被打的那一方声明。
 *
 * 和 IDeliveryCombatInterface 的分工：那个是"我会攻击"（发起方的流程），
 * 这个是"我会被攻击"（受击方的反应）。玩家角色不实现这个接口——
 * 它走 GAS 扣血那条完整路径，有血量、晕倒和起身；路人只有被打飞和退场。
 */
class DELIVERY_API IDeliveryImpactReceiver
{
	GENERATED_BODY()

  public:
	/**
	 * 受到一次外力打击，由服务器调用
	 * @param Impulse     世界空间冲量，方向和力度都由发起方决定
	 * @param ImpactPoint 着力点，用来决定被打中的部位
	 */
	virtual void ReceiveImpact(const FVector& Impulse, const FVector& ImpactPoint) = 0;

	/** 已经被打飞的目标不再重复结算（音效、命中计数都看这个） */
	virtual bool IsKnockedDown() const = 0;
	// 来源在命中时采集，旧接收者继续走原接口
	virtual void ReceiveImpactFrom(const FVector& Impulse, const FVector& Point, AActor* Source) { ReceiveImpact(Impulse, Point); }
};
