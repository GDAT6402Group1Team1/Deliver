// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "Combat/DeliveryCombatTypes.h"
#include "DeliveryCombatInterface.generated.h"

/*
*  战斗接口
*/
UINTERFACE(MinimalAPI, BlueprintType)
class UDeliveryCombatInterface : public UInterface
{
	GENERATED_BODY()
};

/** 近战攻击流程：挥拳、收命中结果。由 Character 实现，GA 调用。 */
class IDeliveryCombatInterface
{
	GENERATED_BODY()

public:

	virtual bool StartMeleeAttack(EMeleeHand Hand) = 0;
	virtual TArray<AActor*> GatherMeleeHits(EMeleeHand Hand) const = 0;
	virtual void EndMeleeAttack(EMeleeHand Hand) = 0;
	virtual bool IsMeleeAttacking() const = 0;
};
