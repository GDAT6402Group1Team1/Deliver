// Copyright Epic Games, Inc. All Rights Reserved.

#include "NPC/DeliveryPedestrian.h"
#include "Traffic/DeliveryTrafficCarComponent.h"
#include "Vehicle/DeliveryMotorbike.h"

#include "Animation/AnimSequence.h"
#include "Combat/DeliveryCollisionChannels.h"
#include "Components/CapsuleComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Delivery.h"
#include "Engine/CollisionProfile.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "NPC/DeliveryPedestrianRoute.h"
#include "Net/UnrealNetwork.h"

namespace DeliveryPedestrianBones
{
// 这六份 FBX 都是 Mixamo 标准命名（Hips / Spine / LeftUpLeg / LeftLeg / LeftArm …），
// 已逐个解析确认过，所以可以写死名字；解析不到的骨骼会被静静跳过，不会写错骨头
const FName Hips(TEXT("Hips"));
const FName Spine(TEXT("Spine1"));
const FName LeftThigh(TEXT("LeftUpLeg"));
const FName RightThigh(TEXT("RightUpLeg"));
const FName LeftCalf(TEXT("LeftLeg"));
const FName RightCalf(TEXT("RightLeg"));
const FName LeftArm(TEXT("LeftArm"));
const FName RightArm(TEXT("RightArm"));
// 前臂只用来量上臂的指向（肘到肩的连线），不单独写它：
// PoseableMesh 改父骨的组件空间旋转时子骨会跟着走，所以垂手一次就够
const FName LeftForeArm(TEXT("LeftForeArm"));
const FName RightForeArm(TEXT("RightForeArm"));
}  // namespace DeliveryPedestrianBones

ADeliveryPedestrian::ADeliveryPedestrian()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	Mesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Mesh"));
	SetRootComponent(Mesh);
	// 路人只是布景，不参与碰撞也不挡玩家：开了碰撞之后他会把布娃娃顶翻，
	// 而他本身是运动学推进的，撞上也不会让路，表现只会是玩家被一个不动的人卡住
	Mesh->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	Mesh->SetGenerateOverlapEvents(false);
	// 看不见时不算骨骼：路人进出视野很频繁，而他们的姿势没有任何逻辑依赖
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;

	PoseableBody = CreateDefaultSubobject<UPoseableMeshComponent>(TEXT("PoseableBody"));
	PoseableBody->SetupAttachment(Mesh);
	PoseableBody->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	PoseableBody->SetGenerateOverlapEvents(false);

	HitBox = CreateDefaultSubobject<UCapsuleComponent>(TEXT("HitBox"));
	HitBox->SetupAttachment(Mesh);
	// 缩放 1.0 的成年路人身高 140（= setup_npc.py 的 TARGET_ADULT_HEIGHT，和主角一样高
	// ——主角的胶囊半高就是 70，renwu 网格实测 139.9）：半高 70、脚底在原点，
	// 所以胶囊中心抬到 70。
	// **这个数要跟着那个基准改**：胶囊挂在根上、跟着 HeightScale 一起缩，
	// 所以小孩/老人自动按比例变矮；但基准本身变了的话，这里不改就会出现
	// 「打他头顶打不到、脚下一截空的也算命中」——而那和"命中窗口没对上"长得一模一样
	HitBox->InitCapsuleSize(24.f, 70.f);
	HitBox->SetRelativeLocation(FVector(0.f, 0.f, 70.f));
	// 实心：玩家走过去会被挡住，不再穿过去。
	// **当初"只查询不阻挡"的理由（会把布娃娃顶翻、而他自己撞上不会让路）没有消失，
	// 是被补上的让路逻辑解掉的**——见 bYieldToObstacles。只开碰撞不开让路
	// 一定会回到"被一个不动的人卡住"
	HitBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	// 走路人自己的通道，不挂 PhysicsBody——那个通道被抓取候选搜索、拖人、
	// 摩托车贴地探测一堆现有查询盯着，挂上去会被它们全部捎上（见 DeliveryCollisionChannels.h）
	HitBox->SetCollisionObjectType(DeliveryCollision::Pedestrian);
	HitBox->SetCollisionResponseToAllChannels(ECR_Ignore);
	HitBox->SetCollisionResponseToChannel(DeliveryCollision::Pedestrian, ECR_Block);
	// 挡玩家的身体（布娃娃刚体是 PhysicsBody）。
	// **刻意不挡 Pawn**：摩托车的碰撞盒是 Pawn 配置、水平推进带 sweep，
	// 挡 Pawn 的表现是路人变成一堵墙把车别停，而骑车撞人该是把人撞飞。
	// 实际开关在 BeginPlay 里按 bBlockCharacters 应用——构造函数读到的永远是
	// C++ 默认值，蓝图上改的覆盖那时还没套上
	HitBox->SetGenerateOverlapEvents(false);

	bReplicates = true;
	SetReplicateMovement(true);
	// 路人进出视野很频繁，净相关距离给小一点，远处的人不占带宽
	// UE 5.5 起这个字段不能直接写，要走 setter
	SetNetCullDistanceSquared(120000.f * 120000.f);
}

void ADeliveryPedestrian::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ADeliveryPedestrian, AppearAlpha);
	DOREPLIFETIME(ADeliveryPedestrian, bFadeAppearance);
	DOREPLIFETIME(ADeliveryPedestrian, ReplicatedMinScale);
	DOREPLIFETIME(ADeliveryPedestrian, bKnockedDown);
	DOREPLIFETIME(ADeliveryPedestrian, KnockImpulse);
	DOREPLIFETIME(ADeliveryPedestrian, KnockPoint);
	DOREPLIFETIME(ADeliveryPedestrian, ActiveMesh);
	DOREPLIFETIME(ADeliveryPedestrian, ActiveWalkAnim);
	DOREPLIFETIME(ADeliveryPedestrian, BaseScale);
	DOREPLIFETIME(ADeliveryPedestrian, Route);
	DOREPLIFETIME(ADeliveryPedestrian, WalkSpeed);
	DOREPLIFETIME(ADeliveryPedestrian, AnimReferenceSpeed);
}

void ADeliveryPedestrian::OnRep_AppearAlpha()
{
	ApplyAppearScale();
}

void ADeliveryPedestrian::OnRep_FadeAppearance()
{
	InstallFadeMaterial();
	ApplyAppearScale();
}

void ADeliveryPedestrian::InstallFadeMaterial()
{
	if (!bFadeAppearance || FadeMaterialInstance)
	{
		return;
	}
	const ADeliveryPedestrianRoute* SourceRoute = Route.IsValid() ? Route.Get()
	    : Cast<ADeliveryPedestrianRoute>(GetOwner());
	USkeletalMesh* CurrentMesh = ActiveMesh ? ActiveMesh.Get()
	    : Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
	if (!CurrentMesh && PoseableBody)
	{
		CurrentMesh = Cast<USkeletalMesh>(PoseableBody->GetSkinnedAsset());
	}
	UMaterialInterface* FadeMaterial = SourceRoute ? SourceRoute->GetFadeMaterialForMesh(CurrentMesh) : nullptr;
	if (FadeMaterial && CurrentMesh && CurrentMesh->GetMaterials().IsValidIndex(0))
	{
		OriginalMaterial = CurrentMesh->GetMaterials()[0].MaterialInterface;
		FadeMaterialInstance = UMaterialInstanceDynamic::Create(FadeMaterial, this);
	}
}

void ADeliveryPedestrian::BeginPlay()
{
	// 挡不挡玩家在这里应用，不在构造函数里：构造函数读到的是 C++ 默认值，
	// 蓝图上改过的覆盖那时还没套上，表现是"开关调了不生效"
	if (HitBox)
	{
		HitBox->SetCollisionResponseToChannel(ECC_PhysicsBody,
		                                      bBlockCharacters ? ECR_Block : ECR_Ignore);
	}

	Super::BeginPlay();

	if (HasAuthority())
	{
		BaseScale = GetActorScale3D();
		// 进场前先缩到看不见，客户端不能把复制来的当前缩放重新当作基准
		SetActorScale3D(BaseScale * KINDA_SMALL_NUMBER);
	}
	else
	{
		OnRep_Appearance();
		ApplyAppearScale();
	}
}

void ADeliveryPedestrian::SetCrosswalkSpeedMultiplier(float Multiplier)
{
	if (!HasAuthority() || BaseWalkSpeed <= 0.f)
	{
		return;
	}
	const float NewSpeed = BaseWalkSpeed * FMath::Max(Multiplier, 1.f);
	if (FMath::IsNearlyEqual(WalkSpeed, NewSpeed))
	{
		return;
	}
	WalkSpeed = NewSpeed;
	if (Mesh && ActiveWalkAnim)
	{
		const float Rate = (AnimReferenceSpeed > 1.f) ? WalkSpeed / AnimReferenceSpeed : 1.f;
		Mesh->SetPlayRate(FMath::Clamp(Rate, 0.25f, 3.f));
	}
}

