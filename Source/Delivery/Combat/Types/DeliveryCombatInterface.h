// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "Combat/Types/DeliveryCombatTypes.h"
#include "DeliveryCombatInterface.generated.h"

UINTERFACE(MinimalAPI, BlueprintType)

class UDeliveryCombatInterface : public UInterface
{
	GENERATED_BODY()
};

/**
 * @brief 玩家角色出拳的接口，ADeliveryCharacter 实现，GA_DeliverPunch 调用
 *
 * 激活技能时调用 StartMeleeAttack，拳头接触时间段里调用 GatherMeleeHits 扫出打到谁，收拳时调用 EndMeleeAttack
 * 受击方的接口是 IDeliveryImpactReceiver
 */
class IDeliveryCombatInterface
{
	GENERATED_BODY()

  public:
	/// @brief 开始挥这只手；身体状态不允许时返回 false
	virtual bool StartMeleeAttack(EMeleeHand Hand) = 0;
	/// @brief 沿这只手这一帧的拳路扫一遍，返回碰到的角色
	virtual TArray<AActor*> GatherMeleeHits(EMeleeHand Hand) const = 0;
	/// @brief 这只手收拳
	virtual void EndMeleeAttack(EMeleeHand Hand) = 0;
	/// @brief 是否正在出拳
	virtual bool IsMeleeAttacking() const = 0;
};
