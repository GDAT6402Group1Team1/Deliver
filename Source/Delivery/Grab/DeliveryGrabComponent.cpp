#include "Grab/DeliveryGrabComponent.h"

#include "Delivery.h"
#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DeliveryCharacter.h"
#include "Combat/DeliveryRagdollCombatComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Grab/DeliveryGrabbableComponent.h"
#include "Net/UnrealNetwork.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"

namespace
{
	constexpr uint8 LeftBit = 1;
	constexpr uint8 RightBit = 2;
	const FName HandBones[2] = { TEXT("LeftHand"), TEXT("RightHand") };
}

UDeliveryGrabComponent::UDeliveryGrabComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	SetIsReplicatedByDefault(true);
}

void UDeliveryGrabComponent::BeginPlay()
{
	Super::BeginPlay();
	if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner()))
	{
		if (UDeliveryActiveRagdollComponent* Ragdoll = Character->GetActiveRagdoll())
		{
			Ragdoll->AddTickPrerequisiteComponent(this);
		}
	}
}

void UDeliveryGrabComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ForceRelease();
	ClearHighlight();
	Super::EndPlay(EndPlayReason);
}

void UDeliveryGrabComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UDeliveryGrabComponent, GrabTarget);
	DOREPLIFETIME(UDeliveryGrabComponent, GrabBone);
	DOREPLIFETIME(UDeliveryGrabComponent, GripLocalLeft);
	DOREPLIFETIME(UDeliveryGrabComponent, GripLocalRight);
	DOREPLIFETIME(UDeliveryGrabComponent, WantedHands);
	DOREPLIFETIME(UDeliveryGrabComponent, AttachedHands);
	DOREPLIFETIME(UDeliveryGrabComponent, CarryForward);
}

bool UDeliveryGrabComponent::IsCarryingProp() const
{
	return IsValid(GrabTarget.Get()) && AttachedHands != 0
		&& !GrabTarget->IsA<ADeliveryCharacter>();
}

bool UDeliveryGrabComponent::IsDraggingCharacter() const
{
	return IsValid(GrabTarget.Get()) && GrabTarget->IsA<ADeliveryCharacter>() && WantedHands != 0;
}

bool UDeliveryGrabComponent::FindCandidate(AActor*& OutActor, FVector& OutPoint) const
{
	OutActor = nullptr;
	const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner());
	if (!Character || !Character->GetFollowCamera() || !GetWorld()) return false;
	const FVector Start = Character->GetFollowCamera()->GetComponentLocation();
	const FVector Aim = Character->GetFollowCamera()->GetForwardVector();
	const FVector End = Start + Aim * 1000.0f;
	const FVector Hips = Character->GetMesh()->GetBoneLocation(TEXT("Hips"));
	FCollisionQueryParams Params(SCENE_QUERY_STAT(GrabAim), false, Character);
	FHitResult Hit;
	auto IsGrabHit = [&](const FHitResult& CandidateHit)
	{
		const AActor* Target = CandidateHit.GetActor();
		const UDeliveryGrabbableComponent* Grabbable = Target
			? Target->FindComponentByClass<UDeliveryGrabbableComponent>() : nullptr;
		return Grabbable && Grabbable->GrabberCount < 2 && Grabbable->CanGrab(Character)
			&& FVector::DistSquared(Hips, CandidateHit.ImpactPoint) <= FMath::Square(GrabDistance);
	};
	if (GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params) && IsGrabHit(Hit))
	{
		OutActor = Hit.GetActor();
		OutPoint = Hit.ImpactPoint;
		return true;
	}

	// 准星稍微偏离小物品时仍可选中：只考虑身边的物理目标，且镜头到目标必须无遮挡。
	FCollisionObjectQueryParams ObjectTypes;
	ObjectTypes.AddObjectTypesToQuery(ECC_PhysicsBody);
	ObjectTypes.AddObjectTypesToQuery(ECC_WorldDynamic);
	TArray<FOverlapResult> Nearby;
	GetWorld()->OverlapMultiByObjectType(Nearby, Hips, FQuat::Identity, ObjectTypes,
		FCollisionShape::MakeSphere(GrabDistance + HighlightAimMargin), Params);
	float BestMiss = TNumericLimits<float>::Max();
	for (const FOverlapResult& Overlap : Nearby)
	{
		AActor* Target = Overlap.GetActor();
		UDeliveryGrabbableComponent* Grabbable = Target
			? Target->FindComponentByClass<UDeliveryGrabbableComponent>() : nullptr;
		if (!Grabbable || Grabbable->GrabberCount >= 2 || !Grabbable->CanGrab(Character)) continue;
		FName Bone;
		const UPrimitiveComponent* Body = Grabbable->GetGrabBody(Bone);
		if (!Body) continue;
		const FVector Focus = Bone.IsNone() ? Body->Bounds.Origin : Body->GetSocketLocation(Bone);
		const float AlongRay = FVector::DotProduct(Focus - Start, Aim);
		if (AlongRay <= 0.0f || AlongRay > 1000.0f) continue;
		const float Miss = FVector::Dist(Focus, Start + Aim * AlongRay);
		const float AllowedMiss = FMath::Clamp(Body->Bounds.SphereRadius * 0.5f
			+ HighlightAimMargin, 35.0f, 70.0f);
		if (Miss > AllowedMiss || Miss >= BestMiss) continue;
		FHitResult SightHit;
		if (!GetWorld()->LineTraceSingleByChannel(SightHit, Start, Focus, ECC_Visibility, Params)
			|| SightHit.GetActor() != Target || !IsGrabHit(SightHit)) continue;
		BestMiss = Miss;
		OutActor = Target;
		OutPoint = SightHit.ImpactPoint;
	}
	return OutActor != nullptr;
}

