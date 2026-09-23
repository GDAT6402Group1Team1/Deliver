// Copyright Epic Games, Inc. All Rights Reserved.

#include "Traffic/DeliveryTrafficCarComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "DeliveryCharacter.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "GAS/DeliverAttributeSet.h"
#include "GAS/DeliverGameplayTags.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/PrimitiveComponent.h"

UDeliveryTrafficCarComponent::UDeliveryTrafficCarComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

void UDeliveryTrafficCarComponent::BeginPlay()
{
	Super::BeginPlay();
	// Camera 弹簧臂仍会避开墙/地形，但车身不再把镜头瞬间推到角色脸前。
	if (AActor* Owner = GetOwner())
	{
		// 已摆在地图里的旧车实例序列化过 bReplicates=false，覆盖了蓝图新默认值。
		// 在权威端运行时注册复制，不改地图资产也能让这些实例发出位置快照。
		if (Owner->HasAuthority())
		{
			Owner->SetReplicates(true);
		}
		// 客户端地图实例也有旧的 bReplicateMovement=false 覆盖；
		// OnRep_ReplicatedMovement 遇到 false 会直接丢弃服务器位置。
		Owner->SetReplicateMovement(true);
		TArray<UPrimitiveComponent*> Primitives;
		Owner->GetComponents(Primitives);
		for (UPrimitiveComponent* Primitive : Primitives)
		{
			Primitive->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
		}
	}
	if (const AActor* Owner = GetOwner())
	{
		PreviousImpactTransform = Owner->GetActorTransform();
		bHasPreviousImpactTransform = true;
	}
}

void UDeliveryTrafficCarComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}
	ProcessVehicleImpacts(DeltaTime);

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
	if (const AActor* Owner = GetOwner())
	{
		PreviousImpactTransform = Owner->GetActorTransform();
		bHasPreviousImpactTransform = true;
		bSkipNextImpactSweep = true;
	}
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

