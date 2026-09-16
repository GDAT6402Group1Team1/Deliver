// Copyright Epic Games, Inc. All Rights Reserved.

#include "Traffic/DeliveryTrafficCarComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

UDeliveryTrafficCarComponent::UDeliveryTrafficCarComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UDeliveryTrafficCarComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bIsCurrentlyOnRoute && bHasReachedMaxSpeed)
	{
		// 只有"已经跑到满速却还是接不上路"才算真正的裸奔。车刚重置完或刚从红灯起步时
		// 速度还没上来，这时候它接不上样条是正常的，不该计时，否则会反复自我重置。
		RouteLostElapsedTime += DeltaTime;
		if (RouteLostElapsedTime >= RouteLostTimeout)
		{
			RouteLostElapsedTime = 0.0f;
			OnRouteLost.Broadcast();
		}
	}
	else if (!bHasReachedMaxSpeed)
	{
		// 掉速了（红灯刹车、避让减速、或者刚被重置）就把计时清零，
		// 保证下次是从"速度重新回到 MaxSpeed 的那一刻"重新开始算满 RouteLostTimeout。
		RouteLostElapsedTime = 0.0f;
	}

	// 红灯期间前车是合法地长时间静止，不是死锁，用更宽松的超时，避免后车提前放弃避让压上去。
	const float EffectiveMaxBlockedTime = bStoppedAtIntersection ? MaxBlockedTimeAtIntersection : MaxBlockedTime;

	const float DistanceAhead = GetDistanceToCarAhead();

	// 按距离渐进，而不是"探测到就刹死"：探测边缘处系数还是 1（完全不减速），
	// 越靠近越小，到 MinFollowDistance 以内才真正压到 0。这样可以探测得很远
	// （提前平缓减速）同时停得很近，不会隔着一整个探测距离就杵住。
	float Target = 1.0f;
	if (DistanceAhead >= 0.0f)
	{
		const float Span = FMath::Max(ForwardTraceDistance - MinFollowDistance, 1.0f);
		Target = FMath::Clamp((DistanceAhead - MinFollowDistance) / Span, 0.0f, 1.0f);
	}

	// 只有真正被逼停（系数已经压到接近 0）才算"卡住"。跟在后面慢慢开不该累计死锁时间，
	// 否则正常跟车几秒之后也会触发强行通过，反而撞上去。
	if (Target <= KINDA_SMALL_NUMBER)
	{
		BlockedElapsedTime += DeltaTime;
		if (BlockedElapsedTime >= EffectiveMaxBlockedTime)
		{
			// 汇入口两辆车互为"前车"时会一直卡住，卡够久就放弃避让强行通过，打破死锁。
			Target = 1.0f;
		}
	}
	else
	{
		BlockedElapsedTime = 0.0f;
	}

	SpeedMultiplier = FMath::FInterpConstantTo(SpeedMultiplier, Target, DeltaTime, SpeedMultiplierChangeRate);

	DebugSpeedMultiplier = SpeedMultiplier;
	DebugDistanceAhead = DistanceAhead;
	DebugBlockedElapsed = BlockedElapsedTime;
}

void UDeliveryTrafficCarComponent::UpdateRouteFollowState(bool bIsFollowingRoute)
{
	bIsCurrentlyOnRoute = bIsFollowingRoute;
	if (bIsFollowingRoute)
	{
		RouteLostElapsedTime = 0.0f;
	}
}

void UDeliveryTrafficCarComponent::UpdateSpeedState(float InCurrentSpeed, float InMaxSpeed)
{
	// MaxSpeed 是蓝图里的纯配置量，正常恒为正；防一手除零/负值配置。
	bHasReachedMaxSpeed = (InMaxSpeed > 0.0f) && (InCurrentSpeed >= InMaxSpeed - KINDA_SMALL_NUMBER);
}

void UDeliveryTrafficCarComponent::UpdateStoppedAtIntersection(bool bInStoppedAtIntersection)
{
	bStoppedAtIntersection = bInStoppedAtIntersection;
}

void UDeliveryTrafficCarComponent::NotifyResetToStart()
{
	bIsCurrentlyOnRoute = true;
	RouteLostElapsedTime = 0.0f;

	// 重置要做到和刚 BeginPlay 时完全一样：避让系数、卡车计时都得跟着归零，
	// 不然车瞬移回出生点后可能还带着重置前"正在让路"的残留状态（比如速度系数
	// 还没插值回 1、卡住计时还没清），跟真正刚出生的车表现不一致。
	SpeedMultiplier = 1.0f;
	BlockedElapsedTime = 0.0f;

	// 车刚被瞬移回出生点，速度也被蓝图归零了，这里跟着回到"还没跑起来"的状态：
	// 裸奔计时要等它重新加速到 MaxSpeed 才开始，红灯标记也不能留着重置前的判断结果。
	bHasReachedMaxSpeed = false;
	bStoppedAtIntersection = false;

	DebugSpeedMultiplier = 1.0f;
	DebugDistanceAhead = -1.0f;
	DebugBlockedElapsed = 0.0f;
}

float UDeliveryTrafficCarComponent::GetDistanceToCarAhead() const
{
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return -1.0f;
	}

	const FVector Forward = Owner->GetActorForwardVector();
	const FVector Start = Owner->GetActorLocation() + Forward * TraceStartForwardOffset;
	const FVector End = Start + Forward * ForwardTraceDistance;

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(DeliveryTrafficCarForwardTrace), false, Owner);
	QueryParams.AddIgnoredActor(Owner);

	TArray<FHitResult> Hits;
	GetWorld()->SweepMultiByChannel(Hits, Start, End, FQuat::Identity, TraceChannel.GetValue(), FCollisionShape::MakeSphere(ForwardTraceRadius), QueryParams);

	if (bDrawDebugTrace)
	{
		DrawDebugCapsule(GetWorld(), (Start + End) * 0.5f, (End - Start).Size() * 0.5f, ForwardTraceRadius, FRotationMatrix::MakeFromZ(Forward).ToQuat(), FColor::Yellow, false, -1.0f, 0, 1.5f);
	}

	// 取最近的一辆，多辆车同时在扫描范围内时应该跟最前面那个的距离减速。
	float Nearest = -1.0f;
	for (const FHitResult& Hit : Hits)
	{
		const AActor* HitActor = Hit.GetActor();
		if (HitActor && HitActor != Owner && HitActor->FindComponentByClass<UDeliveryTrafficCarComponent>())
		{
			// 起始就重叠时 Distance 是 0，当成贴脸处理（0 会让 Target 直接算成 0，正是想要的）。
			const float D = Hit.bStartPenetrating ? 0.0f : Hit.Distance;
			if (Nearest < 0.0f || D < Nearest)
			{
				Nearest = D;
			}
		}
	}

	return Nearest;
}