void UDeliveryGrabComponent::RequestBegin()
{
	ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner());
	if (!Character || !Character->IsLocallyControlled() || IsGrabbing()
		|| (Character->GetRagdollCombat() && Character->GetRagdollCombat()->IsPunching())) return;
	AActor* Candidate = nullptr;
	FVector HitPoint;
	if (!FindCandidate(Candidate, HitPoint)) return;
	PredictedTarget = Candidate;
	PredictedGripPoint = HitPoint;
	if (Candidate->IsA<ADeliveryCharacter>())
	{
		const FVector LeftHand = Character->GetMesh()->GetBoneLocation(HandBones[0]);
		const FVector RightHand = Character->GetMesh()->GetBoneLocation(HandBones[1]);
		PredictedHands = FVector::DistSquared(LeftHand, HitPoint)
			<= FVector::DistSquared(RightHand, HitPoint) ? LeftBit : RightBit;
	}
	else PredictedHands = LeftBit | RightBit;
	LocallyReleasedHands = 0;
	PredictionExpiresAt = GetWorld()->GetTimeSeconds() + 0.5f;
	if (Character->HasAuthority()) BeginOnServer(Candidate, HitPoint);
	else ServerRequestBegin(Candidate, HitPoint);
}

void UDeliveryGrabComponent::ServerRequestBegin_Implementation(AActor* Target, FVector_NetQuantize10 HitPoint)
{
	BeginOnServer(Target, HitPoint);
}

void UDeliveryGrabComponent::BeginOnServer(AActor* Target, const FVector& HitPoint)
{
	ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner());
	UDeliveryGrabbableComponent* Grabbable = IsValid(Target)
		? Target->FindComponentByClass<UDeliveryGrabbableComponent>() : nullptr;
	if (!Character || !Character->HasAuthority() || GrabTarget || !Grabbable
		|| !FMath::IsFinite(HitPoint.X) || !FMath::IsFinite(HitPoint.Y) || !FMath::IsFinite(HitPoint.Z)
		|| (Character->GetRagdollCombat() && Character->GetRagdollCombat()->IsPunching())
		|| !Character->GetActiveRagdoll()
		|| Character->GetActiveRagdoll()->GetControlMode() != EDeliveryRagdollControlMode::Active)
	{
		return;
	}
	FName Bone;
	UPrimitiveComponent* Body = Grabbable->GetGrabBody(Bone);
	if (!Body || !Grabbable->CanGrab(Character)) return;
	int32 DragSide = INDEX_NONE;
	FVector DragPoint = HitPoint;
	const bool bGrabCharacter = Target->IsA<ADeliveryCharacter>();
	if (const ADeliveryCharacter* DraggedCharacter = Cast<ADeliveryCharacter>(Target))
	{
		if (!FindClosestDragGrip(DraggedCharacter, DragSide, Bone, DragPoint)) return;
	}
	const FVector Hips = Character->GetMesh()->GetBoneLocation(TEXT("Hips"));
	const FVector ValidatedPoint = bGrabCharacter ? DragPoint : HitPoint;
	if (FVector::Dist(Hips, ValidatedPoint) > GrabDistance)
	{
		UE_LOG(LogDelivery, Warning, TEXT("Grab rejected: target %s is %.1f cm from hips (limit %.1f)"),
			*GetNameSafe(Target), FVector::Dist(Hips, ValidatedPoint), GrabDistance);
		return;
	}
	// 客户端只提交意图；倒地身体的姿态可能与客户端快照略有差异，
	// 因此服务端以自己选出的身体表面重新核对距离、朝向和无遮挡视线。
	const FVector Eye = Character->GetPawnViewLocation();
	const FVector ToPoint = (ValidatedPoint - Eye).GetSafeNormal();
	const FVector Facing = Character->GetController()
		? Character->GetController()->GetControlRotation().Vector() : Character->GetActorForwardVector();
	if (FVector::DotProduct(ToPoint, Facing) < (bGrabCharacter ? 0.2f : 0.35f))
	{
		UE_LOG(LogDelivery, Warning, TEXT("Grab rejected: target %s is outside server facing"),
			*GetNameSafe(Target));
		return;
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ServerGrabAim), false, Character);
	auto HasClearSight = [&](const FVector& Start)
	{
		const FVector Direction = (ValidatedPoint - Start).GetSafeNormal();
		FHitResult SightHit;
		return GetWorld()->LineTraceSingleByChannel(SightHit, Start,
			ValidatedPoint + Direction * 10.0f, ECC_Visibility, Params)
			&& SightHit.GetActor() == Target
			&& (bGrabCharacter || FVector::Dist(SightHit.ImpactPoint, HitPoint) <= 55.0f);
	};
	const bool bVisible = HasClearSight(Eye)
		|| (bGrabCharacter && Character->GetFollowCamera()
			&& HasClearSight(Character->GetFollowCamera()->GetComponentLocation()));
	if (!bVisible || !Grabbable->RegisterGrabber(Character))
	{
		UE_LOG(LogDelivery, Warning, TEXT("Grab rejected: target %s %s"),
			*GetNameSafe(Target), bVisible ? TEXT("registration failed") : TEXT("server sight blocked"));
		return;
	}
	if (Grabbable->bKinematicCarry && Grabbable->GrabberCount == 1)
	{
		// 第一次抬起时先把箱子朝向对齐身体，再把两个接触点存成局部坐标。
		// 否则手记住的是箱子旧朝向的那一面，箱子转正后手会追到侧面甚至背面。
		const FVector BodyForward = Character->GetActiveRagdoll()->GetBodyForward().GetSafeNormal2D();
		if (!BodyForward.IsNearlyZero())
		{
			Target->SetActorRotation(FRotator(0.0f, BodyForward.Rotation().Yaw, 0.0f),
				ETeleportType::TeleportPhysics);
		}
	}
	GrabTarget = Target;
	GrabBone = Bone;
	CarryForward = Character->GetController()
		? Character->GetController()->GetControlRotation().Vector().GetSafeNormal2D()
		: Character->GetActorForwardVector().GetSafeNormal2D();
	// A ragdoll joint is solved in rigid-body space. Keep the replicated grip in that
	// same space; the skeletal socket pose can lag behind the Chaos body during drag.
	const FBodyInstance* DragBodyInstance = bGrabCharacter
		? Body->GetBodyInstance(Bone) : nullptr;
	const FTransform BodyTransform = DragBodyInstance
		? DragBodyInstance->GetUnrealWorldTransform() : Body->GetSocketTransform(Bone);
	FVector LeftPoint = HitPoint;
	FVector RightPoint = HitPoint;
	if (!Target->IsA<ADeliveryCharacter>())
	{
		FindUndersideGripPoints(Body,
			Character->GetActiveRagdoll()->GetBodyForward().GetSafeNormal2D(), LeftPoint, RightPoint);
	}
	else
	{
		LeftPoint = DragPoint;
		RightPoint = DragPoint;
	}
	GripLocalLeft = BodyTransform.InverseTransformPosition(LeftPoint);
	GripLocalRight = BodyTransform.InverseTransformPosition(RightPoint);
	WantedHands = DragSide == INDEX_NONE ? LeftBit | RightBit
		: (DragSide == 0 ? LeftBit : RightBit);
	AttachedHands = Grabbable->bKinematicCarry ? WantedHands : 0;
	Character->ForceNetUpdate();
}