void UDeliveryTrafficCarComponent::ProcessVehicleImpacts(float DeltaTime)
{
	AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!Owner || !World || DeltaTime <= KINDA_SMALL_NUMBER)
	{
		return;
	}
	const FTransform Current = Owner->GetActorTransform();
	if (!bHasPreviousImpactTransform || bSkipNextImpactSweep)
	{
		PreviousImpactTransform = Current;
		bHasPreviousImpactTransform = true;
		bSkipNextImpactSweep = false;
		return;
	}
	const FVector Travel = Current.GetLocation() - PreviousImpactTransform.GetLocation();
	if (Travel.SizeSquared() > FMath::Square(300.0f))
	{
		// 路线循环传送不能沿整张地图扫出一次虚假的撞击。
		PreviousImpactTransform = Current;
		return;
	}
	const FVector CarVelocity = Travel / DeltaTime;
	if (CarVelocity.SizeSquared2D() < FMath::Square(30.0f))
	{
		PreviousImpactTransform = Current;
		return;
	}
	const FVector Start = PreviousImpactTransform.TransformPosition(ImpactCenterOffset);
	const FVector End = Current.TransformPosition(ImpactCenterOffset);
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ECC_PhysicsBody);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(DeliveryTrafficImpact), false, Owner);
	Params.AddIgnoredActor(Owner);
	TArray<FHitResult> Hits;
	World->SweepMultiByObjectType(Hits, Start, End, Current.GetRotation(), Objects,
		FCollisionShape::MakeBox(ImpactHalfExtent.GetAbs()), Params);
	TSet<ADeliveryCharacter*> SeenCharacters;
	for (const FHitResult& Hit : Hits)
	{
		ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(Hit.GetActor());
		if (!Character || SeenCharacters.Contains(Character))
		{
			continue;
		}
		SeenCharacters.Add(Character);
		const float Now = World->GetTimeSeconds();
		if (const float* LastHit = LastImpactTimeByActor.Find(Character))
		{
			if (Now - *LastHit < RepeatHitCooldown)
			{
				continue;
			}
		}
		USkeletalMeshComponent* Mesh = Character->GetMesh();
		UDeliveryActiveRagdollComponent* Ragdoll = Character->GetActiveRagdoll();
		UAbilitySystemComponent* ASC = Character->GetAbilitySystemComponent();
		if (!Mesh || !Ragdoll || !ASC || !Character->GetDamageEffect())
		{
			continue;
		}
		const FVector Direction = CarVelocity.GetSafeNormal2D();
		const FVector BodyVelocity = Mesh->GetPhysicsLinearVelocity(TEXT("Hips"));
		const float ClosingSpeed = FVector::DotProduct(CarVelocity - BodyVelocity, Direction);
		const float DeltaV = FMath::Max(0.0f, ClosingSpeed)
			* VehicleEffectiveMassKg / FMath::Max(VehicleEffectiveMassKg + CharacterEffectiveMassKg, 1.0f);
		if (DeltaV < MinimumDamageDeltaV || ASC->HasMatchingGameplayTag(TAG_State_Stunned))
		{
			continue;
		}
		const float Health = ASC->GetNumericAttribute(UDeliverAttributeSet::GetHealthAttribute());
		const bool bStrongHit = DeltaV >= KnockdownDeltaV;
		// 先取地面状态；后面的 HP=0 会立即把角色切进 Limp。
		const bool bTakeoff = bStrongHit && Ragdoll->IsGrounded();
		const float Damage = bStrongHit ? Health
			: FMath::Clamp((DeltaV - MinimumDamageDeltaV) * DamagePerDeltaV, 0.0f, MaximumLightDamage);
		if (Damage <= 0.0f)
		{
			continue;
		}
		const FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
		FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(Character->GetDamageEffect(), 1.0f, Context);
		if (!Spec.IsValid())
		{
			continue;
		}
		Spec.Data->SetSetByCallerMagnitude(TAG_Effect_Type_Damage, -Damage);
		ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
		const FGameplayAttribute HealthAttribute = UDeliverAttributeSet::GetHealthAttribute();
		// GE 是既有伤害路径，但在满血回复效果仍激活时可能被其它 GE 修饰抵消。
		// 服务器检查结算后的值，仅在未达到这次应扣 HP 时补足，避免强撞偶发不晕倒。
		const float ExpectedHealth = FMath::Max(0.0f, Health - Damage);
		if (ASC->GetNumericAttribute(HealthAttribute) > ExpectedHealth + 0.5f)
		{
			ASC->SetNumericAttributeBase(HealthAttribute, ExpectedHealth);
		}
		const FVector HorizontalVelocityChange = Direction * FMath::Min(DeltaV, MaximumKnockbackDeltaV);
		float TakeoffDeltaV = 0.0f;
		if (bTakeoff)
		{
			// 一次性补到目标向上速度，已有上升速度不叠加；弱撞及空中再撞不追加升力。
			// 只推髋部会被整条受约束刚体链分摊得几乎看不出升高，竖直分量须给全身刚体。
			TakeoffDeltaV = FMath::Max(0.0f, StrongHitTakeoffSpeed - BodyVelocity.Z);
		}
		Mesh->AddImpulse(HorizontalVelocityChange, TEXT("Hips"), true);
		if (TakeoffDeltaV > 0.0f)
		{
			Mesh->AddImpulseToAllBodiesBelow(FVector::UpVector * TakeoffDeltaV,
				TEXT("Hips"), true, true);
		}
		Character->NotifyVehicleImpact();
		UE_LOG(LogTemp, Log, TEXT("DeliveryVehicleImpact car=%s target=%s deltaV=%.1f launchZ=%.1f damage=%.1f healthAfter=%.1f"),
			*GetNameSafe(Owner), *GetNameSafe(Character), DeltaV, TakeoffDeltaV,
			Damage,
			ASC->GetNumericAttribute(UDeliverAttributeSet::GetHealthAttribute()));
		LastImpactTimeByActor.Add(Character, Now);
	}

	ProcessMotorbikeImpacts(Start, End, Current.GetRotation(), CarVelocity, Params);

	PreviousImpactTransform = Current;
}

void UDeliveryTrafficCarComponent::ProcessMotorbikeImpacts(
	const FVector& Start, const FVector& End, const FQuat& Rotation,
	const FVector& CarVelocity, const FCollisionQueryParams& Params)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 单独扫一遍，不并进上面那次查询：摩托车的碰撞盒是 Pawn 配置（ObjectType = ECC_Pawn），
	// 而上面查的是 ECC_PhysicsBody（角色的布娃娃刚体）。把 Pawn 加进同一次查询的话，
	// 角色胶囊也会跟着被扫到，会扰动那条已经调通的角色撞击逻辑，不值得。
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ECC_Pawn);
	TArray<FHitResult> Hits;
	World->SweepMultiByObjectType(Hits, Start, End, Rotation, Objects,
		FCollisionShape::MakeBox(ImpactHalfExtent.GetAbs()), Params);

	TSet<ADeliveryMotorbike*> SeenBikes;
	for (const FHitResult& Hit : Hits)
	{
		ADeliveryMotorbike* Bike = Cast<ADeliveryMotorbike>(Hit.GetActor());
		if (!Bike || SeenBikes.Contains(Bike))
		{
			continue;
		}
		SeenBikes.Add(Bike);

		// 和角色撞击共用同一张冷却表：一次碰撞会连着好几帧都重叠，
		// 不设冷却的话"撞两下才下车"会在一次碰撞里就被扣完。
		const float Now = World->GetTimeSeconds();
		if (const float* LastHit = LastImpactTimeByActor.Find(Bike))
		{
			if (Now - *LastHit < RepeatHitCooldown)
			{
				continue;
			}
		}
		if (Bike->NotifyTrafficImpact(CarVelocity))
		{
			LastImpactTimeByActor.Add(Bike, Now);
		}
	}
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
