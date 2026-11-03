// Copyright Epic Games, Inc. All Rights Reserved.

#include "Traffic/DeliveryTrafficCarComponent.h"
#include "Delivery.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Character/DeliveryCharacter.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "AbilitySystem/DeliverAttributeSet.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "TimerManager.h"

/** 通过反射读写蓝图车速，兼容单精度和双精度数值属性 */
static const FName MaxSpeedPropName(TEXT("MaxSpeed"));

static bool ReadCarMaxSpeed(const AActor* Car, double& Out)
{
	if (!Car)
	{
		return false;
	}
	if (const FProperty* Prop = Car->GetClass()->FindPropertyByName(MaxSpeedPropName))
	{
		if (const FDoubleProperty* D = CastField<FDoubleProperty>(Prop))
		{
			Out = D->GetPropertyValue_InContainer(Car);
			return true;
		}
		if (const FFloatProperty* F = CastField<FFloatProperty>(Prop))
		{
			Out = F->GetPropertyValue_InContainer(Car);
			return true;
		}
	}
	return false;
}

static bool WriteCarMaxSpeed(AActor* Car, double Value)
{
	if (!Car)
	{
		return false;
	}
	if (FProperty* Prop = Car->GetClass()->FindPropertyByName(MaxSpeedPropName))
	{
		if (FDoubleProperty* D = CastField<FDoubleProperty>(Prop))
		{
			D->SetPropertyValue_InContainer(Car, Value);
			return true;
		}
		if (FFloatProperty* F = CastField<FFloatProperty>(Prop))
		{
			F->SetPropertyValue_InContainer(Car, static_cast<float>(Value));
			return true;
		}
	}
	return false;
}

UDeliveryTrafficCarComponent::UDeliveryTrafficCarComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

// 准备道路路线、车辆初始位置和碰撞检测
void UDeliveryTrafficCarComponent::BeginPlay()
{
	Super::BeginPlay();
	// Camera 弹簧臂仍会避开墙/地形，但车身不再把镜头瞬间推到角色脸前
	if (AActor* Owner = GetOwner())
	{
		// 已摆在地图里的旧车实例序列化过 bReplicates=假，覆盖了蓝图新默认值
		// 在权威端运行时注册复制，不改地图资产也能让这些实例发出位置快照
		if (Owner->HasAuthority())
		{
			Owner->SetReplicates(true);
		}
		// 客户端地图实例也有旧的 bReplicateMovement=假 覆盖
		// OnRep_ReplicatedMovement 遇到 假 会直接丢弃服务器位置
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
		// 记录出生位置和原始速度上限，重置车辆时恢复
		
		SecondWaveTransform = Owner->GetActorTransform();
	}

if (const AActor* Owner = GetOwner())
	{
		double Speed = 0.0;
		if (ReadCarMaxSpeed(Owner, Speed))
		{
			OriginalMaxSpeed = Speed;
			bHasOriginalMaxSpeed = true;
		}
	}

	// 保存原始缩放，再从较小尺寸逐渐恢复，各波车辆共用此流程
	
	if (bScaleInOnSpawn && ScaleInDuration > 0.0f)
	{
		if (AActor* Owner = GetOwner())
		{
			ScaleInTargetScale = Owner->GetActorScale3D();
			ScaleInElapsed = 0.0f;
			ScaleInTicks = 0;
			bScaleInRunning = true;
			Owner->SetActorScale3D(ScaleInTargetScale * ScaleInStartRatio);
		}
	}

	// 分波生成只在权威端跑；客户端靠复制拿到新车，两边各生一辆会变成双份
	if (bSpawnExtraWaves && GetOwner() && GetOwner()->HasAuthority())
	{
		const int32 Wave = GetWaveIndex();
		// 第 0 波（关卡里摆好的）生第 1 波，第 1 波生第 2 波，第 2 波到此为止
		// 总共三波；少了这道闸就是指数增长
		// 旧蓝图组件/关卡实例可能把先前默认的 10 秒序列化成覆盖值
		// 将这个旧值迁到 15 秒，其他用户自定义延迟仍保持原样
		const float EffectiveSecondWaveDelay =
		    FMath::IsNearlyEqual(SecondWaveDelay, 10.0f) ? 15.0f : SecondWaveDelay;
		// 同理兼容旧蓝图里序列化的第三波 15 秒间隔
		const float EffectiveThirdWaveDelay =
		    FMath::IsNearlyEqual(ThirdWaveDelay, 15.0f) ? 30.0f : ThirdWaveDelay;
		const float Delay = (Wave == 0)   ? EffectiveSecondWaveDelay
		                    : (Wave == 1) ? EffectiveThirdWaveDelay
		                                  : -1.0f;
		if (Delay >= 0.0f)
		{
			if (Delay <= 0.0f)
			{
				SpawnNextWave();
			}
			else if (UWorld* World = GetWorld())
			{
				World->GetTimerManager().SetTimer(NextWaveTimer, this,
				                                  &UDeliveryTrafficCarComponent::SpawnNextWave, Delay, false);
			}
		}
	}
}