// 只有正式过街路线响应鸣笛，路线统一管理整组加速
void ADeliveryPedestrian::ReceiveHorn(float SpeedMultiplier)
{
	if (HasAuthority() && Route.IsValid() && !IsKnockedDown() && !IsStandingUp())
	{
		Route->NotifyHorn(SpeedMultiplier);
	}
}

void ADeliveryPedestrian::BeginWalk(ADeliveryPedestrianRoute* InRoute, USkeletalMesh* InMesh,
                                    UAnimSequence* InWalkAnim, float InStartDistance, bool bInForward,
                                    float InSpeed, float InAnimReferenceSpeed)
{
	Route = InRoute;
	DistanceAlongRoute = InStartDistance;
	RoamAppearElapsed = 0.f;
	if (InRoute && InRoute->bRoamInArea)
	{
		RoamTarget = InRoute->PickRoamPoint(GetActorLocation(), 100.f);
	}
	bForward = bInForward;
	BaseWalkSpeed = InSpeed;
	WalkSpeed = InSpeed;
	AnimReferenceSpeed = InAnimReferenceSpeed;
	Phase = EDeliveryPedestrianPhase::Appearing;
	BoidsVelocity = FVector::ZeroVector;
	const float InitialSpread = UsesBoids() ? FMath::Clamp(Route->BoidsCorridorHalfWidth, 0.f, 12.f) : 0.f;
	BoidsLateralOffset = FMath::FRandRange(-InitialSpread, InitialSpread);
	ActiveMesh = InMesh;
	ActiveWalkAnim = InWalkAnim;
	bFadeAppearance = InRoute && InRoute->bFadeInsteadOfScale;
	OnRep_Appearance();
	ScheduleNextPause();
	if (InRoute && InRoute->bRoamInArea)
	{
		UpdateRoamTransform(GetActorLocation(), 0.f);
	}
	else
	{
		UpdateTransform(DistanceAlongRoute);
	}
	UpdateAppearScale(DistanceAlongRoute);
}

// 网格与动画由服务器选择，客户端只重建本机显示，不执行路线初始化
void ADeliveryPedestrian::OnRep_Appearance()
{
	if (!ActiveMesh) return;
	if (PresentedMesh == ActiveMesh && PresentedAnimation == ActiveWalkAnim)
	{
		ApplyAppearScale();
		return;
	}
	PresentedMesh = ActiveMesh;
	PresentedAnimation = ActiveWalkAnim;
	USkeletalMesh* InMesh = ActiveMesh;
	UAnimSequence* InWalkAnim = ActiveWalkAnim;
	// 没有骨骼的网格不能交给 PoseableMesh：它的 AllocateTransformData →
	// FillComponentSpaceTransforms 会按骨骼数索引，骨骼数为 0 时直接断言崩溃
	// （实测崩在 SetSkinnedAssetAndUpdate 里）。配错网格不该让整局崩掉
	const bool bMeshHasBones = (InMesh != nullptr) && (InMesh->GetRefSkeleton().GetNum() > 0);
	if (InMesh && !bMeshHasBones)
	{
		UE_LOG(LogDelivery, Warning,
		       TEXT("[Pedestrian] %s 没有骨骼，只能当静态外观用，不会有走路动作。"),
		       *GetNameSafe(InMesh));
	}

	bProceduralGait = (InWalkAnim == nullptr) && bMeshHasBones;
	// 同一帧生成的几个人不要迈同一条腿
	GaitPhase = FMath::FRand();

	if (bProceduralGait)
	{
		// 没有动画资产：骨骼由 C++ 逐帧写，所以身体换到 PoseableMesh 上
		// UE5 的 Poseable 换网格走 SetSkinnedAssetAndUpdate，旧的 SetSkeletalMesh 已弃用
		PoseableBody->SetSkinnedAssetAndUpdate(InMesh, true);
		PoseableBody->SetVisibility(true);
		Mesh->SetSkeletalMeshAsset(nullptr);
		Mesh->SetVisibility(false);
		CacheGaitReferencePose();
	}
	else
	{
		PoseableBody->SetVisibility(false);
		Mesh->SetSkeletalMeshAsset(InMesh);
		Mesh->SetVisibility(true);

		// InWalkAnim 可能为空（骨骼数为 0 走到这条路），空指针不能喂给 PlayAnimation
		if (InWalkAnim)
		{
			// 单节点播放，不建 AnimBP：路人只有一个"走"的状态，
			// 为它建一套状态机只会多出一个要同步维护的资产
			Mesh->PlayAnimation(InWalkAnim, true);

			// 播放速率对齐实际速度，否则随机出来的快慢会变成滑步或踏步
			const float Rate = (AnimReferenceSpeed > 1.f) ? (WalkSpeed / AnimReferenceSpeed) : 1.f;
			Mesh->SetPlayRate(FMath::Clamp(Rate, 0.25f, 3.f));
			Mesh->SetPosition(GaitPhase * InWalkAnim->GetPlayLength());
		}
	}

	InstallFadeMaterial();
	ApplyAppearScale();
	if (!HasAuthority() && bKnockedDown) StartKnockdown();
}

void ADeliveryPedestrian::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	// 运动学 Actor 的 GetVelocity 不代表复制的位移，客户端按实际位移推进摆腿
	const float ClientTravel = bClientLocationReady ? FVector::Dist2D(LastClientLocation, GetActorLocation()) : 0.f;
	LastClientLocation = GetActorLocation();
	bClientLocationReady = true;

	// 被打飞之后位置完全交给物理，不能再按样条写变换——
	// 两边同时写的表现是人一边躺着一边被拖回路线上
	if (bKnockedDown)
	{
		UpdateKnockdown(DeltaTime);
		return;
	}

	// 起身期间位置和姿势都由起身那套算，不能再走样条那条路——
	// 两边同时写的表现是人一边往上立一边被拖着往前走
	if (Phase == EDeliveryPedestrianPhase::StandingUp)
	{
		UpdateGetUp(DeltaTime);
		return;
	}

	// 位置只由服务器推进，客户端靠移动复制跟上；两边各算一遍会互相拉扯
	if (!HasAuthority())
	{
		// 但程序化步态要在每台机器上各跑一遍：骨骼姿势不在复制范围内，
		// 不跑的话远端客户端看到的是一群滑行的木头人
		if (bProceduralGait)
		{
			// 停着的时候速度自然是 0，所以客户端不需要知道"在停顿"这件事——
			// 按速度分流就够了，省掉一个要复制的状态
			if (ClientTravel > .01f)
			{
				UpdateProceduralGait(FMath::Min(ClientTravel, 80.f));
			}
			else
			{
				UpdateIdleSway(DeltaTime);
			}
		}
		return;
	}

	ADeliveryPedestrianRoute* RoutePtr = Route.Get();
	if (!RoutePtr)
	{
		// 路线被删了（改关卡、或者路线自己被销毁），路人没有线可走，直接退场
		Destroy();
		return;
	}

	if (RoutePtr->bRoamInArea)
	{
		const float AllowedStep = UpdateWalkAndPause(DeltaTime, 0.f);
		FVector Position = GetActorLocation();
		FVector ToTarget = RoamTarget - Position;
		ToTarget.Z = 0.f;
		if (ToTarget.SizeSquared2D() < FMath::Square(15.f))
		{
			RoamTarget = RoutePtr->PickRoamPoint(Position, 100.f);
			ToTarget = RoamTarget - Position;
			ToTarget.Z = 0.f;
		}
		float Travel = FMath::Min(AllowedStep, ToTarget.Size2D());
		if (Travel > 0.f)
		{
			const FVector Next = Position + ToTarget.GetSafeNormal2D() * Travel;
			if (IsRoamStepBlocked(Position, Next))
			{
				ReverseRoamTarget();
				Travel = 0.f;
			}
			else
			{
				Position = Next;
				DistanceAlongRoute += Travel;
			}
		}
		RoamAppearElapsed += DeltaTime;
		UpdateRoamTransform(Position, DeltaTime);
		UpdateAppearScale(DistanceAlongRoute);
		if (bProceduralGait)
		{
			if (Travel > 0.f)
			{
				UpdateProceduralGait(Travel);
			}
			else
			{
				UpdateIdleSway(DeltaTime);
			}
		}
		return;
	}

	// 换线之后要重新读，所以不是 const
	float Length = RoutePtr->GetRouteLength();
	if (Length <= KINDA_SMALL_NUMBER)
	{
		Destroy();
		return;
	}

	float Step = UpdateWalkAndPause(DeltaTime, Length);
	if (UsesBoids()) Step = UpdateBoids(Step, DeltaTime);
	DistanceAlongRoute += (bForward ? 1.f : -1.f) * Step;

	// 走到端点：先试着换一条相连的路线继续走，接不上才退场。
	// 判据放在推进之后、写变换之前，省掉"最后一帧越过端点"的穿出
	const float Remaining = bForward ? (Length - DistanceAlongRoute) : DistanceAlongRoute;
	if (Remaining <= 0.f)
	{
		if (!TryHandoff())
		{
			RoutePtr->NotifyPedestrianFinished(this);
			Destroy();
			return;
		}
		// 换线之后 Route / DistanceAlongRoute 都变了，这一帧按新路线写变换
		RoutePtr = Route.Get();
		if (!RoutePtr)
		{
			Destroy();
			return;
		}
		Length = RoutePtr->GetRouteLength();
	}

	UpdateTransform(DistanceAlongRoute, DeltaTime);
	UpdateAppearScale(DistanceAlongRoute);

	if (bProceduralGait)
	{
		if (Step > 0.f)
		{
			UpdateProceduralGait(Step);
		}
		else
		{
			UpdateIdleSway(DeltaTime);
		}
	}
}