bool UDeliveryGrabComponent::FindClosestDragGrip(const ADeliveryCharacter* Target,
	int32& OutSide, FName& OutBone, FVector& OutPoint) const
{
	const ADeliveryCharacter* Grabber = Cast<ADeliveryCharacter>(GetOwner());
	const USkeletalMeshComponent* TargetMesh = Target ? Target->GetMesh() : nullptr;
	const UPhysicsAsset* Asset = TargetMesh ? TargetMesh->GetPhysicsAsset() : nullptr;
	if (!Grabber || !Asset) return false;
	float BestDistanceSq = TNumericLimits<float>::Max();
	for (const USkeletalBodySetup* Setup : Asset->SkeletalBodySetups)
	{
		if (!Setup || !TargetMesh->IsSimulatingPhysics(Setup->BoneName)) continue;
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const FVector Hand = Grabber->GetMesh()->GetBoneLocation(HandBones[Side]);
			FVector Surface;
			if (TargetMesh->GetClosestPointOnCollision(Hand, Surface, Setup->BoneName) <= 0.0f)
			{
				Surface = TargetMesh->GetBoneLocation(Setup->BoneName);
			}
			const float DistanceSq = FVector::DistSquared(Hand, Surface);
			if (DistanceSq >= BestDistanceSq) continue;
			BestDistanceSq = DistanceSq;
			OutSide = Side;
			OutBone = Setup->BoneName;
			OutPoint = Surface;
		}
	}
	return OutSide != INDEX_NONE;
}

UPrimitiveComponent* UDeliveryGrabComponent::GetTargetBody() const
{
	const UDeliveryGrabbableComponent* Grabbable = IsValid(GrabTarget.Get())
		? GrabTarget->FindComponentByClass<UDeliveryGrabbableComponent>() : nullptr;
	FName Bone;
	return Grabbable ? Grabbable->GetGrabBody(Bone) : nullptr;
}