/** 波次标签形如 TrafficWave1、TrafficWave2；没有标签就是第 0 波 */
static const TCHAR* WaveTagPrefix = TEXT("TrafficWave");

// 根据车辆所属分组取得生成批次
int32 UDeliveryTrafficCarComponent::GetWaveIndex() const
{
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return 0;
	}
	for (const FName& Tag : Owner->Tags)
	{
		const FString TagStr = Tag.ToString();
		if (TagStr.StartsWith(WaveTagPrefix))
		{
			return FCString::Atoi(*TagStr.RightChop(FCString::Strlen(WaveTagPrefix)));
		}
	}
	return 0;
}

// 更新车辆出现或离开时的缩放表现
void UDeliveryTrafficCarComponent::UpdateScaleAnim(float RawDeltaTime)
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		bScaleInRunning = false;
		bScaleOutRunning = false;
		return;
	}

	// 限制每帧缩放进度，避免启动卡顿使出现效果在一帧内结束
	
	
	const float DeltaTime = FMath::Min(RawDeltaTime, MaxScaleAnimStep);

	// --- 缩小消失 ---
	if (bScaleOutRunning)
	{
		ScaleOutElapsed += DeltaTime;
		const float A = FMath::Clamp(ScaleOutElapsed / ScaleOutDuration, 0.0f, 1.0f);
		// 缓入：开头慢、收尾快，和出现时的缓出正好相反，看着像被"吸走"
		const float Eased = FMath::Square(A);
		OwnerActor->SetActorScale3D(ScaleInTargetScale * FMath::Lerp(1.0f, ScaleInStartRatio, Eased));
		if (A >= 1.0f)
		{
			bScaleOutRunning = false;
			bRouteLostPending = false;
			// 缩小完成后再通知蓝图重置位置，避免在出生点才播放消失效果
			
			OnRouteLost.Broadcast();
		}
		return;
	}

	if (!bScaleInRunning)
	{
		return;
	}
	AActor* Owner = OwnerActor;

	ScaleInElapsed += DeltaTime;
	const float Alpha = FMath::Clamp(ScaleInElapsed / ScaleInDuration, 0.0f, 1.0f);
	// 放大过程先快后慢，接近目标尺寸时平滑收尾
	
	const float Eased = 1.0f - FMath::Square(1.0f - Alpha);
	const float Ratio = FMath::Lerp(ScaleInStartRatio, 1.0f, Eased);
	Owner->SetActorScale3D(ScaleInTargetScale * Ratio);

	++ScaleInTicks;
	if (Alpha >= 1.0f)
	{
		// 结束时恢复精确的原始缩放，避免保留插值误差
		
		Owner->SetActorScale3D(ScaleInTargetScale);
		bScaleInRunning = false;
		// 记录效果经过的帧数，便于检查是否因卡顿而在单帧结束
		
		UE_LOG(LogDelivery, Log, TEXT("%s: 缩放淡入完成 wave=%d ticks=%d"), *GetNameSafe(Owner),
		       GetWaveIndex(), ScaleInTicks);
	}
}

