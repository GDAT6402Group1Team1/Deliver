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
#include "TimerManager.h"

/**
 * MaxSpeed 住在蓝图里（BP_car_base 的成员变量），C++ 侧没有对应的类型可以直接引用，
 * 只能走反射按名字读写。不写死 FFloatProperty：BP 里的 float 在 UE5 编译成 double，
 * 写死单精度会静默匹配不上、读出来永远是 0。
 */
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
		// 出生变换要在这里就记下来：等计时器到点时车早就开走了，
		// 那时候再取拿到的是半路上的位置，不是出生点。
		SecondWaveTransform = Owner->GetActorTransform();
	}

	// 记下出厂的 MaxSpeed。后面"顶慢车"会改这个值，裸奔重生时要能还原——
	// 不存一份的话重生后车速会停在被改过的状态上，越跑越偏离最初的配置。
	if (const AActor* Owner = GetOwner())
	{
		double Speed = 0.0;
		if (ReadCarMaxSpeed(Owner, Speed))
		{
			OriginalMaxSpeed = Speed;
			bHasOriginalMaxSpeed = true;
		}
	}

	// 缩放淡入：记下本来的缩放，先缩到很小，再由 Tick 推回去。
	// 第一波和第二波共用这条路——克隆体的 BeginPlay 同样会跑到这里。
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

	// 分波生成只在权威端跑；客户端靠复制拿到新车，两边各生一辆会变成双份。
	if (bSpawnExtraWaves && GetOwner() && GetOwner()->HasAuthority())
	{
		const int32 Wave = GetWaveIndex();
		// 第 0 波（关卡里摆好的）生第 1 波，第 1 波生第 2 波，第 2 波到此为止。
		// 总共三波。少了这道闸就是指数增长。
		const float Delay = (Wave == 0) ? SecondWaveDelay
						  : (Wave == 1) ? ThirdWaveDelay
										: -1.0f;
		if (Delay >= 0.0f)
		{
			if (Delay <= 0.0f)
			{
				SpawnNextWave();
			}
			else if (UWorld* World = GetWorld())
			{
				World->GetTimerManager().SetTimer(
					NextWaveTimer, this,
					&UDeliveryTrafficCarComponent::SpawnNextWave,
					Delay, false);
			}
		}
	}
}

/** 波次标签形如 TrafficWave1、TrafficWave2；没有标签就是第 0 波。 */
static const TCHAR* WaveTagPrefix = TEXT("TrafficWave");

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