void ADeliveryPedestrian::UpdateTransform(float Distance, float DeltaTime)
{
	ADeliveryPedestrianRoute* RoutePtr = Route.Get();
	if (!RoutePtr)
	{
		return;
	}

	FVector Target = RoutePtr->GetLocationAtDistance(Distance);
	if (UsesBoids())
	{
		const FVector Side = FVector::CrossProduct(FVector::UpVector, RoutePtr->GetDirectionAtDistance(Distance)).GetSafeNormal2D();
		Target += Side * BoidsLateralOffset;
	}

	// 样条是人手拉的，端点高度未必正好贴在路面上，所以每帧实测一次
	float GroundZ = 0.f;
	if (TraceGroundZ(Target, GroundZ))
	{
		Target.Z = GroundZ;
	}

	FVector Forward = RoutePtr->GetDirectionAtDistance(Distance);
	if (!bForward)
	{
		Forward = -Forward;
	}
	if (UsesBoids() && !BoidsVelocity.IsNearlyZero()) Forward = BoidsVelocity;
	// 只取水平朝向：坡上的路人不该跟着路面俯仰，那会让他像在爬
	Forward.Z = 0.f;
	if (Forward.IsNearlyZero())
	{
		Forward = GetActorForwardVector();
		Forward.Z = 0.f;
	}

	FRotator Rotation = Forward.IsNearlyZero() ? GetActorRotation() : Forward.GetSafeNormal().Rotation();
	if (UsesBoids() && DeltaTime > 0.f) Rotation = FMath::RInterpConstantTo(GetActorRotation(), Rotation, DeltaTime, 150.f);
	// 停着的时候朝向偏开一点，像在看橱窗；站得笔直不动最像卡住
	if (PauseRemaining > 0.f)
	{
		Rotation.Yaw += PauseYawOffset;
	}
	SetActorLocationAndRotation(Target, Rotation);
}

bool ADeliveryPedestrian::UsesBoids() const
{
	return Route.IsValid() && Route->bUseBoids && !Route->bRoamInArea;
}

float ADeliveryPedestrian::UpdateBoids(float AllowedStep, float DeltaTime)
{
	if (AllowedStep <= 0.f || DeltaTime <= 0.f)
	{
		BoidsVelocity = FVector::ZeroVector;
		return 0.f;
	}
	const auto* Group = Route.Get();
	const float Dt = FMath::Min(DeltaTime, .1f);
	const FVector Tangent = Group->GetDirectionAtDistance(DistanceAlongRoute).GetSafeNormal2D();
	const FVector Forward = bForward ? Tangent : -Tangent;
	const FVector Side = FVector::CrossProduct(FVector::UpVector, Tangent);
	const FVector Position = GetActorLocation();
	FVector Separation = FVector::ZeroVector, Alignment = FVector::ZeroVector, Center = FVector::ZeroVector;
	int32 Count = 0;
	// 邻居仅取本路线的弱引用名单，不建跨 World 静态缓存；倒地者不参与群体引导
	for (const auto& Entry : Group->GetAlivePedestrians())
	{
		const auto* Other = Entry.Get();
		if (!IsValid(Other) || Other == this || Other->GetWorld() != GetWorld() || Other->IsActorBeingDestroyed() ||
		    Other->IsKnockedDown() || Other->IsStandingUp()) continue;
		FVector Away = Position - Other->GetActorLocation(); Away.Z = 0.f;
		const float Distance = Away.Size();
		if (Distance > Group->BoidsNeighborRadius) continue;
		++Count; Center += Other->GetActorLocation();
		Alignment += Other->BoidsVelocity.IsNearlyZero() ? Forward * Other->WalkSpeed : Other->BoidsVelocity;
		if (Distance < Group->BoidsSeparationDistance)
		{
			// 完全共线或重叠时给两人相反的侧向偏好，避免只会互相减速堵成一列
			if (FMath::Abs(FVector::DotProduct(Away, Side)) < 5.f)
				Away += Side * (GetUniqueID() < Other->GetUniqueID() ? -20.f : 20.f);
			Separation += Away.GetSafeNormal() * (1.f - Distance / FMath::Max(Group->BoidsSeparationDistance, 1.f));
		}
	}
	FVector Desired = Forward * WalkSpeed + Separation * WalkSpeed * Group->BoidsSeparationWeight;
	if (Count > 0)
	{
		Desired += (Alignment / Count - Forward * WalkSpeed) * Group->BoidsAlignmentWeight;
		Desired += (Center / Count - Position).GetClampedToMaxSize2D(WalkSpeed) * Group->BoidsCohesionWeight;
	}
	// 轻微回到路线中心，硬边界仍在更新位置时限制；始终保留过街方向
	Desired -= Side * BoidsLateralOffset * .6f;
	Desired.Z = 0.f;
	BoidsVelocity += (Desired - BoidsVelocity).GetClampedToMaxSize(Group->BoidsMaxAcceleration * Dt);
	const float Along = FMath::Clamp(FVector::DotProduct(BoidsVelocity, Forward), WalkSpeed * .25f, WalkSpeed * 1.15f);
	const float Across = FMath::Clamp(FVector::DotProduct(BoidsVelocity, Side), -WalkSpeed * .65f, WalkSpeed * .65f);
	const float Width = FMath::Max(Group->BoidsCorridorHalfWidth, 0.f);
	const float Offset = FMath::Clamp(BoidsLateralOffset + Across * Dt, -Width, Width);
	const float Step = FMath::Min(Along * Dt, AllowedStep * 1.15f);
	const FVector Next = Group->GetLocationAtDistance(DistanceAlongRoute + (bForward ? Step : -Step)) + Side * Offset;
	if (IsRoamStepBlocked(Position, Next))
	{
		BoidsVelocity = FVector::ZeroVector;
		return 0.f;
	}
	BoidsVelocity = Forward * Along + Side * ((Offset - BoidsLateralOffset) / Dt);
	BoidsLateralOffset = Offset;
	return Step;
}

void ADeliveryPedestrian::UpdateRoamTransform(const FVector& DesiredLocation, float DeltaTime)
{
	FVector Grounded = DesiredLocation;
	float GroundZ = 0.f;
	if (TraceGroundZ(Grounded, GroundZ))
	{
		Grounded.Z = GroundZ;
	}

	FVector Forward = RoamTarget - Grounded;
	Forward.Z = 0.f;
	FRotator Wanted = Forward.IsNearlyZero() ? GetActorRotation() : Forward.Rotation();
	if (PauseRemaining > 0.f)
	{
		Wanted.Yaw += PauseYawOffset;
	}
	const FRotator Facing = DeltaTime <= 0.f ? Wanted
	    : FMath::RInterpTo(GetActorRotation(), Wanted, DeltaTime, 4.f);
	SetActorLocationAndRotation(Grounded, Facing);
}

bool ADeliveryPedestrian::IsRoamStepBlocked(const FVector& From, const FVector& To) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ECC_WorldStatic);
	Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(DeliveryPedestrianRoamStep), false, this);
	FHitResult Hit;
	const FVector Waist(0.f, 0.f, YieldProbeHeight);
	return World->SweepSingleByObjectType(
	    Hit, From + Waist, To + Waist, FQuat::Identity, Objects,
	    FCollisionShape::MakeSphere(FMath::Max(YieldProbeRadius, 1.f)), Params);
}

void ADeliveryPedestrian::ReverseRoamTarget()
{
	const ADeliveryPedestrianRoute* RoutePtr = Route.Get();
	if (!RoutePtr || !RoutePtr->bRoamInArea)
	{
		return;
	}
	const FVector Position = GetActorLocation();
	FVector Away = (Position - RoamTarget).GetSafeNormal2D();
	if (Away.IsNearlyZero())
	{
		Away = -GetActorForwardVector().GetSafeNormal2D();
	}
	const FVector Center = RoutePtr->GetActorLocation();
	const float Radius = FMath::Max(RoutePtr->RoamRadius, 50.f);
	const auto ClampToArea = [&](const FVector& Candidate)
	{
		return Center + (Candidate - Center).GetClampedToMaxSize2D(Radius);
	};
	FVector Next = ClampToArea(Position + Away * FMath::Min(Radius, 250.f));
	if (FVector::DistSquared2D(Next, Position) < FMath::Square(60.f))
	{
		// 到了活动区边缘，反向无路可退时沿边界转开。
		Next = ClampToArea(Position + FVector(-Away.Y, Away.X, 0.f) *
		                              FMath::Min(Radius, 250.f));
	}
	RoamTarget = Next;
	PauseRemaining = FMath::Max(PauseRemaining, 0.5f);
	PauseYawOffset = 0.f;
	YieldElapsed = 0.f;
}