// 按批次创建后续交通车辆
void UDeliveryTrafficCarComponent::SpawnNextWave()
{
	AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!Owner || !World || !Owner->HasAuthority())
	{
		return;
	}

	// 第一波原车也可能重新绕回出生点，因此包括 Owner 在内检查全部交通车
	// 只看水平距离和有限高度差，避免高架上下层互相等待
	const FVector SpawnLocation = SecondWaveTransform.GetLocation();
	AActor* BlockingCar = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Other = *It;
		if (!IsValid(Other) || Other->IsActorBeingDestroyed() ||
		    !Other->FindComponentByClass<UDeliveryTrafficCarComponent>())
		{
			continue;
		}
		const FVector OtherLocation = Other->GetActorLocation();
		if (FMath::Abs(OtherLocation.Z - SpawnLocation.Z) <= WaveSpawnHeightTolerance &&
		    FVector::DistSquared2D(OtherLocation, SpawnLocation) < FMath::Square(WaveSpawnClearance))
		{
			BlockingCar = Other;
			break;
		}
	}
	if (BlockingCar)
	{
		if (!bWaveSpawnWasBlocked)
		{
			UE_LOG(LogDelivery, Log, TEXT("%s: wave=%d 出生点被 %s 占用，每 %.1f 秒重试"),
			       *GetNameSafe(Owner), GetWaveIndex() + 1, *GetNameSafe(BlockingCar), WaveSpawnRetryDelay);
			bWaveSpawnWasBlocked = true;
		}
		World->GetTimerManager().SetTimer(NextWaveTimer, this, &UDeliveryTrafficCarComponent::SpawnNextWave,
		                                  FMath::Max(0.1f, WaveSpawnRetryDelay), false);
		return;
	}
	if (bWaveSpawnWasBlocked)
	{
		UE_LOG(LogDelivery, Log, TEXT("%s: wave=%d 出生点已空，继续生成"), *GetNameSafe(Owner),
		       GetWaveIndex() + 1);
		bWaveSpawnWasBlocked = false;
	}

	FActorSpawnParameters Params;
	Params.Owner = Owner->GetOwner();
	Params.Instigator = Owner->GetInstigator();

	// 延迟完成生成，先设置波次和参数，再让新车开始运行

AActor* Clone =
	    World->SpawnActorDeferred<AActor>(Owner->GetClass(), SecondWaveTransform, Params.Owner,
		                                  Params.Instigator, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Clone)
	{
		return;
	}

	// 复制原车实例上的速度设置，仅在双方属性类型和大小一致时写入
	
	
	static const FName MaxSpeedName(TEXT("MaxSpeed"));
	if (FProperty* Src = Owner->GetClass()->FindPropertyByName(MaxSpeedName))
	{
		if (FProperty* Dst = Clone->GetClass()->FindPropertyByName(MaxSpeedName))
		{
			if (Src->GetClass() == Dst->GetClass() && Src->GetSize() == Dst->GetSize())
			{
				Src->CopyCompleteValue(Dst->ContainerPtrToValuePtr<void>(Clone),
				                       Src->ContainerPtrToValuePtr<void>(Owner));
			}
		}
	}

	// 波次写入对象标签，因为此时蓝图组件尚未创建

Clone->Tags.Add(FName(*FString::Printf(TEXT("%s%d"), WaveTagPrefix, GetWaveIndex() + 1)));

	Clone->FinishSpawning(SecondWaveTransform);
}

// 推进车辆行驶、路线恢复和碰撞处理
void UDeliveryTrafficCarComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                                 FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	// 服务器和客户端都更新缩放效果，移动同步不会替它同步缩放
	
	
	UpdateScaleAnim(DeltaTime);
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}
	ProcessVehicleImpacts(DeltaTime);

	if (!bIsCurrentlyOnRoute && bHasReachedMaxSpeed)
	{
		// 达到正常速度后才累计路线丢失时间，减速或重置时清零
		
		RouteLostElapsedTime += DeltaTime;
		if (RouteLostElapsedTime >= RouteLostTimeout && !bRouteLostPending)
		{
			RouteLostElapsedTime = 0.0f;
			BeginRouteReset();
		}
	}
	else if (!bHasReachedMaxSpeed)
	{
		
		
		RouteLostElapsedTime = 0.0f;
	}

	// 单独检查长时间停滞，红灯排队使用较长时限
	
	
	if (FMath::Abs(LastKnownSpeed) <= StallSpeedThreshold)
	{
		const bool bWaitingForLight = bStoppedAtIntersection || bLeaderStoppedAtIntersection;
		const float Limit = bWaitingForLight ? StalledTimeoutAtIntersection : StalledTimeout;
		StalledElapsedTime += DeltaTime;
		if (Limit > 0.0f && StalledElapsedTime >= Limit && !bRouteLostPending)
		{
			UE_LOG(LogDelivery, Log, TEXT("%s：车速为 0 已 %.1f 秒（阈值 %.1f%s），回溯到出生点"),
			       *GetNameSafe(GetOwner()), StalledElapsedTime, Limit,
			       bWaitingForLight ? TEXT("，在等红灯") : TEXT(""));
			StalledElapsedTime = 0.0f;
			BeginRouteReset();
		}
	}
	else
	{
		StalledElapsedTime = 0.0f;
	}

	// 红灯期间前车是合法地长时间静止，不是死锁，用更宽松的超时，避免后车提前放弃避让压上去
	
	// 根据上一帧的前车红灯状态选择超时，再刷新本帧探测结果