FVector UDeliveryGrabComponent::GripWorld(int32 Side) const
{
	if (UPrimitiveComponent* Body = GetTargetBody())
	{
		const FBodyInstance* DragBodyInstance = GrabTarget->IsA<ADeliveryCharacter>()
			? Body->GetBodyInstance(GrabBone) : nullptr;
		const FTransform BodyTransform = DragBodyInstance
			? DragBodyInstance->GetUnrealWorldTransform() : Body->GetSocketTransform(GrabBone);
		return BodyTransform.TransformPosition(
			Side == 0 ? FVector(GripLocalLeft) : FVector(GripLocalRight));
	}
	return FVector::ZeroVector;
}

void UDeliveryGrabComponent::FindUndersideGripPoints(const UPrimitiveComponent* Body,
	const FVector& Forward, FVector& OutLeft, FVector& OutRight) const
{
	// 从物体底面两侧向上查最近的真实碰撞表面，不假设网格原点位于中心，
	// 也不假设它是固定尺寸的立方体。两个抓点只在开始抓取时计算一次。
	const FBoxSphereBounds& Bounds = Body->Bounds;
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward).GetSafeNormal();
	const float HalfSpacing = FMath::Clamp(
		FMath::Min(Bounds.BoxExtent.X, Bounds.BoxExtent.Y) * 0.7f, 5.0f, 18.0f);
	const FVector Probe = Bounds.Origin
		- FVector::UpVector * (Bounds.BoxExtent.Z + 30.0f)
		- Forward * FMath::Min(Bounds.SphereRadius * 0.2f, 20.0f);
	const FVector Fallback = Bounds.Origin - FVector::UpVector * Bounds.BoxExtent.Z;
	FVector Surface;
	OutLeft = Body->GetClosestPointOnCollision(Probe - Right * HalfSpacing, Surface) > 0.0f
		? Surface : Fallback - Right * HalfSpacing;
	OutRight = Body->GetClosestPointOnCollision(Probe + Right * HalfSpacing, Surface) > 0.0f
		? Surface : Fallback + Right * HalfSpacing;
}

FVector UDeliveryGrabComponent::CarryCenterWorld(const UPrimitiveComponent* Body) const
{
	const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner());
	if (!Character || !Body) return FVector::ZeroVector;
	FVector Forward = Character->GetActiveRagdoll()
		? Character->GetActiveRagdoll()->GetBodyForward().GetSafeNormal2D()
		: FVector(CarryForward).GetSafeNormal2D();
	if (Forward.IsNearlyZero()) Forward = Character->GetActorForwardVector().GetSafeNormal2D();
	// 先给双手一个胸口前的稳定支撑位置，再根据物体真实底面抓点反推物体中心。
	// 因而无论网格大小或 Pivot 在哪里，物体都在双手之上，而不是悬在手下。
	const FVector HandSupport = Character->GetMesh()->GetBoneLocation(TEXT("Spine"))
		+ Forward * CarryHandDistance + FVector::UpVector * CarryHeightAboveSpine;
	const FVector LocalGripMidpoint = (FVector(GripLocalLeft) + FVector(GripLocalRight)) * 0.5f;
	const FQuat Rotation = FRotator(0.0f, Forward.Rotation().Yaw, 0.0f).Quaternion();
	const FTransform Orientation(Rotation, FVector::ZeroVector, Body->GetComponentScale());
	return HandSupport - Orientation.TransformVector(LocalGripMidpoint);
}

bool UDeliveryGrabComponent::GetCarryPoseFor(const AActor* Target, FVector& OutCenter, FVector& OutForward) const
{
	if (!IsValid(GrabTarget.Get()) || GrabTarget.Get() != Target || !WantedHands
		|| Target->IsA<ADeliveryCharacter>()) return false;
	const UPrimitiveComponent* Body = GetTargetBody();
	if (!Body) return false;
	OutCenter = CarryCenterWorld(Body);
	const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner());
	OutForward = Character && Character->GetActiveRagdoll()
		? Character->GetActiveRagdoll()->GetBodyForward().GetSafeNormal2D()
		: FVector(CarryForward).GetSafeNormal2D();
	return true;
}

FVector UDeliveryGrabComponent::FilterApproachWish(const FVector& Wish) const
{
	const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner());
	const AActor* Target = IsValid(GrabTarget.Get()) ? GrabTarget.Get() : PredictedTarget.Get();
	if (!Character || !Target || Target->IsA<ADeliveryCharacter>() || AttachedHands == (LeftBit | RightBit)
		|| Wish.IsNearlyZero()) return Wish;
	const UDeliveryGrabbableComponent* Marker = Target->FindComponentByClass<UDeliveryGrabbableComponent>();
	FName Bone;
	const UPrimitiveComponent* Body = Marker ? Marker->GetGrabBody(Bone) : nullptr;
	if (!Body) return Wish;
	const FVector Hips = Character->GetMesh()->GetBoneLocation(TEXT("Hips"));
	FVector Surface;
	if (Body->GetClosestPointOnCollision(Hips, Surface) <= 0.0f) return Wish;
	const FVector Toward = (Surface - Hips).GetSafeNormal2D();
	const float TowardAmount = FVector::DotProduct(Wish, Toward);
	if (TowardAmount <= 0.0f) return Wish;
	const float Distance = FVector::Dist2D(Hips, Surface);
	const float Scale = FMath::Clamp((Distance - ApproachStopDistance)
		/ FMath::Max(ApproachSlowDistance - ApproachStopDistance, 1.0f), 0.0f, 1.0f);
	return Wish - Toward * (TowardAmount * (1.0f - Scale));
}