bool ADeliveryPedestrian::TraceGroundZ(const FVector& Probe, float& OutZ) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// 探测范围刻意不大：路人只是在人行道的高低起伏上找面，
	// 给太长会让他在桥洞或路口叠面处掉到下一层路面上
	const FVector Start = Probe + FVector(0.f, 0.f, 150.f);
	const FVector End = Probe - FVector(0.f, 0.f, 400.f);

	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
	ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(DeliveryPedestrianGround), false, this);

	FHitResult Hit;
	if (World->LineTraceSingleByObjectType(Hit, Start, End, ObjectParams, Params))
	{
		OutZ = Hit.ImpactPoint.Z;
		return true;
	}

	return false;
}

void ADeliveryPedestrian::UpdateAppearScale(float Distance)
{
	ADeliveryPedestrianRoute* RoutePtr = Route.Get();
	if (!RoutePtr)
	{
		return;
	}
	if (RoutePtr->bRoamInArea)
	{
		AppearAlpha = FMath::Clamp(RoamAppearElapsed / 0.5f, 0.f, 1.f);
		ReplicatedMinScale = RoutePtr->GetMinVisibleScale();
		Phase = AppearAlpha < 1.f ? EDeliveryPedestrianPhase::Appearing
		                          : EDeliveryPedestrianPhase::Walking;
		ApplyAppearScale();
		return;
	}

	const float Length = RoutePtr->GetRouteLength();
	const float Window = FMath::Max(RoutePtr->GetAppearDistance(), 1.f);

	// 两头各留一个窗口：从进场端点走出去的那一段放大，接近离场端点的那一段缩小
	const float FromEntry = bForward ? Distance : (Length - Distance);
	const float ToExit = bForward ? (Length - Distance) : Distance;

	// 两头的淡入淡出都是**有条件的**：
	//   * 换线进来的那一头不淡入——路人正站在新路线的端点上，照常淡入会在路口
	//     原地缩到看不见再长回来；
	//   * 还能接下一条的那一头不淡出——他不是要消失，是要继续走。
	// 还能接驳取决于"接驳次数没用完"且"那一端真的有下家"，所以两个条件都要查：
	// 只查有没有下家的话，用完次数的人会一路走到端点才突然消失
	const bool bCanContinue = RoutePtr->MaxHandoffs > HandoffCount && RoutePtr->HasContinuation(bForward);

	float Alpha = 1.f;
	if (ToExit < Window && !bCanContinue)
	{
		Alpha = FMath::Clamp(ToExit / Window, 0.f, 1.f);
		Phase = EDeliveryPedestrianPhase::Disappearing;
	}
	else if (FromEntry < Window && !bEnteredByHandoff)
	{
		Alpha = FMath::Clamp(FromEntry / Window, 0.f, 1.f);
		Phase = EDeliveryPedestrianPhase::Appearing;
	}
	else
	{
		Phase = EDeliveryPedestrianPhase::Walking;
	}

	AppearAlpha = Alpha;
	ReplicatedMinScale = RoutePtr->GetMinVisibleScale();
	ApplyAppearScale();
}

void ADeliveryPedestrian::ApplyAppearScale()
{
	if (bFadeAppearance)
	{
		InstallFadeMaterial();
		if (!FadeMaterialInstance || !OriginalMaterial)
		{
			SetActorScale3D(BaseScale * FMath::Max(AppearAlpha, KINDA_SMALL_NUMBER));
			return;
		}
		SetActorScale3D(BaseScale);
		const bool bInFadeWindow = AppearAlpha < 1.f - KINDA_SMALL_NUMBER;
		UMaterialInterface* DesiredMaterial = bInFadeWindow
		    ? static_cast<UMaterialInterface*>(FadeMaterialInstance.Get()) : OriginalMaterial.Get();
		if (Mesh->GetMaterial(0) != DesiredMaterial)
		{
			Mesh->SetMaterial(0, DesiredMaterial);
		}
		if (PoseableBody->GetMaterial(0) != DesiredMaterial)
		{
			PoseableBody->SetMaterial(0, DesiredMaterial);
		}
		if (bInFadeWindow)
		{
			FadeMaterialInstance->SetScalarParameterValue(TEXT("FadeOpacity"), AppearAlpha);
		}
		return;
	}
	// 缩放乘在进场时记下的原始缩放上，不直接写 1：
	// 关卡里手调过路人大小的话，直接写 1 会把那个调整抹掉
	const float Floor = FMath::Max(ReplicatedMinScale, KINDA_SMALL_NUMBER);
	SetActorScale3D(BaseScale * FMath::Max(AppearAlpha, Floor));
}

void ADeliveryPedestrian::CacheGaitReferencePose()
{
	GaitReferenceRotations.Reset();
	bHasHipBone = false;

	if (!PoseableBody || !PoseableBody->GetSkinnedAsset())
	{
		return;
	}

	// PoseableMesh 刚换上网格时就是参考姿势，所以这时候读到的正是没被改过的朝向。
	// 每帧现读是不行的：读到的会是上一帧自己写进去的值，角度会一路累积
	// （出拳那边"禁止独立积分旋转"记的是同一个教训）
	const TArray<FName> Bones = {DeliveryPedestrianBones::Spine,     DeliveryPedestrianBones::LeftThigh,
	                             DeliveryPedestrianBones::RightThigh, DeliveryPedestrianBones::LeftCalf,
	                             DeliveryPedestrianBones::RightCalf,  DeliveryPedestrianBones::LeftArm,
	                             DeliveryPedestrianBones::RightArm};

	for (const FName& Bone : Bones)
	{
		if (PoseableBody->GetBoneIndex(Bone) == INDEX_NONE)
		{
			continue;
		}

		GaitReferenceRotations.Add(
		    Bone, PoseableBody->GetBoneRotationByName(Bone, EBoneSpaces::ComponentSpace).Quaternion());
	}

	if (PoseableBody->GetBoneIndex(DeliveryPedestrianBones::Hips) != INDEX_NONE)
	{
		HipReferenceLocation =
		    PoseableBody->GetBoneLocationByName(DeliveryPedestrianBones::Hips, EBoneSpaces::ComponentSpace);
		bHasHipBone = true;
	}

	CacheArmRestDeltas();

	if (GaitReferenceRotations.IsEmpty())
	{
		// 骨骼名对不上（换了不是 Mixamo 命名的模型）时点名一次，
		// 否则表现只是"人在滑行"，而那和"忘了配动画"长得一模一样
		UE_LOG(LogDelivery, Warning,
		       TEXT("[Pedestrian] %s 的骨架里找不到 Mixamo 骨骼名（Hips/LeftUpLeg/…），"
		            "程序化步态不会有动作。"),
		       *GetNameSafe(PoseableBody->GetSkinnedAsset()));
	}
}