const bool bAnyoneAtLight = bStoppedAtIntersection || bLeaderStoppedAtIntersection;
	const float EffectiveMaxBlockedTime = bAnyoneAtLight ? MaxBlockedTimeAtIntersection : MaxBlockedTime;

	const float DistanceAhead = GetDistanceToCarAhead();

	// 随前车距离逐渐降低目标速度，到达最小跟车距离时目标速度为零
	
	
	float Target = 1.0f;
	if (DistanceAhead >= 0.0f)
	{
		const float Span = FMath::Max(ForwardTraceDistance - MinFollowDistance, 1.0f);
		Target = FMath::Clamp((DistanceAhead - MinFollowDistance) / Span, 0.0f, 1.0f);
	}

	// 只有接近停止才累计阻塞时间，正常缓慢跟车不触发强制放行
	
	if (Target <= KINDA_SMALL_NUMBER)
	{
		BlockedElapsedTime += DeltaTime;
		if (BlockedElapsedTime >= EffectiveMaxBlockedTime)
		{
			// 汇入口两辆车互为"前车"时会一直卡住，卡够久就放弃避让强行通过，打破死锁
			Target = 1.0f;
		}
	}
	else
	{
		BlockedElapsedTime = 0.0f;
	}

	// 减速立即生效，恢复加速需等待，避免前车在探测边缘反复出现时频繁加减速
	
	
	if (Target < SpeedMultiplier - KINDA_SMALL_NUMBER)
	{
		SpeedMultiplier =
		    FMath::FInterpConstantTo(SpeedMultiplier, Target, DeltaTime, SpeedMultiplierChangeRate);
		FollowAccelerationPauseRemaining = FollowAccelerationPause;
	}
	else
	{
		const float PreviousPause = FollowAccelerationPauseRemaining;
		FollowAccelerationPauseRemaining = FMath::Max(0.0f, FollowAccelerationPauseRemaining - DeltaTime);
		if (PreviousPause <= 0.0f && Target > SpeedMultiplier + KINDA_SMALL_NUMBER)
		{
			SpeedMultiplier =
			    FMath::FInterpConstantTo(SpeedMultiplier, Target, DeltaTime, SpeedMultiplierChangeRate);
		}
	}

	DebugSpeedMultiplier = SpeedMultiplier;
	DebugDistanceAhead = DistanceAhead;
	DebugBlockedElapsed = BlockedElapsedTime;
	DebugAccelerationPauseRemaining = FollowAccelerationPauseRemaining;
}

// 记录车辆是否正在正常跟随道路
void UDeliveryTrafficCarComponent::UpdateRouteFollowState(bool bIsFollowingRoute)
{
	bIsCurrentlyOnRoute = bIsFollowingRoute;
	if (bIsFollowingRoute)
	{
		RouteLostElapsedTime = 0.0f;
	}
}

// 开始将离开路线的车辆恢复到道路
void UDeliveryTrafficCarComponent::BeginRouteReset()
{
	// 先播放缩小效果，再通知蓝图重置；等待期间阻止重复发出重置通知

if (bScaleOutOnRouteLost && ScaleOutDuration > 0.0f && GetOwner())
	{
		bRouteLostPending = true;
		bScaleOutRunning = true;
		bScaleInRunning = false;
		ScaleOutElapsed = 0.0f;
	}
	else
	{
		OnRouteLost.Broadcast();
	}
}

// 记录路线计算使用的当前车速
void UDeliveryTrafficCarComponent::UpdateSpeedState(float InCurrentSpeed, float InMaxSpeed)
{
	// MaxSpeed 是蓝图里的纯配置量，正常恒为正；防一手除零/负值配置
	bHasReachedMaxSpeed = (InMaxSpeed > 0.0f) && (InCurrentSpeed >= InMaxSpeed - KINDA_SMALL_NUMBER);
	// 停滞检测要用绝对车速；组件自己不接管移动，只能靠蓝图每帧报上来
	LastKnownSpeed = InCurrentSpeed;
}

void UDeliveryTrafficCarComponent::UpdateStoppedAtIntersection(bool bInStoppedAtIntersection)
{
	bStoppedAtIntersection = bInStoppedAtIntersection;
}