bool UDeliveryGrabComponent::GetHandGoal(bool bLeft, FVector& OutGoal) const
{
	const int32 Side = bLeft ? 0 : 1;
	const uint8 Bit = bLeft ? LeftBit : RightBit;
	if (IsValid(GrabTarget.Get()) && (WantedHands & Bit) && !(LocallyReleasedHands & Bit))
	{
		OutGoal = GripWorld(Side);
		// 单人持物时，手和箱子取同一个胸口相对托举目标。若手追箱子已平滑后的
		// 世界位置，箱子每次滞后都会把肩膀朝反方向拉，引起整段躯干扭动。
		if (const UDeliveryGrabbableComponent* Marker = GrabTarget->FindComponentByClass<UDeliveryGrabbableComponent>();
			Marker && Marker->bKinematicCarry && Marker->GrabberCount == 1)
		{
			FVector Center, Forward;
			if (GetCarryPoseFor(GrabTarget.Get(), Center, Forward))
			{
				if (const UPrimitiveComponent* Body = GetTargetBody())
				{
					const FQuat Rotation = FRotator(0.0f, Forward.Rotation().Yaw, 0.0f).Quaternion();
					const FTransform CarryTransform(Rotation, Center, Body->GetComponentScale());
					OutGoal = CarryTransform.TransformPosition(
						Side == 0 ? FVector(GripLocalLeft) : FVector(GripLocalRight));
				}
			}
		}
		return !OutGoal.IsNearlyZero();
	}
	if (PredictedTarget.IsValid() && (PredictedHands & Bit))
	{
		OutGoal = PredictedGripPoint + CastChecked<ADeliveryCharacter>(GetOwner())->GetActorRightVector()
			* (bLeft ? -10.0f : 10.0f);
		return true;
	}
	return false;
}

bool UDeliveryGrabComponent::GetGrabFacingDirection(FVector& OutDirection) const
{
	const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner());
	if (!Character) return false;
	const FVector Hips = Character->GetMesh()->GetBoneLocation(TEXT("Hips"));
	if (IsCarryingProp() && !FVector(CarryForward).IsNearlyZero())
	{
		OutDirection = FVector(CarryForward).GetSafeNormal2D();
		return true;
	}
	const int32 Side = (WantedHands & RightBit) ? 1 : 0;
	const FVector Point = IsValid(GrabTarget.Get()) ? GripWorld(Side) : PredictedGripPoint;
	if (!IsValid(GrabTarget.Get()) && !PredictedTarget.IsValid()) return false;
	const FVector ToGrip = FVector::VectorPlaneProject(Point - Hips, FVector::UpVector);
	if (ToGrip.SizeSquared() < FMath::Square(30.0f)) return false;
	OutDirection = ToGrip.GetSafeNormal();
	return true;
}

