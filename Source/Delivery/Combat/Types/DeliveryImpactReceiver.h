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
 * @brief 能被拳头打飞、被车撞飞，但没有 GAS 血量的角色实现这个接口：路人、居民 NPC、二狗
 *
 * 出拳（GA_DeliverPunch）和撞车（交通车、摩托车）都用 Cast<IDeliveryImpactReceiver> 判断能不能打，
 * 不各自 Cast 到具体的 NPC 类：那样战斗和交通代码要反向依赖 NPC 模块，同一个判断还要写在两个地方
 * 玩家角色不实现它，玩家挨打走 GAS 扣血，有血量、晕倒和起身
 * 发起方的接口是 IDeliveryCombatInterface
 */
class DELIVERY_API IDeliveryImpactReceiver
{
	GENERATED_BODY()

  public:
	/// @brief 服务器：受到一次外力打击
	/// @param Impulse 世界坐标下的冲量，方向和力度由打人或撞人的一方决定
	/// @param ImpactPoint 着力点，决定打中哪个部位
	virtual void ReceiveImpact(const FVector& Impulse, const FVector& ImpactPoint) = 0;

	/// @brief 已经倒地时返回 true；倒地的目标一般不再结算（命中音效、计数都看它）
	virtual bool IsKnockedDown() const = 0;
	/// @brief 倒地后还能挨打时返回 true；默认 false，少数角色（例如居民 NPC）重写
	virtual bool CanReceiveHitWhileDown() const { return false; }
	/// @brief 带上是谁打的或撞的（Source）；默认转给 ReceiveImpact，不关心来源的接收者不用重写
	virtual void ReceiveImpactFrom(const FVector& Impulse, const FVector& Point, AActor* Source) { ReceiveImpact(Impulse, Point); }
	/// @brief 出拳专用，带上这一拳的编号 PunchId，倒地后还能挨打的角色用它防止同一拳算两次；默认转给 ReceiveImpactFrom
	virtual void ReceiveMeleeHitFrom(const FVector& Impulse, const FVector& Point, AActor* Source, FGuid PunchId)
	{ ReceiveImpactFrom(Impulse, Point, Source); }
};