// 车辆回到路线起点时重置运动和碰撞记录
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
	// 清除停滞计时，避免重置后继续使用旧时长而再次重置
	
	
	StalledElapsedTime = 0.0f;
	LastKnownSpeed = 0.0f;

	// 恢复原始速度上限，清除跟车过程中产生的调整
	
	if (bHasOriginalMaxSpeed)
	{
		WriteCarMaxSpeed(GetOwner(), OriginalMaxSpeed);
	}

	// 缩出已经放完（或者根本没开），状态一律清掉，免得重生后还在缩
	bScaleOutRunning = false;
	bRouteLostPending = false;

	// 重置后重新播放放大效果，目标始终使用最初保存的完整尺寸
	
	if (bScaleInOnSpawn && ScaleInDuration > 0.0f)
	{
		if (AActor* Owner = GetOwner())
		{
			
			
			ScaleInElapsed = 0.0f;
			ScaleInTicks = 0;
			bScaleInRunning = true;
			Owner->SetActorScale3D(ScaleInTargetScale * ScaleInStartRatio);
		}
	}

	// 清除避让系数和阻塞计时，使车辆回到刚生成时的状态
	
	
	SpeedMultiplier = 1.0f;
	FollowAccelerationPauseRemaining = 0.0f;
	BlockedElapsedTime = 0.0f;

	// 清除红灯记录，并等待车辆重新达到正常速度后再检测路线丢失
	
	bHasReachedMaxSpeed = false;
	bStoppedAtIntersection = false;
	bLeaderStoppedAtIntersection = false;

	DebugSpeedMultiplier = 1.0f;
	DebugDistanceAhead = -1.0f;
	DebugBlockedElapsed = 0.0f;
	DebugAccelerationPauseRemaining = 0.0f;
}

// 检查交通车辆对角色或其他物体的撞击
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
		// 路线循环传送不能沿整张地图扫出一次虚假的撞击
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
	// 一次检测可能碰到同一角色的多个身体部位，每个角色只处理一次
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
		// 同一角色再次受撞击前需要经过冷却，避免持续接触时反复扣血
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
		// 用车辆相对角色的接近速度计算撞击强度，同向移动时撞击会减弱
		const float ClosingSpeed = FVector::DotProduct(CarVelocity - BodyVelocity, Direction);
		const float DeltaV = FMath::Max(0.0f, ClosingSpeed) * VehicleEffectiveMassKg /
		                     FMath::Max(VehicleEffectiveMassKg + CharacterEffectiveMassKg, 1.0f);
		if (DeltaV < MinimumDamageDeltaV || ASC->HasMatchingGameplayTag(TAG_State_Stunned))
		{
			continue;
		}
		const float Health = ASC->GetNumericAttribute(UDeliverAttributeSet::GetHealthAttribute());
		const bool bStrongHit = DeltaV >= KnockdownDeltaV;
		// 先取地面状态；后面的 HP=0 会立即把角色切进 Limp
		const bool bTakeoff = bStrongHit && Ragdoll->IsGrounded();
		const float Damage = bStrongHit ? Health
		                                : FMath::Clamp((DeltaV - MinimumDamageDeltaV) * DamagePerDeltaV, 0.0f,
		                                               MaximumLightDamage);
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
		// GE 是既有伤害路径，但在满血回复效果仍激活时可能被其它 GE 修饰抵消
		// 服务器检查结算后的值，仅在未达到这次应扣 HP 时补足，避免强撞偶发不晕倒
		const float ExpectedHealth = FMath::Max(0.0f, Health - Damage);
		if (ASC->GetNumericAttribute(HealthAttribute) > ExpectedHealth + 0.5f)
		{
			ASC->SetNumericAttributeBase(HealthAttribute, ExpectedHealth);
		}
		const FVector HorizontalVelocityChange = Direction * FMath::Min(DeltaV, MaximumKnockbackDeltaV);
		float TakeoffDeltaV = 0.0f;
		if (bTakeoff)
		{
			// 一次性补到目标向上速度，已有上升速度不叠加；弱撞及空中再撞不追加升力
			// 只推髋部会被整条受约束刚体链分摊得几乎看不出升高，竖直分量须给全身刚体
			TakeoffDeltaV = FMath::Max(0.0f, StrongHitTakeoffSpeed - BodyVelocity.Z);
		}
		// 整条刚体链获得同一速度变化；只推动髋部会在约束两端瞬间制造
		// 数百 cm/s 的相对速度，强撞时可能把四肢拧进极端姿态
		Mesh->AddImpulseToAllBodiesBelow(HorizontalVelocityChange + FVector::UpVector * TakeoffDeltaV,
		                                 TEXT("Hips"), true, true);
		Character->NotifyVehicleImpact();
		UE_LOG(
		    LogTemp, Log,
		    TEXT(
		        "DeliveryVehicleImpact car=%s target=%s deltaV=%.1f launchZ=%.1f damage=%.1f healthAfter=%.1f"),
		    *GetNameSafe(Owner), *GetNameSafe(Character), DeltaV, TakeoffDeltaV, Damage,
		    ASC->GetNumericAttribute(UDeliverAttributeSet::GetHealthAttribute()));
		LastImpactTimeByActor.Add(Character, Now);
	}

	ProcessMotorbikeImpacts(Start, End, Current.GetRotation(), CarVelocity, Params);

	PreviousImpactTransform = Current;
}

