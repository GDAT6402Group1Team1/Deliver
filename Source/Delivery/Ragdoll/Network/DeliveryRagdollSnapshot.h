#pragma once

#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"
#include "DeliveryRagdollSnapshot.generated.h"

class ADeliveryMotorbike;

USTRUCT()

struct FDeliveryRagdollBodyState
{
	GENERATED_BODY()

	UPROPERTY()
	FName Bone;

	UPROPERTY()
	FVector_NetQuantize10 Position;

	UPROPERTY()
	FRotator Rotation = FRotator::ZeroRotator;

	UPROPERTY()
	FVector_NetQuantize10 LinearVelocity;

	UPROPERTY()
	FVector_NetQuantize10 AngularVelocity;
};

USTRUCT()

struct FDeliveryRagdollSnapshot
{
	GENERATED_BODY()

	UPROPERTY()
	uint16 Sequence = 0;

	UPROPERTY()
	float ServerTime = 0.0f;

	// 乘员仍发送实际世界姿态，附带同帧车体参考供客户端消除车辆预测与身体插值的时间差
	UPROPERTY() TObjectPtr<ADeliveryMotorbike> Carrier;
	UPROPERTY() FTransform CarrierFrame = FTransform::Identity;
	UPROPERTY() FVector_NetQuantize10 CarrierVelocity = FVector::ZeroVector;

	UPROPERTY()
	TArray<FDeliveryRagdollBodyState> Bodies;
};