void ADeliveryPedestrian::CacheArmRestDeltas()
{
	LeftArmRestDelta = FQuat::Identity;
	RightArmRestDelta = FQuat::Identity;

	if (!PoseableBody || ArmRestDownAngle <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	// 目标方向和竖直向下的夹角：没转满的那一截（90 − 垂手角）加上外张角。
	// 两项都落在"手臂往身体外侧张开"这同一个方向上，所以直接相加——
	// 垂手角给 90、外张给 0 时正好是完全贴着身体竖直向下
	const float FromDown = FMath::DegreesToRadians(90.f - FMath::Clamp(ArmRestDownAngle, 0.f, 90.f) +
	                                               FMath::Clamp(ArmRestOutwardAngle, 0.f, 45.f));

	struct FArmPair
	{
		FName Upper;
		FName Lower;
		FQuat* Delta;
	};
	const FArmPair Arms[] = {
	    {DeliveryPedestrianBones::LeftArm, DeliveryPedestrianBones::LeftForeArm, &LeftArmRestDelta},
	    {DeliveryPedestrianBones::RightArm, DeliveryPedestrianBones::RightForeArm, &RightArmRestDelta}};

	for (const FArmPair& Arm : Arms)
	{
		if (PoseableBody->GetBoneIndex(Arm.Upper) == INDEX_NONE ||
		    PoseableBody->GetBoneIndex(Arm.Lower) == INDEX_NONE)
		{
			continue;
		}

		// 上臂的实际指向 = 肘的位置 − 肩的位置。读的是刚换上网格时的参考姿势，
		// 所以这就是绑定姿势下手臂朝哪（T 字形时近似 ±Y）
		const FVector Shoulder =
		    PoseableBody->GetBoneLocationByName(Arm.Upper, EBoneSpaces::ComponentSpace);
		const FVector Elbow = PoseableBody->GetBoneLocationByName(Arm.Lower, EBoneSpaces::ComponentSpace);
		const FVector Current = (Elbow - Shoulder).GetSafeNormal();
		if (Current.IsNearlyZero())
		{
			continue;
		}

		// 外张朝哪一侧由实测方向自己说：左臂在组件空间里朝 −Y，右臂朝 +Y，
		// 写死左右的话换一份镜像导出的模型就会把手转到身体另一边
		const float SideSign = Current.Y >= 0.f ? 1.f : -1.f;
		const FVector Desired(0.f, SideSign * FMath::Sin(FromDown), -FMath::Cos(FromDown));

		// 只求"把这根骨头从现在的指向转到目标指向"的最短旋转：不碰绕骨轴的自转，
		// 那一轴决定手掌朝内还是朝外，硬给会把手腕拧过来
		*Arm.Delta = FQuat::FindBetweenNormals(Current, Desired.GetSafeNormal());
	}
}

void ADeliveryPedestrian::SetBoneDelta(FName BoneName, const FQuat& Delta)
{
	const FQuat* Reference = GaitReferenceRotations.Find(BoneName);
	if (!Reference)
	{
		return;
	}

	PoseableBody->SetBoneRotationByName(BoneName, (Delta * (*Reference)).Rotator(),
	                                    EBoneSpaces::ComponentSpace);
}

void ADeliveryPedestrian::AddBoneSwing(FName BoneName, const FVector& Axis, float AngleDegrees)
{
	const FQuat* Reference = GaitReferenceRotations.Find(BoneName);
	if (!Reference)
	{
		return;
	}

	// 增量乘在参考朝向左边：绕的是组件空间的轴，而不是骨骼自己的局部轴。
	// 这几份模型来自 Blender，哪个局部轴是"摆腿轴"并不保证一致，
	// 而组件空间的 X 永远是角色正前方、Y 是右、Z 是上
	const FQuat Delta(Axis.GetSafeNormal(), FMath::DegreesToRadians(AngleDegrees));
	PoseableBody->SetBoneRotationByName(BoneName, (Delta * (*Reference)).Rotator(),
	                                    EBoneSpaces::ComponentSpace);
}

void ADeliveryPedestrian::UpdateProceduralGait(float DistanceDelta)
{
	if (!PoseableBody || GaitReferenceRotations.IsEmpty())
	{
		return;
	}

	// 相位按走过的路程推进，不按时间：走得快的人步频自然就高，
	// 不需要再调一个频率参数，也不会出现"腿的速度和位移速度不一致"的滑步
	const float Stride = FMath::Max(StrideLength, 10.f);
	GaitPhase = FMath::Fmod(GaitPhase + FMath::Abs(DistanceDelta) / Stride, 1.f);

	const float Theta = GaitPhase * 2.f * PI;
	const float Swing = FMath::Sin(Theta);

	// 组件空间：绕 Y 轴是前后摆（腿和手臂），绕 X 轴是左右倾（躯干）
	const FVector PitchAxis = FVector::YAxisVector;
	const FVector RollAxis = FVector::XAxisVector;

	AddBoneSwing(DeliveryPedestrianBones::LeftThigh, PitchAxis, -ThighSwingAngle * Swing);
	AddBoneSwing(DeliveryPedestrianBones::RightThigh, PitchAxis, ThighSwingAngle * Swing);

	// 膝盖只往后弯，所以取单边：抬腿的那一侧折起来，支撑的那一侧保持直
	// 给成双向的话抬腿时小腿会向前反折，一眼就是坏的
	const float LeftBend = FMath::Max(0.f, -FMath::Sin(Theta - 0.6f));
	const float RightBend = FMath::Max(0.f, -FMath::Sin(Theta + PI - 0.6f));
	AddBoneSwing(DeliveryPedestrianBones::LeftCalf, PitchAxis, KneeBendAngle * LeftBend);
	AddBoneSwing(DeliveryPedestrianBones::RightCalf, PitchAxis, -KneeBendAngle * RightBend);

	// 手臂和同侧腿反相，这是人走路的样子；同相会像在跳。
	// **垂手和摆动必须一次写进去**：先垂手再调一次 AddBoneSwing 的话，
	// 后一次会从参考姿势重算、把垂手抹掉，表现就是又变回平举
	const FQuat LeftSwing(PitchAxis, FMath::DegreesToRadians(ArmSwingAngle * Swing));
	const FQuat RightSwing(PitchAxis, FMath::DegreesToRadians(-ArmSwingAngle * Swing));
	SetBoneDelta(DeliveryPedestrianBones::LeftArm, LeftSwing * LeftArmRestDelta);
	SetBoneDelta(DeliveryPedestrianBones::RightArm, RightSwing * RightArmRestDelta);

	AddBoneSwing(DeliveryPedestrianBones::Spine, RollAxis, TorsoRollAngle * Swing);

	if (bHasHipBone && HipBobHeight > 0.f)
	{
		// 一个步幅里起伏两次：左右脚各一次支撑，所以频率是摆腿的两倍
		FVector HipLocation = HipReferenceLocation;
		HipLocation.Z += HipBobHeight * FMath::Sin(Theta * 2.f);
		PoseableBody->SetBoneLocationByName(DeliveryPedestrianBones::Hips, HipLocation,
		                                    EBoneSpaces::ComponentSpace);
	}
}

void ADeliveryPedestrian::ReceiveImpact(const FVector& Impulse, const FVector& ImpactPoint)
{
	if (!HasAuthority())
	{
		return;
	}

	if (bKnockedDown)
	{
		// 已经躺着的人再被撞，只给可选布娃娃追加冲量，不重置起身计时。
		if (Mesh && Mesh->IsSimulatingPhysics())
		{
			Mesh->AddImpulseAtLocation(Impulse, ImpactPoint, DeliveryPedestrianBones::Spine);
		}
		return;
	}

	KnockImpulse = Impulse;
	KnockPoint = ImpactPoint;
	bKnockedDown = true;
	KnockedRemaining = FMath::Max(KnockedLieDuration, 0.f);
	Phase = EDeliveryPedestrianPhase::KnockedDown;

	// 服务器自己也要切：OnRep 只在客户端跑
	StartKnockdown();
	ForceNetUpdate();
}

void ADeliveryPedestrian::OnRep_KnockedDown()
{
	if (bKnockedDown)
	{
		StartKnockdown();
		return;
	}

	// 翻回 false 就是"服务器决定起身了"。**不另加一个要复制的状态**：
	// 这个 bool 本来就要复制，再加一个就有两个状态要保持一致，
	// 而不一致的表现是客户端那边人躺着不动、服务器上他已经走了
	if (Phase == EDeliveryPedestrianPhase::KnockedDown)
	{
		Phase = EDeliveryPedestrianPhase::StandingUp;
		StartGetUp();
	}
}

void ADeliveryPedestrian::StartKnockdown()
{
	BoidsVelocity = FVector::ZeroVector;
	Phase = EDeliveryPedestrianPhase::KnockedDown;
	FallElapsed = 0.f;
	bFallHorizontalBlocked = false;
	FallArcHeight = 0.f;
	FallStartLocation = GetActorLocation();
	FallStartRotation = GetActorQuat();
	bSimulatingKnockdown = bUsePhysicalKnockdown && ActiveMesh && ActiveMesh->GetPhysicsAsset();
	// 即使刚出生便受击，也要让倒地的人以正常大小留在场上。
	AppearAlpha = 1.f;
	ApplyAppearScale();

	// 倒地时胶囊仍跟着脚边的 Actor 原点，先关闭，起身后再恢复。
	if (HitBox)
	{
		HitBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	if (!bSimulatingKnockdown)
	{
		FVector Direction = KnockImpulse.GetSafeNormal2D();
		if (Direction.IsNearlyZero())
		{
			Direction = GetActorForwardVector().GetSafeNormal2D();
		}
		const FVector TiltAxis = FVector::CrossProduct(FVector::UpVector, Direction).GetSafeNormal();
		FallEndRotation = FQuat(TiltAxis, FMath::DegreesToRadians(78.f)) * FallStartRotation;
		// 普通倾倒保留原距离，车撞按冲量强度击离车头；位置仍只由服务器复制
		const float Strength = FMath::Clamp((KnockImpulse.Size2D() - KnockbackImpulseThreshold) /
		    FMath::Max(KnockbackImpulseReference - KnockbackImpulseThreshold, 1.f), 0.f, 1.f);
		const float Distance = FMath::Lerp(28.f, FMath::Max(KnockbackMaxDistance, 28.f), Strength);
		FallArcHeight = FMath::Max(KnockbackMaxArcHeight, 0.f) * Strength;
		FallEndLocation = FallStartLocation + Direction * Distance + FVector(0.f, 0.f, 14.f);
		if (Mesh && ActiveWalkAnim)
		{
			Mesh->SetPlayRate(0.f);
		}
		return;
	}

	if (!Mesh || !ActiveMesh)
	{
		return;
	}

	// 换到 SkeletalMesh 槽位：PoseableMesh 不能模拟物理，
	// 这也是当初把身体做成两个固定槽位的原因之一
	if (PoseableBody)
	{
		PoseableBody->SetVisibility(false);
	}
	Mesh->SetSkeletalMeshAsset(ActiveMesh);
	Mesh->SetVisibility(true);
	// 布娃娃要落到地上，所以这一下必须是真碰撞，不是查询。
	// 这套响应是照着引擎自带的 Ragdoll 配置改的，区别只在物体类型换成路人自己的通道：
	// 挡地面和墙（不然会穿地）、不挡 Pawn（玩家能从躺着的人身上走过去，
	// 挡住的表现是被一个躺着的人卡住）
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Mesh->SetCollisionObjectType(DeliveryCollision::Pedestrian);
	Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	Mesh->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Mesh->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	// 对自己这个通道保留反应，这样躺在地上的人还能被再打一拳、再撞一次
	Mesh->SetCollisionResponseToChannel(DeliveryCollision::Pedestrian, ECR_Block);
	Mesh->SetAllBodiesSimulatePhysics(true);
	Mesh->SetSimulatePhysics(true);
	Mesh->WakeAllRigidBodies();

	const FVector Impulse = KnockImpulse;
	if (!Impulse.IsNearlyZero())
	{
		Mesh->AddImpulseAtLocation(Impulse, KnockPoint, DeliveryPedestrianBones::Spine);
	}
}

void ADeliveryPedestrian::UpdateKnockdown(float DeltaTime)
{
	if (!bSimulatingKnockdown)
	{
		UpdateFallAnimation(DeltaTime);
	}
	// 起身时刻由服务器决定，客户端根据复制的状态切换。
	if (!HasAuthority())
	{
		return;
	}

	if (!bSimulatingKnockdown && GetFallAlpha() < 1.f)
	{
		return;
	}
	if (KnockedRemaining > 0.f)
	{
		KnockedRemaining = FMath::Max(0.f, KnockedRemaining - DeltaTime);
		return;
	}

	// 复制这一下就够了，客户端在 OnRep 里自己也切过去。
	bKnockedDown = false;
	Phase = EDeliveryPedestrianPhase::StandingUp;
	StartGetUp();
}

void ADeliveryPedestrian::UpdateFallAnimation(float DeltaTime)
{
	const bool bWasFalling = GetFallAlpha() < 1.f;
	FallElapsed = FMath::Min(FallElapsed + DeltaTime, FMath::Max(FallDuration, 0.1f));
	const float Alpha = GetFallAlpha();
	if (HasAuthority() && bWasFalling)
	{
		const float Smooth = FMath::SmoothStep(0.f, 1.f, Alpha);
		// 强撞击快速离开撞点再减速，避免原地缓慢翻倒；弱击仍沿用原节奏
		const float TravelAlpha = FallArcHeight > 0.f ? 1.f - FMath::Square(1.f - Alpha) : Smooth;
		FVector Desired = FMath::Lerp(FallStartLocation, FallEndLocation, TravelAlpha);
		if (bFallHorizontalBlocked)
		{
			Desired.X = GetActorLocation().X;
			Desired.Y = GetActorLocation().Y;
		}
		Desired.Z += FallArcHeight * FMath::Sin(PI * Alpha);
		float GroundZ;
		if (TraceGroundZ(Desired, GroundZ)) Desired.Z = FMath::Max(Desired.Z, GroundZ + 14.f * Smooth);
		// 根网格无碰撞且倒地胶囊已关闭，显式扫掠才能阻止击退穿墙；不扫玩家和车辆
		FCollisionObjectQueryParams Objects;
		Objects.AddObjectTypesToQuery(ECC_WorldStatic);
		Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
		FCollisionQueryParams Params(SCENE_QUERY_STAT(PedestrianKnockback), false, this);
		const FVector Offset(0.f, 0.f, 32.f);
		FHitResult Hit;
		if (GetWorld()->SweepSingleByObjectType(Hit, GetActorLocation() + Offset, Desired + Offset,
		    FQuat::Identity, Objects, FCollisionShape::MakeSphere(20.f), Params))
		{
			Desired = Hit.bStartPenetrating ? GetActorLocation() : Hit.Location - Offset;
			bFallHorizontalBlocked = true;
		}
		SetActorLocationAndRotation(Desired, FQuat::Slerp(FallStartRotation, FallEndRotation, Smooth));
	}
	// 身体倾倒的同时屈膝、前倾、双手向前撑；两端回到参考姿势，
	// 与原行走姿势和随后爬起姿势衔接时不会留下一帧手脚跳变。
	if (bProceduralGait && PoseableBody && !GaitReferenceRotations.IsEmpty())
	{
		const float Brace = FMath::Sin(Alpha * PI);
		const FVector PitchAxis = FVector::YAxisVector;
		AddBoneSwing(DeliveryPedestrianBones::LeftThigh, PitchAxis, -25.f * Brace);
		AddBoneSwing(DeliveryPedestrianBones::RightThigh, PitchAxis, -25.f * Brace);
		AddBoneSwing(DeliveryPedestrianBones::LeftCalf, PitchAxis, 30.f * Brace);
		AddBoneSwing(DeliveryPedestrianBones::RightCalf, PitchAxis, -30.f * Brace);
		AddBoneSwing(DeliveryPedestrianBones::Spine, PitchAxis, 18.f * Brace);
		const FQuat ArmBrace(PitchAxis, FMath::DegreesToRadians(-25.f * Brace));
		SetBoneDelta(DeliveryPedestrianBones::LeftArm, ArmBrace * LeftArmRestDelta);
		SetBoneDelta(DeliveryPedestrianBones::RightArm, ArmBrace * RightArmRestDelta);
	}
}

float ADeliveryPedestrian::GetGetUpAlpha() const
{
	return FMath::Clamp(GetUpElapsed / FMath::Max(GetUpDuration, 0.1f), 0.f, 1.f);
}

void ADeliveryPedestrian::StartGetUp()
{
	BoidsLateralOffset = 0.f;
	BoidsVelocity = FVector::ZeroVector;
	GetUpElapsed = 0.f;
	const bool bWasSimulating = bSimulatingKnockdown;
	bSimulatingKnockdown = false;

	// 躺着时骨盆在哪，那才是他该站起来的地方。
	// 取 actor 原点不行：根骨被物理带着飞，原点可能停在半空或者墙里
	FVector Pelvis = GetActorLocation();
	FVector BodyUp = bWasSimulating ? FVector::UpVector : GetActorUpVector();
	if (Mesh)
	{
		const int32 HipIndex = Mesh->GetBoneIndex(DeliveryPedestrianBones::Hips);
		const int32 SpineIndex = Mesh->GetBoneIndex(DeliveryPedestrianBones::Spine);
		if (HipIndex != INDEX_NONE)
		{
			Pelvis = Mesh->GetBoneLocation(DeliveryPedestrianBones::Hips);
			if (SpineIndex != INDEX_NONE)
			{
				// 身体的"上"方向按实测的骨盆→脊柱连线算，不写死"趴着就是 pitch 90"：
				// 他可能侧躺也可能仰面，写死一个方向会在一半的情况下先翻到另一边再起来
				const FVector Up =
				    (Mesh->GetBoneLocation(DeliveryPedestrianBones::Spine) - Pelvis).GetSafeNormal();
				if (!Up.IsNearlyZero())
				{
					BodyUp = Up;
				}
			}
		}

		// **先关物理再写变换**：顺序反了的话写进去的位置会被物理在同一帧拽回去
		Mesh->SetAllBodiesSimulatePhysics(false);
		Mesh->SetSimulatePhysics(false);
		Mesh->PutAllRigidBodiesToSleep();
		// 回到"布景"状态：路人不参与碰撞也不挡玩家，这和走路时的配置一致
		Mesh->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);

		if (bProceduralGait)
		{
			// 身体搬回 PoseableMesh；Mesh 是根组件，留着但不挂网格、不显示，
			// 和 BeginWalk 里程序化那条路的配置一样
			Mesh->SetSkeletalMeshAsset(nullptr);
			Mesh->SetVisibility(false);
			if (PoseableBody)
			{
				PoseableBody->SetVisibility(true);
			}
		}
		// 行走动画等起身完成再恢复，避免躺在地上踏步。
	}

	// 挨打的胶囊要还回来，否则站起来之后再也打不中他
	if (HitBox)
	{
		HitBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	}

	// 位置和朝向只由服务器算，客户端靠移动复制跟上（和走路时同一个分工）
	if (!HasAuthority())
	{
		return;
	}

	FVector Target = Pelvis;
	FVector Forward = GetActorForwardVector();
	if (ADeliveryPedestrianRoute* RoutePtr = Route.Get())
	{
		if (RoutePtr->bRoamInArea)
		{
			const FVector Center = RoutePtr->GetActorLocation();
			const FVector Offset = FVector(Pelvis.X - Center.X, Pelvis.Y - Center.Y, 0.f)
			    .GetClampedToMaxSize2D(FMath::Max(RoutePtr->RoamRadius, 50.f));
			Target = Center + Offset;
			RoamTarget = RoutePtr->PickRoamPoint(Target, 100.f);
			Forward = RoamTarget - Target;
		}
		else
		{
			// 特殊行人仍回到样条，供以后学校门口的过马路队伍使用。
			DistanceAlongRoute =
			    FMath::Clamp(RoutePtr->GetDistanceAtLocation(Pelvis), 0.f, RoutePtr->GetRouteLength());
			Target = RoutePtr->GetLocationAtDistance(DistanceAlongRoute);
			Forward = RoutePtr->GetDirectionAtDistance(DistanceAlongRoute);
			if (!bForward)
			{
				Forward = -Forward;
			}
		}
	}
	Forward.Z = 0.f;
	if (Forward.IsNearlyZero())
	{
		Forward = FVector::ForwardVector;
	}
	Forward = Forward.GetSafeNormal();

	float GroundZ = 0.f;
	FVector Start = Pelvis;
	if (TraceGroundZ(Pelvis, GroundZ))
	{
		Start.Z = GroundZ;
	}
	if (TraceGroundZ(Target, GroundZ))
	{
		Target.Z = GroundZ;
	}

	GetUpStartLocation = Start;
	GetUpTargetLocation = Target;
	// 起始朝向：Z 轴对着实测的身体上轴，X 轴尽量朝行进方向
	GetUpStartRotation = bWasSimulating
	    ? FRotationMatrix::MakeFromZX(BodyUp, Forward).ToQuat() : GetActorQuat();
	GetUpTargetRotation = Forward.Rotation().Quaternion();

	SetActorLocationAndRotation(GetUpStartLocation, GetUpStartRotation);
}

void ADeliveryPedestrian::UpdateGetUp(float DeltaTime)
{
	GetUpElapsed += DeltaTime;
	const float Alpha = GetGetUpAlpha();

	// 两端平滑：线性插值在起止两帧会有明显的折角
	const float Smooth = FMath::SmoothStep(0.f, 1.f, Alpha);

	if (HasAuthority())
	{
		SetActorLocationAndRotation(FMath::Lerp(GetUpStartLocation, GetUpTargetLocation, Smooth),
		                            FQuat::Slerp(GetUpStartRotation, GetUpTargetRotation, Smooth));
	}

	// 姿势：屈腿前倾撑起来。**钟形曲线、两端为零**——起点为零接上躺着的姿势，
	// 终点为零接上正常步态，中间最大读起来才是"蹲着撑起来"。
	// 这一段每台机器各跑一遍：骨骼姿势不在复制范围内
	if (bProceduralGait && PoseableBody && !GaitReferenceRotations.IsEmpty())
	{
		const float Bell = FMath::Sin(Alpha * PI);
		const FVector PitchAxis = FVector::YAxisVector;

		// 两腿同相屈起（走路那套是反相），因为这是双脚一起撑地
		AddBoneSwing(DeliveryPedestrianBones::LeftThigh, PitchAxis, -GetUpCrouchAngle * Bell);
		AddBoneSwing(DeliveryPedestrianBones::RightThigh, PitchAxis, -GetUpCrouchAngle * Bell);
		// 膝盖只往后弯，和步态那边同一个约束
		AddBoneSwing(DeliveryPedestrianBones::LeftCalf, PitchAxis, GetUpCrouchAngle * Bell);
		AddBoneSwing(DeliveryPedestrianBones::RightCalf, PitchAxis, -GetUpCrouchAngle * Bell);
		AddBoneSwing(DeliveryPedestrianBones::Spine, PitchAxis, GetUpTorsoLeanAngle * Bell);

		// 手臂保持垂着：起身中途把手举起来会像在投降
		SetBoneDelta(DeliveryPedestrianBones::LeftArm, LeftArmRestDelta);
		SetBoneDelta(DeliveryPedestrianBones::RightArm, RightArmRestDelta);
	}

	if (Alpha >= 1.f)
	{
		FinishGetUp();
	}
}

void ADeliveryPedestrian::FinishGetUp()
{
	Phase = EDeliveryPedestrianPhase::Walking;
	GetUpElapsed = 0.f;
	KnockedRemaining = 0.f;
	// 起身之后那一下缩放要回到原尺寸：被打飞那条路没碰过 AppearAlpha，
	// 但不起身那条路会把它缩下去，两条路共用这些字段，所以这里显式归位
	AppearAlpha = 1.f;
	ApplyAppearScale();
	if (Mesh && ActiveWalkAnim)
	{
		Mesh->PlayAnimation(ActiveWalkAnim, true);
		const float Rate = (AnimReferenceSpeed > 1.f) ? (WalkSpeed / AnimReferenceSpeed) : 1.f;
		Mesh->SetPlayRate(FMath::Clamp(Rate, 0.25f, 3.f));
	}

	if (HasAuthority())
	{
		// 刚站起来先别立刻又停下来看橱窗
		ScheduleNextPause();
		PauseRemaining = 0.f;
	}
}

bool ADeliveryPedestrian::IsInAppearWindow(float Length) const
{
	const ADeliveryPedestrianRoute* RoutePtr = Route.Get();
	if (!RoutePtr)
	{
		return false;
	}
	if (RoutePtr->bRoamInArea)
	{
		return RoamAppearElapsed < 0.5f;
	}
	const float Window = FMath::Max(RoutePtr->GetAppearDistance(), 1.f);
	const float FromEntry = bForward ? DistanceAlongRoute : (Length - DistanceAlongRoute);
	const float ToExit = bForward ? (Length - DistanceAlongRoute) : DistanceAlongRoute;
	return FromEntry < Window || ToExit < Window;
}

bool ADeliveryPedestrian::IsBlockedAhead(bool* bFixedObstacle) const
{
	if (bFixedObstacle)
	{
		*bFixedObstacle = false;
	}
	const UWorld* World = GetWorld();
	if (!World || YieldProbeDistance <= 0.f)
	{
		return false;
	}

	// 游走时转向是平滑的，探测要朝这一帧真正准备走的目标方向。
	// 否则刚换目标时会沿新方向穿过墙，探针却还在看旧方向。
	FVector Forward = GetActorForwardVector();
	if (const ADeliveryPedestrianRoute* RoutePtr = Route.Get(); RoutePtr && RoutePtr->bRoamInArea)
	{
		Forward = (RoamTarget - GetActorLocation()).GetSafeNormal2D();
		if (Forward.IsNearlyZero())
		{
			Forward = GetActorForwardVector();
		}
	}
	const FVector Start = GetActorLocation() + FVector(0.f, 0.f, YieldProbeHeight);
	const FVector End = Start + Forward * YieldProbeDistance;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(DeliveryPedestrianYield), false, this);
	Params.AddIgnoredActor(this);

	// 按**物体类型**查，不按通道查：这几个通道的默认响应是 Ignore，
	// 按通道查会被响应筛掉、扫描恒为空（和停车线那条、避让那条同一个坑）
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(DeliveryCollision::Pedestrian);  // 别的路人
	Objects.AddObjectTypesToQuery(ECC_PhysicsBody);                // 玩家的布娃娃
	Objects.AddObjectTypesToQuery(ECC_Pawn);                       // 玩家胶囊和摩托车
	Objects.AddObjectTypesToQuery(ECC_WorldStatic);                // 房屋、围墙、树石
	Objects.AddObjectTypesToQuery(ECC_WorldDynamic);               // 可移动场景物件

	TArray<FHitResult> Hits;
	World->SweepMultiByObjectType(Hits, Start, End, FQuat::Identity, Objects,
	                              FCollisionShape::MakeSphere(FMath::Max(YieldProbeRadius, 1.f)),
	                              Params);

	bool bBlocked = false;
	for (const FHitResult& Hit : Hits)
	{
		const AActor* Other = Hit.GetActor();
		if (!Other || Other == this)
		{
			continue;
		}
		// 车辆单向让行人，忽略车辆的所有碰撞组件，避免人车互等
		if (Cast<ADeliveryMotorbike>(Other) ||
		    Other->FindComponentByClass<UDeliveryTrafficCarComponent>() ||
		    (Hit.Component.IsValid() && Hit.Component->GetCollisionObjectType() == ECC_Vehicle))
		{
			continue;
		}
		// 躺在地上的不用让：绕不过去，而且他过几秒自己就起来了。
		// 不跳过的话一个躺着的人会把整条人行道堵死到 MaxYieldSeconds 为止
		if (const ADeliveryPedestrian* OtherPedestrian = Cast<ADeliveryPedestrian>(Other))
		{
			if (UsesBoids() && OtherPedestrian->Route == Route) continue;
			if (OtherPedestrian->IsKnockedDown() || OtherPedestrian->IsStandingUp())
			{
				continue;
			}
		}
		bBlocked = true;
		if (bFixedObstacle && Hit.Component.IsValid())
		{
			const ECollisionChannel ObjectType = Hit.Component->GetCollisionObjectType();
			*bFixedObstacle |= ObjectType == ECC_WorldStatic || ObjectType == ECC_WorldDynamic;
		}
	}
	return bBlocked;
}