// 处理交通车辆与玩家摩托车之间的撞击
void UDeliveryTrafficCarComponent::ProcessMotorbikeImpacts(const FVector& Start, const FVector& End,
                                                           const FQuat& Rotation, const FVector& CarVelocity,
                                                           const FCollisionQueryParams& Params)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 摩托车使用不同的碰撞类型，单独扫描，避免将角色胶囊重复计入身体撞击
	
	
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

		// 同一目标连续重叠的多帧共用撞击冷却，避免一次碰撞被累计多次
		
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

// 寻找前方同路线车辆，提供跟车距离
float UDeliveryTrafficCarComponent::GetDistanceToCarAhead()
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
	GetWorld()->SweepMultiByChannel(Hits, Start, End, FQuat::Identity, TraceChannel.GetValue(),
	                                FCollisionShape::MakeSphere(ForwardTraceRadius), QueryParams);

	if (bDrawDebugTrace)
	{
		DrawDebugCapsule(GetWorld(), (Start + End) * 0.5f, (End - Start).Size() * 0.5f, ForwardTraceRadius,
		                 FRotationMatrix::MakeFromZ(Forward).ToQuat(), FColor::Yellow, false, -1.0f, 0, 1.5f);
	}

	// 取最近的一辆，多辆车同时在扫描范围内时应该跟最前面那个的距离减速
	float Nearest = -1.0f;
	AActor* NearestCar = nullptr;
	UDeliveryTrafficCarComponent* NearestComp = nullptr;
	for (const FHitResult& Hit : Hits)
	{
		AActor* HitActor = Hit.GetActor();
		UDeliveryTrafficCarComponent* HitComp =
		    HitActor ? HitActor->FindComponentByClass<UDeliveryTrafficCarComponent>() : nullptr;
		if (HitActor && HitActor != Owner && HitComp)
		{
			// 起始就重叠时 Distance 是 0，当成贴脸处理（0 会让 Target 直接算成 0，正是想要的）
			const float D = Hit.bStartPenetrating ? 0.0f : Hit.Distance;
			if (Nearest < 0.0f || D < Nearest)
			{
				Nearest = D;
				NearestCar = HitActor;
				NearestComp = HitComp;
			}
		}
	}

	// 保存前车红灯状态供阻塞判断，未发现前车时清除旧状态
	
	bLeaderStoppedAtIntersection = NearestComp && NearestComp->IsStoppedAtIntersection();

	// 前车较慢时提高其速度上限，并适当降低本车上限，使双方速度接近

// 这个函数里的 Owner 是 const AActor*（只用来读位置），写属性要非 const 的自己
	// 只在形成近距离跟车时协调速度，不调整远处车辆
	
	if (AActor* Self = GetOwner();
	    bPushSlowLeader && NearestCar && Self && Nearest >= 0.0f && Nearest <= PushSlowLeaderMaxDistance)
	{
		double MySpeed = 0.0;
		double LeadSpeed = 0.0;
		if (ReadCarMaxSpeed(Self, MySpeed) && ReadCarMaxSpeed(NearestCar, LeadSpeed) && LeadSpeed < MySpeed)
		{
			WriteCarMaxSpeed(NearestCar, MySpeed);
			WriteCarMaxSpeed(Self, FMath::Max(MySpeed - PushSlowLeaderSelfPenalty,
			                                  static_cast<double>(MinPushedMaxSpeed)));
		}
	}

	return Nearest;
}
