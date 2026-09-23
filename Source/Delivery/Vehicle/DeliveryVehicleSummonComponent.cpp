// Copyright Epic Games, Inc. All Rights Reserved.

#include "Vehicle/DeliveryVehicleSummonComponent.h"
#include "Delivery.h"
#include "DeliveryCharacter.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Interaction/DeliveryPromptSubsystem.h"
#include "Net/UnrealNetwork.h"
#include "Vehicle/DeliveryMotorbike.h"

UDeliveryVehicleSummonComponent::UDeliveryVehicleSummonComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// 只用来刷左下角那条提示，10Hz 足够，读秒的小数位也看不出跳。
	PrimaryComponentTick.TickInterval = 0.1f;
	SetIsReplicatedByDefault(true);

	ReadyHintFormat = NSLOCTEXT("Delivery", "SummonReady", "按 {0} 召唤摩托车");
	CooldownHintFormat = NSLOCTEXT("Delivery", "SummonCooling", "召唤冷却 {0}s");
}

void UDeliveryVehicleSummonComponent::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// 只发给拥有者：别人的冷却和我无关，没必要占带宽。
	DOREPLIFETIME_CONDITION(UDeliveryVehicleSummonComponent, ReadyServerTime, COND_OwnerOnly);
}

float UDeliveryVehicleSummonComponent::GetCooldownRemaining() const
{
	const UWorld* World = GetWorld();
	return World ? FMath::Max(0.0f, ReadyServerTime - World->GetTimeSeconds()) : 0.0f;
}

void UDeliveryVehicleSummonComponent::TickComponent(
	float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const APawn* Owner = Cast<APawn>(GetOwner());
	// 上了车之后控制器去 Possess 摩托车了，这具身体不再是本机操控的，
	// 于是召唤提示自动让位给车上那条"按 P 切换视角"，两条不会打架。
	if (Owner && Owner->IsLocallyControlled())
	{
		PushHint();
	}
}

void UDeliveryVehicleSummonComponent::PushHint()
{
	UDeliveryPromptSubsystem* Prompt = UDeliveryPromptSubsystem::Get(this);
	if (!Prompt || DisplayKeyName.IsEmpty())
	{
		return;
	}

	// 图里根本没有摩托车时不提示：告诉玩家一个按了什么都不会发生的键没有意义。
	// 客户端上车也是复制过来的，所以这个判断在两端都成立。
	bool bAnyBike = false;
	if (const UWorld* World = GetWorld())
	{
		for (TActorIterator<ADeliveryMotorbike> It(const_cast<UWorld*>(World)); It; ++It)
		{
			bAnyBike = true;
			break;
		}
	}
	if (!bAnyBike)
	{
		return;
	}

	const float Remaining = GetCooldownRemaining();
	if (Remaining > 0.0f)
	{
		Prompt->PushCornerHint(
			FText::Format(CooldownHintFormat, FText::AsNumber(FMath::CeilToInt(Remaining))), true);
	}
	else
	{
		Prompt->PushCornerHint(FText::Format(ReadyHintFormat, DisplayKeyName), false);
	}
}

void UDeliveryVehicleSummonComponent::RequestSummon()
{
	// 本机先挡一道，纯粹是省一次无用的 RPC；真正的冷却判定在服务器。
	if (GetCooldownRemaining() > 0.0f)
	{
		return;
	}
	ServerSummon();
}

ADeliveryMotorbike* UDeliveryVehicleSummonComponent::FindSummonTarget() const
{
	UWorld* World = GetWorld();
	const AActor* Owner = GetOwner();
	if (!World || !Owner)
	{
		return nullptr;
	}

	const FVector From = Owner->GetActorLocation();
	ADeliveryMotorbike* Best = nullptr;
	float BestDistSq = TNumericLimits<float>::Max();

	for (TActorIterator<ADeliveryMotorbike> It(World); It; ++It)
	{
		ADeliveryMotorbike* Bike = *It;
		// 别人正骑着的车不能抽走。
		if (!IsValid(Bike) || Bike->GetDriver() != nullptr)
		{
			continue;
		}
		const float DistSq = FVector::DistSquared(From, Bike->GetActorLocation());
		if (SearchRadius > 0.0f && DistSq > FMath::Square(SearchRadius))
		{
			continue;
		}
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = Bike;
		}
	}
	return Best;
}

void UDeliveryVehicleSummonComponent::ServerSummon_Implementation()
{
	UWorld* World = GetWorld();
	AActor* Owner = GetOwner();
	if (!World || !Owner)
	{
		return;
	}

	const float Now = World->GetTimeSeconds();
	if (Now < ReadyServerTime)
	{
		return;
	}

	ADeliveryMotorbike* Bike = FindSummonTarget();
	if (!Bike)
	{
		return;
	}

	// 放在玩家正前方、车头朝着玩家的朝向——召唤出来就是"可以直接骑上去往前开"的姿态。
	const FRotator OwnerRot = Owner->GetActorRotation();
	const FVector Forward = FRotator(0.0f, OwnerRot.Yaw, 0.0f).Vector();
	const FVector Target = Owner->GetActorLocation() + Forward * PlaceDistance;

	// 失败（探不到地面）就不进冷却：玩家没得到任何东西，不该被罚等 10 秒。
	if (!Bike->SummonTo(Target, OwnerRot.Yaw))
	{
		UE_LOG(LogDelivery, Log, TEXT("召唤失败：%s 的落点探不到地面，不计冷却。"), *Bike->GetName());
		return;
	}

	ReadyServerTime = Now + Cooldown;
}
