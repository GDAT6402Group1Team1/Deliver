#include "Grab/DeliveryGrabComponent.h"

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
#include "PhysicsEngine/PhysicsConstraintComponent.h"
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
	PredictedHands = LeftBit | RightBit;
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
		|| Character->GetActiveRagdoll()->GetControlMode() != EDeliveryRagdollControlMode::Active
		|| FVector::Dist(Character->GetMesh()->GetBoneLocation(TEXT("Hips")), HitPoint) > GrabDistance)
	{
		return;
	}
	// 客户端只提交意图；服务端重新核对朝向与遮挡，不能凭传来的 Actor 建约束。
	const FVector Eye = Character->GetPawnViewLocation();
	const FVector ToPoint = (HitPoint - Eye).GetSafeNormal();
	const FVector Facing = Character->GetController()
		? Character->GetController()->GetControlRotation().Vector() : Character->GetActorForwardVector();
	if (FVector::DotProduct(ToPoint, Facing) < 0.35f) return;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ServerGrabAim), false, Character);
	FHitResult Hit;
	if (!GetWorld()->LineTraceSingleByChannel(Hit, Eye, HitPoint + ToPoint * 10.0f,
		ECC_Visibility, Params) || Hit.GetActor() != Target
		|| FVector::Dist(Hit.ImpactPoint, HitPoint) > 55.0f
		|| !Grabbable->RegisterGrabber(Character))
	{
		return;
	}
	FName Bone;
	UPrimitiveComponent* Body = Grabbable->GetGrabBody(Bone);
	if (!Body)
	{
		Grabbable->UnregisterGrabber(Character);
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
	const FTransform BodyTransform = Body->GetSocketTransform(Bone);
	FVector LeftPoint = HitPoint;
	FVector RightPoint = HitPoint;
	if (!Target->IsA<ADeliveryCharacter>())
	{
		FindUndersideGripPoints(Body,
			Character->GetActiveRagdoll()->GetBodyForward().GetSafeNormal2D(), LeftPoint, RightPoint);
	}
	else
	{
		const FVector Side = Character->GetActorRightVector() * 18.0f;
		LeftPoint -= Side;
		RightPoint += Side;
	}
	GripLocalLeft = BodyTransform.InverseTransformPosition(LeftPoint);
	GripLocalRight = BodyTransform.InverseTransformPosition(RightPoint);
	WantedHands = LeftBit | RightBit;
	AttachedHands = Grabbable->bKinematicCarry ? WantedHands : 0;
	Character->ForceNetUpdate();
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
		return Body->GetSocketTransform(GrabBone).TransformPosition(
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
	if (AttachedHands && !FVector(CarryForward).IsNearlyZero())
	{
		OutDirection = FVector(CarryForward).GetSafeNormal2D();
		return true;
	}
	const FVector Point = IsValid(GrabTarget.Get()) ? GripWorld(0) : PredictedGripPoint;
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
	if (FVector::Dist(HandPoint, Point) > AttachDistance) return;
	// 触到表面时才确定这个手的局部抓点，之后物体旋转也不会在表面滑动。
	const FVector LocalPoint = Body->GetSocketTransform(GrabBone).InverseTransformPosition(Point);
	if (Side == 0) GripLocalLeft = LocalPoint;
	else GripLocalRight = LocalPoint;
	UPhysicsConstraintComponent* Constraint = NewObject<UPhysicsConstraintComponent>(Character);
	Constraint->RegisterComponent();
	Constraint->SetWorldLocation(Point);
	Constraint->SetDisableCollision(true);
	Constraint->SetLinearXLimit(LCM_Locked, 0.0f);
	Constraint->SetLinearYLimit(LCM_Locked, 0.0f);
	Constraint->SetLinearZLimit(LCM_Locked, 0.0f);
	// 两只手的位置已经约束了物品朝向，额外的角限位会反过来扭角色肩膀。
	const bool bGrabCharacter = GrabTarget->IsA<ADeliveryCharacter>();
	Constraint->SetAngularSwing1Limit(bGrabCharacter ? ACM_Limited : ACM_Free, 75.0f);
	Constraint->SetAngularSwing2Limit(bGrabCharacter ? ACM_Limited : ACM_Free, 75.0f);
	Constraint->SetAngularTwistLimit(ACM_Free, 0.0f);
	Constraint->SetLinearBreakable(true, 80000.0f);
	Constraint->SetConstrainedComponents(Character->GetMesh(), HandBones[Side], Body, GrabBone);
	// 默认两侧参考帧都取抓点，会永久保留“手还差 20–35 cm”的间隔。
	// 改为把手骨骼的真实位置锁到物体抓点，消除视觉上的隔空抓取。
	if (const FBodyInstance* HandBody = Character->GetMesh()->GetBodyInstance(HandBones[Side]))
	{
		Constraint->SetConstraintReferencePosition(EConstraintFrame::Frame1,
			HandBody->GetUnrealWorldTransform().InverseTransformPosition(HandPoint));
	}
	HandConstraints[Side] = Constraint;
	AttachedHands |= Bit;
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
			ReleaseHandOnServer(Side);
			if (!GrabTarget) return;
			continue;
		}
		const float Separation = FVector::Dist(
			Character->GetMesh()->GetBoneLocation(HandBones[Side]), GripWorld(Side));
		if (HandConstraints[Side] && Separation > MaxHandSeparation)
		{
			ReleaseHandOnServer(Side);
			if (!GrabTarget) return;
			continue;
		}
		if (!HandConstraints[Side]) TryAttach(Side);
		if (HandConstraints[Side])
		{
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
	PredictedHands &= ~Bit;
	LocallyReleasedHands |= Bit;
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