void ADeliveryPedestrian::ScheduleNextPause()
{
	const float Min = FMath::Max(PauseIntervalMin, 0.f);
	const float Max = FMath::Max(PauseIntervalMax, Min);
	DistanceUntilPause = (Max <= 0.f) ? TNumericLimits<float>::Max() : FMath::FRandRange(Min, Max);
}

float ADeliveryPedestrian::UpdateWalkAndPause(float DeltaTime, float Length)
{
	// 正停着：倒计时走完就继续
	if (PauseRemaining > 0.f)
	{
		PauseRemaining -= DeltaTime;
		if (PauseRemaining <= 0.f)
		{
			PauseRemaining = 0.f;
			PauseYawOffset = 0.f;
			ScheduleNextPause();
		}
		return 0.f;
	}

	// 前方有人就停下来等。**让够 MaxYieldSeconds 就硬挤过去**：
	// 两个人面对面会互相判定"前面有人"、永久互相礼让，表现是两个人贴在一起不动
	// （交通车那边同一个坑，同一个解法）。
	// 出入场窗口里不让——那两个窗口按距离算缩放，在里面停住会卡成半个大小的人
	bool bFixedObstacle = false;
	const bool bBlocked =
	    bYieldToObstacles && !IsInAppearWindow(Length) && IsBlockedAhead(&bFixedObstacle);
	if (bBlocked)
	{
		YieldElapsed += DeltaTime;
		if (const ADeliveryPedestrianRoute* ActiveRoute = Route.Get();
		    bFixedObstacle && ActiveRoute && ActiveRoute->bRoamInArea)
		{
			ReverseRoamTarget();
			return 0.f;
		}
		// 固定场景物件不会像对向路人那样自行让开，不能超时硬挤穿墙。
		if (bFixedObstacle || YieldElapsed < FMath::Max(MaxYieldSeconds, 0.f))
		{
			// 返回 0 就够了：步态相位是按走过的路程推进的，速度为零腿自己就停，
			// 不会出现原地踏步
			return 0.f;
		}
	}
	else
	{
		// 间歇遮挡时逐渐退掉已等待时间，避免每隔一帧清零而永远让路。
		YieldElapsed = FMath::Max(0.f, YieldElapsed - DeltaTime * 0.5f);
	}

	const float Step = WalkSpeed * DeltaTime;
	DistanceUntilPause -= Step;
	if (DistanceUntilPause > 0.f || PauseDurationMax <= 0.f)
	{
		return Step;
	}

	// **出入场窗口里不许停**：那两个窗口按距离算缩放，在里面停住会卡成
	// 一个半大小的人站在原地不动，看着像渲染坏了
	const ADeliveryPedestrianRoute* RoutePtr = Route.Get();
	if (RoutePtr && RoutePtr->bRoamInArea)
	{
		const float DurMin = FMath::Max(PauseDurationMin, 0.f);
		const float DurMax = FMath::Max(PauseDurationMax, DurMin);
		PauseRemaining = FMath::FRandRange(DurMin, DurMax);
		PauseYawOffset = FMath::FRandRange(-PauseLookAroundAngle, PauseLookAroundAngle);
		return Step;
	}
	const float Window = RoutePtr ? FMath::Max(RoutePtr->GetAppearDistance(), 1.f) : 0.f;
	const float FromEntry = bForward ? DistanceAlongRoute : (Length - DistanceAlongRoute);
	const float ToExit = bForward ? (Length - DistanceAlongRoute) : DistanceAlongRoute;
	if (FromEntry < Window || ToExit < Window)
	{
		// 推迟到走出窗口再说，不是取消——取消的话每次都正好在端点附近被吃掉一次停顿
		DistanceUntilPause = Window;
		return Step;
	}

	const float DurMin = FMath::Max(PauseDurationMin, 0.f);
	const float DurMax = FMath::Max(PauseDurationMax, DurMin);
	PauseRemaining = FMath::FRandRange(DurMin, DurMax);
	PauseYawOffset = (PauseLookAroundAngle > 0.f)
	                     ? FMath::FRandRange(-PauseLookAroundAngle, PauseLookAroundAngle)
	                     : 0.f;
	return Step;
}