void UDeliveryGrabComponent::TryAttach(int32 Side)
{
	ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner());
	UPrimitiveComponent* Body = GetTargetBody();
	if (!Character || !Body || HandConstraints[Side]) return;
	const uint8 Bit = Side == 0 ? LeftBit : RightBit;
	if (!(WantedHands & Bit)) return;
	const FVector HandPoint = Character->GetMesh()->GetBoneLocation(HandBones[Side]);
	FVector Point = GripWorld(Side);
	const bool bGrabCharacter = GrabTarget->IsA<ADeliveryCharacter>();
	bool bSnappedCharacter = false;
	if (!bGrabCharacter && FVector::Dist(HandPoint, Point) > AttachDistance) return;
	if (bGrabCharacter)
	{
		const UDeliveryGrabbableComponent* Marker = GrabTarget->FindComponentByClass<UDeliveryGrabbableComponent>();
		// 第一个抓人者：整条物理刚体链平移，让选中的身体抓点直接贴到手上。
		// 第二个人加入争抢时不能再瞬移目标，否则会扯断第一人的 joint；仍使用柔性连接。
		if (Marker && Marker->GrabberCount == 1)
		{
			USkeletalMeshComponent* TargetMesh = Cast<USkeletalMeshComponent>(Body);
			FBodyInstance* RootBody = TargetMesh ? TargetMesh->GetBodyInstance() : nullptr;
			if (!RootBody)
			{
				ForceRelease();
				return;
			}
			FCollisionObjectQueryParams Blockers;
			Blockers.AddObjectTypesToQuery(ECC_WorldStatic);
			Blockers.AddObjectTypesToQuery(ECC_WorldDynamic);
			FCollisionQueryParams Params(SCENE_QUERY_STAT(DragSnapPath), false, Character);
			Params.AddIgnoredActor(GrabTarget.Get());
			FHitResult Blocker;
			if (GetWorld()->LineTraceSingleByObjectType(Blocker, Point, HandPoint, Blockers, Params))
			{
				UE_LOG(LogDelivery, Warning, TEXT("Drag snap blocked by %s at %s"),
					*GetNameSafe(Blocker.GetActor()), *Blocker.ImpactPoint.ToCompactString());
				ForceRelease();
				return;
			}
			const FVector Offset = HandPoint - Point;
			TargetMesh->SetAllPhysicsPosition(
				RootBody->GetUnrealWorldTransform().GetLocation() + Offset);
			GrabTarget->ForceNetUpdate();
			UE_LOG(LogDelivery, Log, TEXT("Drag snapped %s by %s (offset %.1f cm)"),
				*GetNameSafe(GrabTarget.Get()), *GetNameSafe(Character), Offset.Size());
			Point = HandPoint;
			bSnappedCharacter = true;
		}
	}
	else
	{
		// 普通物品仍在实际接触时固定局部抓点。
		const FVector LocalPoint = Body->GetSocketTransform(GrabBone).InverseTransformPosition(Point);
		if (Side == 0) GripLocalLeft = LocalPoint;
		else GripLocalRight = LocalPoint;
	}
	UPhysicsConstraintComponent* Constraint = NewObject<UPhysicsConstraintComponent>(Character);
	Constraint->RegisterComponent();
	Constraint->SetWorldLocation(Point);
	Constraint->SetDisableCollision(true);
	Constraint->SetLinearXLimit(bGrabCharacter && !bSnappedCharacter ? LCM_Free : LCM_Locked, 0.0f);
	Constraint->SetLinearYLimit(bGrabCharacter && !bSnappedCharacter ? LCM_Free : LCM_Locked, 0.0f);
	Constraint->SetLinearZLimit(bGrabCharacter && !bSnappedCharacter ? LCM_Free : LCM_Locked, 0.0f);
	// 单手拖人要允许对方自然翻滚，不额外拧紧肩膀和躯干。
	Constraint->SetAngularSwing1Limit(ACM_Free, 0.0f);
	Constraint->SetAngularSwing2Limit(ACM_Free, 0.0f);
	Constraint->SetAngularTwistLimit(ACM_Free, 0.0f);
	Constraint->SetLinearBreakable(true, bGrabCharacter ? 120000.0f : 80000.0f);
	Constraint->SetConstrainedComponents(Character->GetMesh(), HandBones[Side], Body, GrabBone);
	if (bGrabCharacter && !Constraint->ConstraintInstance.IsValidConstraintInstance())
	{
		UE_LOG(LogDelivery, Warning, TEXT("Drag joint could not bind %s to %s"),
			*HandBones[Side].ToString(), *GrabBone.ToString());
		Constraint->DestroyComponent();
		ForceRelease();
		return;
	}
	// 默认两侧参考帧都取抓点，会永久保留“手还差 20–35 cm”的间隔。
	// 改为把手骨骼的真实位置锁到物体抓点，消除视觉上的隔空抓取。
	if (const FBodyInstance* HandBody = Character->GetMesh()->GetBodyInstance(HandBones[Side]))
	{
		Constraint->SetConstraintReferencePosition(EConstraintFrame::Frame1,
			HandBody->GetUnrealWorldTransform().InverseTransformPosition(HandPoint));
	}
	if (bGrabCharacter)
	{
		// Both the visual grip and the joint's second anchor now refer to the same
		// physics body point, so they cannot diverge when the ragdoll animates.
		Constraint->SetConstraintReferencePosition(EConstraintFrame::Frame2,
			Side == 0 ? FVector(GripLocalLeft) : FVector(GripLocalRight));
		if (bSnappedCharacter)
		{
			// Locked joints can accumulate a large soft error between two full ragdolls.
			// Projection is an emergency correction, not a continuous pulling motor.
			Constraint->SetProjectionParams(0.0f, 0.0f, 15.0f, 180.0f);
			Constraint->SetProjectionEnabled(true);
		}
	}
	if (bGrabCharacter && !bSnappedCharacter)
	{
		// 多人争抢时第二人的两端从当前真实位置开始，有限力靠近后才硬锁。
		Constraint->SetLinearDriveParams(DragReelStrength, DragReelDamping, DragReelForceLimit);
		Constraint->SetLinearPositionTarget(FVector::ZeroVector);
		Constraint->SetLinearPositionDrive(true, true, true);
		bDragReeling[Side] = true;
	}
	HandConstraints[Side] = Constraint;
	AttachedHands |= Bit;
	UE_LOG(LogDelivery, Log, TEXT("Grab joint attached: grabber=%s target=%s side=%d snapped=%d initialGap=%.1f"),
		*GetNameSafe(Character), *GetNameSafe(GrabTarget.Get()), Side, bSnappedCharacter ? 1 : 0,
		FVector::Dist(HandPoint, GripWorld(Side)));
	Character->ForceNetUpdate();
}

void UDeliveryGrabComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner());
	if (!Character || !GetWorld()) return;
	LeftHandGap = (AttachedHands & LeftBit)
		? FVector::Dist(Character->GetMesh()->GetBoneLocation(HandBones[0]),
			GripWorld(0)) : 0.0f;
	RightHandGap = (AttachedHands & RightBit)
		? FVector::Dist(Character->GetMesh()->GetBoneLocation(HandBones[1]),
			GripWorld(1)) : 0.0f;
	if (Character->IsLocallyControlled())
	{
		AActor* Candidate = nullptr;
		FVector Point;
		if (!IsGrabbing()) FindCandidate(Candidate, Point);
		UpdateHighlight(Candidate);
	}
	if (PredictedTarget.IsValid()
		&& (GrabTarget || GetWorld()->GetTimeSeconds() >= PredictionExpiresAt))
	{
		PredictedTarget.Reset();
		PredictedHands = 0;
	}
	if (Character->IsLocallyControlled() && Character->IsGrabChordHeld() && !IsGrabbing())
	{
		// 双键一直按着时持续寻找目标；第一次按下时没对准箱子也不必松手重按。
		RequestBegin();
	}
	LocallyReleasedHands &= WantedHands;
	if (!Character->HasAuthority() || !GrabTarget) return;
	if (!IsValid(GrabTarget.Get()))
	{
		ForceRelease();
		return;
	}
	UDeliveryGrabbableComponent* Grabbable = GrabTarget->FindComponentByClass<UDeliveryGrabbableComponent>();
	if (!Grabbable || !Grabbable->CanGrab(Character)
		|| Character->GetActiveRagdoll()->GetControlMode() != EDeliveryRagdollControlMode::Active)
	{
		ForceRelease();
		return;
	}
	if (Character->GetController())
	{
		const FVector NewForward = Character->GetController()->GetControlRotation().Vector().GetSafeNormal2D();
		if (!NewForward.IsNearlyZero() && FVector::DotProduct(NewForward, FVector(CarryForward)) < 0.995f)
		{
			CarryForward = NewForward;
			Character->ForceNetUpdate();
		}
	}
	if (Grabbable->bKinematicCarry) return;
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const uint8 Bit = Side == 0 ? LeftBit : RightBit;
		if (!(WantedHands & Bit)) continue;
		if (HandConstraints[Side] && HandConstraints[Side]->IsBroken())
		{
			UE_LOG(LogDelivery, Warning, TEXT("Grab joint broke: grabber=%s target=%s side=%d"),
				*GetNameSafe(Character), *GetNameSafe(GrabTarget.Get()), Side);
			ReleaseHandOnServer(Side);
			if (!GrabTarget) return;
			continue;
		}
		const float Separation = FVector::Dist(
			Character->GetMesh()->GetBoneLocation(HandBones[Side]), GripWorld(Side));
		const bool bGrabCharacter = GrabTarget->IsA<ADeliveryCharacter>();
		if (HandConstraints[Side]
			&& Separation > (bGrabCharacter ? DragBreakDistance : MaxHandSeparation))
		{
			UE_LOG(LogDelivery, Warning, TEXT("Grab joint released: grabber=%s target=%s gap=%.1f"),
				*GetNameSafe(Character), *GetNameSafe(GrabTarget.Get()), Separation);
			ReleaseHandOnServer(Side);
			if (!GrabTarget) return;
			continue;
		}
		if (!HandConstraints[Side]) TryAttach(Side);
		if (HandConstraints[Side])
		{
			if (bGrabCharacter)
			{
				// Chaos can leave a long soft error between two heavy ragdolls even with a locked joint.
				// Give the limp victim a capped horizontal pull toward the actual hand; the joint
				// still supplies the contact/rotation and a second grabber can pull the other way.
				USkeletalMeshComponent* TargetMesh = Cast<USkeletalMeshComponent>(GetTargetBody());
				if (TargetMesh)
				{
					const FVector Hand = Character->GetMesh()->GetBoneLocation(HandBones[Side]);
					const FVector Error = FVector::VectorPlaneProject(Hand - GripWorld(Side), FVector::UpVector);
					// Feed the grabber's actual travel velocity forward. A pure error spring
					// capped below walking speed leaves the victim permanently behind the hand.
					const FVector GrabberVelocity = FVector::VectorPlaneProject(
						Character->GetMesh()->GetPhysicsLinearVelocity(TEXT("Hips")), FVector::UpVector);
					const FVector DesiredVelocity = (GrabberVelocity + Error * 4.0f)
						.GetClampedToMaxSize(DragFollowSpeed);
					const FVector ActualVelocity = FVector::VectorPlaneProject(
						TargetMesh->GetPhysicsLinearVelocity(TEXT("Hips")), FVector::UpVector);
					const FVector Acceleration = ((DesiredVelocity - ActualVelocity) * 8.0f)
						.GetClampedToMaxSize(DragFollowAcceleration);
					// Apply the same acceleration to every simulated body, not just the pelvis:
					// a single pelvis force is dissipated by the prone body's many ground contacts.
					if (const UPhysicsAsset* Asset = TargetMesh->GetPhysicsAsset())
					{
						for (const USkeletalBodySetup* Setup : Asset->SkeletalBodySetups)
						{
							if (Setup && TargetMesh->IsSimulatingPhysics(Setup->BoneName))
							{
								TargetMesh->AddForce(Acceleration, Setup->BoneName, true);
							}
						}
					}
				}
			}
			if (bDragReeling[Side] && Separation <= DragAttachDistance)
			{
				HandConstraints[Side]->SetLinearPositionDrive(false, false, false);
				HandConstraints[Side]->SetLinearXLimit(LCM_Locked, 0.0f);
				HandConstraints[Side]->SetLinearYLimit(LCM_Locked, 0.0f);
				HandConstraints[Side]->SetLinearZLimit(LCM_Locked, 0.0f);
				HandConstraints[Side]->SetProjectionParams(0.0f, 0.0f, 15.0f, 180.0f);
				HandConstraints[Side]->SetProjectionEnabled(true);
				bDragReeling[Side] = false;
			}
			if (bDragReeling[Side]) continue;
			const bool bJumping = Character->GetActiveRagdoll()->IsJumping();
			if (bJumping && !bVerticalFree[Side])
			{
				HandConstraints[Side]->SetLinearZLimit(LCM_Free, 0.0f);
				bVerticalFree[Side] = true;
			}
			else if (!bJumping && bVerticalFree[Side] && Separation < AttachDistance)
			{
				HandConstraints[Side]->SetLinearZLimit(LCM_Locked, 0.0f);
				bVerticalFree[Side] = false;
			}
		}
	}
}