void UDeliveryTrafficCarComponent::UpdateScaleAnim(float RawDeltaTime)
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		bScaleInRunning = false;
		bScaleOutRunning = false;
		return;
	}

	// 钳住单帧步长。关卡启动那一两帧动辄卡几百毫秒，不钳的话 0.5 秒的淡入
	// 在第一个 tick 就走完了，第一波车等于没有淡入（第二三波十几秒后生成、
	// 那时帧率已稳，所以只有它们看得见）。钳完之后淡入一定要经过约 10 个 tick。
	const float DeltaTime = FMath::Min(RawDeltaTime, MaxScaleAnimStep);

	// --- 缩小消失 ---
	if (bScaleOutRunning)
	{
		ScaleOutElapsed += DeltaTime;
		const float A = FMath::Clamp(ScaleOutElapsed / ScaleOutDuration, 0.0f, 1.0f);
		// 缓入：开头慢、收尾快，和出现时的缓出正好相反，看着像被"吸走"
		const float Eased = FMath::Square(A);
		OwnerActor->SetActorScale3D(
			ScaleInTargetScale * FMath::Lerp(1.0f, ScaleInStartRatio, Eased));
		if (A >= 1.0f)
		{
			bScaleOutRunning = false;
			bRouteLostPending = false;
			// 缩到最小了才真正通知蓝图去瞬移。顺序反过来的话车会先闪到出生点
			// 再在那儿缩小，看着像"在终点重生了一次又消失"。
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
	// 缓出：起步快、收尾慢，比线性更像"弹出来"，而且最后那几帧尺寸变化小，
	// 不会在快到位时还有肉眼可见的跳动。
	const float Eased = 1.0f - FMath::Square(1.0f - Alpha);
	const float Ratio = FMath::Lerp(ScaleInStartRatio, 1.0f, Eased);
	Owner->SetActorScale3D(ScaleInTargetScale * Ratio);

	++ScaleInTicks;
	if (Alpha >= 1.0f)
	{
		// 结尾精确设回目标值，不留插值误差——否则每辆车的最终缩放会差一点点，
		// 而这些车的缩放是按车型定死的（0.5 / 0.7 / 0.8），差一点就看得出来。
		Owner->SetActorScale3D(ScaleInTargetScale);
		bScaleInRunning = false;
		// ticks=1 就说明这一次淡入被单帧吃掉了（开局卡顿），这是加 MaxScaleAnimStep 的理由；
		// 正常应当是 10 个 tick 上下。留着这行日志，以后再有人说"没看到淡入"能直接查。
		UE_LOG(LogDelivery, Log, TEXT("%s: 缩放淡入完成 wave=%d ticks=%d"),
			*GetNameSafe(Owner), GetWaveIndex(), ScaleInTicks);
	}
}

void UDeliveryTrafficCarComponent::SpawnNextWave()
{
	AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!Owner || !World)
	{
		return;
	}

	// 出生点此刻多半已经有别的车压着（原车刚起步没走远，或者后面排着的车），
	// 按默认规则会因为碰撞而放弃生成，而"第二波少了几辆"很难发现。
	// 交通车是运动学的（bSimulatePhysics=false，靠 SetActorLocation 沿样条走），
	// 短暂重叠不会把谁弹飞，所以明确要求照放不误。
	FActorSpawnParameters Params;
	Params.Owner = Owner->GetOwner();
	Params.Instigator = Owner->GetInstigator();

	// 必须用延迟生成：SpawnActor 返回时克隆体的 BeginPlay 已经跑完了，
	// 那时候再打"我是克隆体"的标记已经晚了——它自己也会挂上第二波计时器，
	// 每 5 秒翻一倍，一分钟就是 4096 辆。SpawnActorDeferred 把 BeginPlay
	// 推迟到 FinishSpawning，中间这段正好用来打标记和搬参数。
	AActor* Clone = World->SpawnActorDeferred<AActor>(
		Owner->GetClass(), SecondWaveTransform, Params.Owner, Params.Instigator,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Clone)
	{
		return;
	}

	// 实例上改过的 MaxSpeed 是**实例覆盖**，新生成的是类默认值，不搬就全变成一样快。
	// 走反射按名字搬，不写死类型：BP 里的 float 在 UE5 编译成 double，
	// 写死 FFloatProperty 会静默搬不动。比对两边的属性类和大小，一致才搬。
	static const FName MaxSpeedName(TEXT("MaxSpeed"));
	if (FProperty* Src = Owner->GetClass()->FindPropertyByName(MaxSpeedName))
	{
		if (FProperty* Dst = Clone->GetClass()->FindPropertyByName(MaxSpeedName))
		{
			if (Src->GetClass() == Dst->GetClass() && Src->GetSize() == Dst->GetSize())
			{
				Src->CopyCompleteValue(
					Dst->ContainerPtrToValuePtr<void>(Clone),
					Src->ContainerPtrToValuePtr<void>(Owner));
			}
		}
	}

	// 给新车打上波次标签。**不能**改用 FindComponentByClass 去设组件上的成员：
	// 延迟生成的 actor 这时候还没有蓝图组件（SCS 组件要到 FinishSpawning 里
	// 执行构造脚本才创建），那样拿到的是 null，标记打不上、每辆都以为自己是
	// 第一波，于是每 N 秒翻一倍——实测就是这么炸的。
	// 标签在 actor 自己身上，现在就能设，BeginPlay 里读得到。
	Clone->Tags.Add(FName(*FString::Printf(TEXT("%s%d"), WaveTagPrefix, GetWaveIndex() + 1)));

	Clone->FinishSpawning(SecondWaveTransform);
}

void UDeliveryTrafficCarComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	// 缩放淡入要在权威判断**之前**跑：FRepMovement 只复制位置和旋转、不带缩放，
	// 放在权威分支里的话客户端看到的车一直是 0.01 倍那么小。
	// 这段纯表现、无副作用，两端各算各的，结果一致。
	UpdateScaleAnim(DeltaTime);
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
		if (RouteLostElapsedTime >= RouteLostTimeout && !bRouteLostPending)
		{
			RouteLostElapsedTime = 0.0f;
			// 先缩小再消失。缩完由 UpdateScaleAnim 广播 OnRouteLost，
			// 蓝图那边照旧瞬移回出生点 + 调 NotifyResetToStart，那里再放大回来。
			// bRouteLostPending 是必须的：缩小这零点几秒里车还在裸奔，
			// 计时会继续累加、再次触发超时，没有这道闸会广播好几次。
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
	}
	else if (!bHasReachedMaxSpeed)
	{
		// 掉速了（红灯刹车、避让减速、或者刚被重置）就把计时清零，
		// 保证下次是从"速度重新回到 MaxSpeed 的那一刻"重新开始算满 RouteLostTimeout。
		RouteLostElapsedTime = 0.0f;
	}

	// 红灯期间前车是合法地长时间静止，不是死锁，用更宽松的超时，避免后车提前放弃避让压上去。
	//
	// 注意顺序：bLeaderStoppedAtIntersection 是上一帧扫描留下的值，所以必须先算
	// EffectiveMaxBlockedTime 再调 GetDistanceToCarAhead()——反过来写就变成"这一帧的
	// 前车状态配这一帧的超时"，读起来像是对的，实际上让第一帧的判断依赖尚未初始化的值。
	// 差一帧对 3~10 秒量级的超时没有影响。
	const bool bAnyoneAtLight = bStoppedAtIntersection || bLeaderStoppedAtIntersection;
	const float EffectiveMaxBlockedTime = bAnyoneAtLight ? MaxBlockedTimeAtIntersection : MaxBlockedTime;

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

	// 还原被"顶慢车"改过的 MaxSpeed。要求是重生后所有参数都和刚 BeginPlay 一致，
	// 车速也在其中——不还原的话它会一直带着路上被顶上去/压下来的值。
	if (bHasOriginalMaxSpeed)
	{
		WriteCarMaxSpeed(GetOwner(), OriginalMaxSpeed);
	}

	// 缩出已经放完（或者根本没开），状态一律清掉，免得重生后还在缩
	bScaleOutRunning = false;
	bRouteLostPending = false;

	// 重生也走一遍缩放淡入：瞬移回出生点是"重新出现"，和第二三波出现是同一件事，
	// 突然在原地冒出一辆整车比较突兀。
	if (bScaleInOnSpawn && ScaleInDuration > 0.0f)
	{
		if (AActor* Owner = GetOwner())
		{
			// 目标缩放用 BeginPlay 记的那份，不要用当前值——万一上一次淡入没走完
			// 就触发了重生，当前值是缩到一半的尺寸，拿它当目标会越缩越小。
			ScaleInElapsed = 0.0f;
			ScaleInTicks = 0;
			bScaleInRunning = true;
			Owner->SetActorScale3D(ScaleInTargetScale * ScaleInStartRatio);
		}
	}

	// 重置要做到和刚 BeginPlay 时完全一样：避让系数、卡车计时都得跟着归零，
	// 不然车瞬移回出生点后可能还带着重置前"正在让路"的残留状态（比如速度系数
	// 还没插值回 1、卡住计时还没清），跟真正刚出生的车表现不一致。
	SpeedMultiplier = 1.0f;
	BlockedElapsedTime = 0.0f;

	// 车刚被瞬移回出生点，速度也被蓝图归零了，这里跟着回到"还没跑起来"的状态：
	// 裸奔计时要等它重新加速到 MaxSpeed 才开始，红灯标记也不能留着重置前的判断结果。
	bHasReachedMaxSpeed = false;
	bStoppedAtIntersection = false;
	bLeaderStoppedAtIntersection = false;

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
	GetWorld()->SweepMultiByChannel(Hits, Start, End, FQuat::Identity, TraceChannel.GetValue(), FCollisionShape::MakeSphere(ForwardTraceRadius), QueryParams);

	if (bDrawDebugTrace)
	{
		DrawDebugCapsule(GetWorld(), (Start + End) * 0.5f, (End - Start).Size() * 0.5f, ForwardTraceRadius, FRotationMatrix::MakeFromZ(Forward).ToQuat(), FColor::Yellow, false, -1.0f, 0, 1.5f);
	}

	// 取最近的一辆，多辆车同时在扫描范围内时应该跟最前面那个的距离减速。
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
			// 起始就重叠时 Distance 是 0，当成贴脸处理（0 会让 Target 直接算成 0，正是想要的）。
			const float D = Hit.bStartPenetrating ? 0.0f : Hit.Distance;
			if (Nearest < 0.0f || D < Nearest)
			{
				Nearest = D;
				NearestCar = HitActor;
				NearestComp = HitComp;
			}
		}
	}

	// 顺手把前车的红灯状态记下来，死锁超时要用。扫不到车时清成 false，
	// 否则前车开走之后这辆车还会一直以为"前面在等红灯"而拿着宽松超时。
	bLeaderStoppedAtIntersection = NearestComp && NearestComp->IsStoppedAtIntersection();

	// 前车比自己慢的话，把它提到自己的速度、自己再降 100：两边各让一步，
	// 后车不用一路跟着爬，前车也不至于被一脚顶到很快。
	// 只调整一次就收敛——前车提到的是自己**降之前**的值，比降完的自己快 100，
	// 下一帧条件不再成立，不会来回拉扯。
	// 这个函数里的 Owner 是 const AActor*（只用来读位置），写属性要非 const 的自己
	// Nearest >= 0 说明确实扫到了车；再卡一道距离门槛，
	// 老远看见一辆慢车不算跟车（那时候避让都还没开始减速）。
	if (AActor* Self = GetOwner();
		bPushSlowLeader && NearestCar && Self
		&& Nearest >= 0.0f && Nearest <= PushSlowLeaderMaxDistance)
	{
		double MySpeed = 0.0;
		double LeadSpeed = 0.0;
		if (ReadCarMaxSpeed(Self, MySpeed) && ReadCarMaxSpeed(NearestCar, LeadSpeed)
			&& LeadSpeed < MySpeed)
		{
			WriteCarMaxSpeed(NearestCar, MySpeed);
			WriteCarMaxSpeed(Self, FMath::Max(MySpeed - PushSlowLeaderSelfPenalty,
											  static_cast<double>(MinPushedMaxSpeed)));
		}
	}

	return Nearest;
}