void ADeliveryPedestrian::UpdateIdleSway(float DeltaTime)
{
	if (!PoseableBody || GaitReferenceRotations.IsEmpty())
	{
		return;
	}

	// 步态相位是按**路程**推进的，停下来它自己就停了（腿不会原地踏步，这是想要的）。
	// 但完全不动看着像卡住，所以另起一个按**时间**走的慢晃动
	const float Period = FMath::Max(IdleSwayPeriod, 0.5f);
	IdlePhase = FMath::Fmod(IdlePhase + DeltaTime / Period, 1.f);
	const float Theta = IdlePhase * 2.f * PI;

	// 只晃躯干，不碰腿：腿一动就会和"脚钉在地上"矛盾，变成原地滑步。
	// 两轴要合成一个增量再写：分两次调 AddBoneSwing 的话第二次会把第一次抹掉
	const FQuat SwayRoll(FVector::XAxisVector,
	                     FMath::DegreesToRadians(IdleSwayAngle * FMath::Sin(Theta)));
	const FQuat SwayPitch(FVector::YAxisVector,
	                      FMath::DegreesToRadians(IdleSwayAngle * 0.4f * FMath::Sin(Theta * 2.f)));
	SetBoneDelta(DeliveryPedestrianBones::Spine, SwayPitch * SwayRoll);

	// 站着也得把手垂下来，否则停下来的人是平举着双手站在街边
	SetBoneDelta(DeliveryPedestrianBones::LeftArm, LeftArmRestDelta);
	SetBoneDelta(DeliveryPedestrianBones::RightArm, RightArmRestDelta);

	if (bHasHipBone && HipBobHeight > 0.f)
	{
		// 呼吸那点起伏，比走路时小一个量级
		FVector HipLocation = HipReferenceLocation;
		HipLocation.Z += HipBobHeight * 0.25f * FMath::Sin(Theta);
		PoseableBody->SetBoneLocationByName(DeliveryPedestrianBones::Hips, HipLocation,
		                                    EBoneSpaces::ComponentSpace);
	}
}