void UDeliveryGrabComponent::RequestReleaseHand(bool bLeft)
{
	const uint8 Bit = bLeft ? LeftBit : RightBit;
	const bool bDraggingCharacter = IsDraggingCharacter()
		|| (PredictedTarget.IsValid() && PredictedTarget->IsA<ADeliveryCharacter>());
	if (bDraggingCharacter)
	{
		PredictedHands = 0;
		LocallyReleasedHands = LeftBit | RightBit;
	}
	else
	{
		PredictedHands &= ~Bit;
		LocallyReleasedHands |= Bit;
	}
	if (!PredictedHands) PredictedTarget.Reset();
	if (GetOwner() && GetOwner()->HasAuthority()) ReleaseHandOnServer(bLeft ? 0 : 1);
	else ServerReleaseHand(bLeft);
}

void UDeliveryGrabComponent::ServerReleaseHand_Implementation(bool bLeft)
{
	ReleaseHandOnServer(bLeft ? 0 : 1);
}

void UDeliveryGrabComponent::ReleaseHandOnServer(int32 Side)
{
	if (!GetOwner() || !GetOwner()->HasAuthority()) return;
	// 拖人只允许一只手：双键中任意一键松开，就断开唯一的 joint。
	if (IsDraggingCharacter())
	{
		ForceRelease();
		return;
	}
	const uint8 Bit = Side == 0 ? LeftBit : RightBit;
	WantedHands &= ~Bit;
	AttachedHands &= ~Bit;
	if (HandConstraints[Side])
	{
		HandConstraints[Side]->BreakConstraint();
		HandConstraints[Side]->DestroyComponent();
		HandConstraints[Side] = nullptr;
	}
	bVerticalFree[Side] = false;
	bDragReeling[Side] = false;
	if (!WantedHands) ForceRelease();
	else GetOwner()->ForceNetUpdate();
}

void UDeliveryGrabComponent::ForceRelease()
{
	if (!GetOwner() || !GetOwner()->HasAuthority()) return;
	for (int32 Side = 0; Side < 2; ++Side)
	{
		if (HandConstraints[Side])
		{
			HandConstraints[Side]->BreakConstraint();
			HandConstraints[Side]->DestroyComponent();
			HandConstraints[Side] = nullptr;
		}
		bVerticalFree[Side] = false;
		bDragReeling[Side] = false;
	}
	if (IsValid(GrabTarget.Get()))
	{
		if (UDeliveryGrabbableComponent* Marker = GrabTarget->FindComponentByClass<UDeliveryGrabbableComponent>())
		{
			Marker->UnregisterGrabber(Cast<ADeliveryCharacter>(GetOwner()));
		}
	}
	GrabTarget = nullptr;
	WantedHands = 0;
	AttachedHands = 0;
	GetOwner()->ForceNetUpdate();
}

void UDeliveryGrabComponent::UpdateHighlight(AActor* Candidate)
{
	UPrimitiveComponent* NewBody = nullptr;
	if (UDeliveryGrabbableComponent* Marker = IsValid(Candidate)
		? Candidate->FindComponentByClass<UDeliveryGrabbableComponent>() : nullptr)
	{
		FName Bone;
		NewBody = Marker->GetGrabBody(Bone);
	}
	if (HighlightedBody.Get() == NewBody) return;
	ClearHighlight();
	if (NewBody)
	{
		HighlightedBody = NewBody;
		bOldCustomDepth = NewBody->bRenderCustomDepth;
		OldStencil = NewBody->CustomDepthStencilValue;
		NewBody->SetCustomDepthStencilValue(1);
		NewBody->SetRenderCustomDepth(true);
	}
}

void UDeliveryGrabComponent::ClearHighlight()
{
	if (UPrimitiveComponent* Body = HighlightedBody.Get())
	{
		Body->SetCustomDepthStencilValue(OldStencil);
		Body->SetRenderCustomDepth(bOldCustomDepth);
	}
	HighlightedBody.Reset();
}