bool ADeliveryPedestrian::TryHandoff()
{
	ADeliveryPedestrianRoute* RoutePtr = Route.Get();
	if (!RoutePtr)
	{
		return false;
	}

	// 接驳次数有上限：不限的话开局那批人会一直在路网里转，
	// 路线的人数上限永远是满的、再也不生成新人，街上永远是同样几个面孔
	if (HandoffCount >= RoutePtr->MaxHandoffs)
	{
		return false;
	}

	// 从哪一头出去：正向走就是终点那头
	const bool bFromFinishEnd = bForward;

	ADeliveryPedestrianRoute* NextRoute = nullptr;
	bool bEnterAtStart = true;
	if (!RoutePtr->FindContinuation(bFromFinishEnd, NextRoute, bEnterAtStart) || !NextRoute)
	{
		return false;
	}

	// 先从旧路线的名单里摘掉再加进新的，顺序反了的话"同一个人同时算在两条路线上"，
	// 两边的人数上限都会被它占一个
	RoutePtr->NotifyPedestrianFinished(this);
	NextRoute->AcceptHandoff(this);

	Route = NextRoute;
	BoidsVelocity = FVector::ZeroVector;
	BoidsLateralOffset = 0.f;
	bForward = bEnterAtStart;
	DistanceAlongRoute = bEnterAtStart ? 0.f : NextRoute->GetRouteLength();
	++HandoffCount;
	// 换线进来的不再淡入：路人正站在新路线的端点上，照常走出入场窗口的话
	// 会在路口原地缩到看不见再长回来
	bEnteredByHandoff = true;
	Phase = EDeliveryPedestrianPhase::Walking;

	// 路口本来就是人会停一下的地方，顺便抽一次
	if (JunctionPauseChance > 0.f && FMath::FRand() < JunctionPauseChance)
	{
		const float DurMin = FMath::Max(PauseDurationMin, 0.f);
		const float DurMax = FMath::Max(PauseDurationMax, DurMin);
		PauseRemaining = FMath::FRandRange(DurMin, DurMax);
		PauseYawOffset = (PauseLookAroundAngle > 0.f)
		                     ? FMath::FRandRange(-PauseLookAroundAngle, PauseLookAroundAngle)
		                     : 0.f;
	}
	ScheduleNextPause();

	return true;
}
