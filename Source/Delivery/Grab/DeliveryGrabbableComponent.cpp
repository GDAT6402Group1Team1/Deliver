#include "Grab/DeliveryGrabbableComponent.h"

#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "GameFramework/Pawn.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Vehicle/DeliveryTrunkPassengerComponent.h"
#include "Net/UnrealNetwork.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"

UDeliveryGrabbableComponent::UDeliveryGrabbableComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	SetIsReplicatedByDefault(true);
}

// 初始化与更新

// 为普通可搬动物体启用位置同步
void UDeliveryGrabbableComponent::BeginPlay()
{
	Super::BeginPlay();
	if (GetOwner() && GetOwner()->HasAuthority() && !GetOwner()->IsA<ADeliveryCharacter>())
	{
		GetOwner()->SetReplicates(true);
		GetOwner()->SetReplicateMovement(true);
	}
}

// 物体结束运行前解除所有抓取关系
void UDeliveryGrabbableComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bEndingPlay = true;
	ReleaseAllGrabbers();
	// 组件先于 Actor 退出时，客户端也要归还已经暂停的根刚体
	if (GetOwner() && !GetOwner()->HasAuthority() && (bNpcCarry || (bKinematicCarry && GrabberCount == 0)))
	{
		bNpcCarry = false;
		bKinematicCarry = false;
		RestoreCarryBody(Cast<UPrimitiveComponent>(GetOwner()->GetRootComponent()));
	}
	Super::EndPlay(EndPlayReason);
}

// 根据搬运者的位置更新普通物体，并记录松手时的速度
void UDeliveryGrabbableComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                                FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority() || !bKinematicCarry)
		return;
	const bool bMovingNpcObject = bNpcCarry;
	const FGuid CarryId = ProviderCarryId;
	const TWeakObjectPtr<UActorComponent> Provider = CarryProvider;
	const TWeakObjectPtr<UDeliveryGrabbableComponent> WeakThis(this);
	FName Bone;
	UPrimitiveComponent* Body = bMovingNpcObject ? ProviderCarryBody.Get() : GetGrabBody(Bone);
	if (!Body || Body != Owner->GetRootComponent())
	{
		if (bMovingNpcObject)
			ReleaseCarryProvider(EDeliveryObjectCarryReleaseReason::ObjectUnavailable);
		return;
	}
	FVector Target = FVector::ZeroVector;
	FVector TargetForward = FVector::ZeroVector;
	int32 Contributors = 0;
	if (bMovingNpcObject)
	{
		if (!Provider.IsValid() || !Provider->IsRegistered() || !Provider->HasBegunPlay())
		{
			ReleaseCarryProvider(EDeliveryObjectCarryReleaseReason::ProviderUnavailable);
			return;
		}
		if (!ReadCarryProviderPose(Provider.Get(), CarryId, Body, Target, TargetForward))
		{
			if (WeakThis.IsValid() && bNpcCarry && ProviderCarryId == CarryId && CarryProvider.HasSameIndexAndSerialNumber(Provider))
				ReleaseCarryProvider(EDeliveryObjectCarryReleaseReason::InvalidPose);
			return;
		}
		// Provider 查询和扫掠回调都可能结束当前搬运，旧 Tick 不能续写下一次搬运
		if (!IsCarriedByProvider(Provider.Get(), CarryId) || ProviderCarryBody.Get() != Body)
			return;
		if (!FMath::IsFinite(DeltaTime) || DeltaTime <= 0.f)
			return;
		Contributors = 1;
	}
	else
	{
		for (const TWeakObjectPtr<ADeliveryCharacter>& Entry : Grabbers)
		{
			const ADeliveryCharacter* Character = Entry.Get();
			const UDeliveryGrabComponent* Grab = Character ? Character->GetGrabComponent() : nullptr;
			FVector CarryCenter;
			FVector CarryForward;
			if (Grab && Grab->GetCarryPoseFor(Owner, CarryCenter, CarryForward))
			{
				Target += CarryCenter;
				TargetForward += CarryForward;
				++Contributors;
			}
		}
	}
	if (!Contributors)
		return;
	// 多人搬运时取各自期望位置的平均值，避免物体只跟随其中一人
	Target /= Contributors;
	const FVector OldLocation = Body->GetComponentLocation();
	// NPC 的坏姿势和长帧不能把物体拉出近距范围，玩家仍沿原有插值参数
	const float MoveDelta = bMovingNpcObject ? FMath::Min(DeltaTime, .1f) : DeltaTime;
	const float FollowSpeed = bMovingNpcObject
	    ? (FMath::IsFinite(CarryFollowSpeed) ? FMath::Clamp(CarryFollowSpeed, .1f, 100.f) : 20.f)
	    : CarryFollowSpeed;
	FVector Next = FMath::VInterpTo(OldLocation, Target, MoveDelta, FollowSpeed);
	if (bMovingNpcObject)
	{
		const float MaxSpeed = FMath::IsFinite(NpcCarryMaxSpeed) ? FMath::Clamp(NpcCarryMaxSpeed, 10.f, 600.f) : 300.f;
		Next = OldLocation + (Next - OldLocation).GetClampedToMaxSize(MaxSpeed * MoveDelta);
	}
	FRotator NextRotation = Owner->GetActorRotation();
	if (!TargetForward.IsNearlyZero())
	{
		const FRotator Upright(0.0f, TargetForward.Rotation().Yaw, 0.0f);
		if (bMovingNpcObject)
		{
			const float TurnSpeed = FMath::IsFinite(NpcCarryMaxTurnSpeed) ? FMath::Clamp(NpcCarryMaxTurnSpeed, 10.f, 360.f) : 90.f;
			NextRotation = FMath::RInterpConstantTo(NextRotation, Upright, MoveDelta, TurnSpeed);
		}
		else
			NextRotation = FMath::RInterpTo(NextRotation, Upright, MoveDelta, FollowSpeed);
	}
	if (bMovingNpcObject && !IsNpcCarryMoveClear(Body, Next, NextRotation.Quaternion()))
	{
		// 地面上的物体可以先按原朝向抬起，腾出空间后再转向
		NextRotation = Owner->GetActorRotation();
		if (!IsNpcCarryMoveClear(Body, Next, NextRotation.Quaternion()))
		{
			LastCarryVelocity = FVector::ZeroVector;
			PreviousCarryLocation = Body->GetComponentLocation();
			return;
		}
	}
	const TWeakObjectPtr<AActor> WeakOwner(Owner);
	const TWeakObjectPtr<UPrimitiveComponent> WeakBody(Body);
	FHitResult Hit;
	Owner->SetActorLocationAndRotation(Next, NextRotation, true, &Hit, ETeleportType::None);
	if (!WeakThis.IsValid() || !WeakOwner.IsValid() || !WeakBody.IsValid())
		return;
	if (bMovingNpcObject && (bEndingPlay || !IsCarriedByProvider(Provider.Get(), CarryId) ||
	    ProviderCarryBody.Get() != Body || Owner->GetRootComponent() != Body))
		return;
	// 用碰撞处理后的实际位移计算速度，松手时物体沿这个速度继续运动
	LastCarryVelocity =
	    (Body->GetComponentLocation() - PreviousCarryLocation) / FMath::Max(DeltaTime, KINDA_SMALL_NUMBER);
	LastCarryVelocity = LastCarryVelocity.GetClampedToMaxSize(500.0f);
	PreviousCarryLocation = Body->GetComponentLocation();
}

// 同步搬运人数和搬运模式
void UDeliveryGrabbableComponent::GetLifetimeReplicatedProps(
    TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UDeliveryGrabbableComponent, GrabberCount);
	DOREPLIFETIME(UDeliveryGrabbableComponent, bKinematicCarry);
	DOREPLIFETIME(UDeliveryGrabbableComponent, bNpcCarry);
}

// 选择被抓取的刚体，角色使用躯干，普通物体使用配置的组件
UPrimitiveComponent* UDeliveryGrabbableComponent::GetGrabBody(FName& OutBone) const
{
	OutBone = NAME_None;
	AActor* Owner = GetOwner();
	if (!Owner)
		return nullptr;
	if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(Owner))
	{
		OutBone = TEXT("Spine");
		return Character->GetMesh();
	}
	TArray<UPrimitiveComponent*> Bodies;
	Owner->GetComponents(Bodies);
	for (UPrimitiveComponent* Body : Bodies)
	{
		if (Body && (Body->IsSimulatingPhysics() || ((bKinematicCarry || bNpcCarry) && Body == Owner->GetRootComponent())) &&
		    (BodyComponentName.IsNone() || Body->GetFName() == BodyComponentName))
		{
			return Body;
		}
	}
	return nullptr;
}

// 抓取条件与抓取者登记

// 检查物体是否可抓，角色只有倒地时允许被拖动
bool UDeliveryGrabbableComponent::CanGrab(const ADeliveryCharacter* Requester) const
{
	if (!Requester || Requester == GetOwner())
		return false;
	// 两个复制标志先后到达时也不能把 NPC 的物体开放给玩家，人数仍只统计玩家
	if (bNpcCarry || (bKinematicCarry && GrabberCount == 0) || bChangingCarryProvider || bReleasingAllGrabbers || bEndingPlay)
		return false;
	FName Bone;
	UPrimitiveComponent* Body = GetGrabBody(Bone);
	if (!Body || (!Body->IsSimulatingPhysics(Bone) && !bKinematicCarry))
		return false;
	// Actor 的移动复制只保证根刚体；子组件单独模拟会在其他客户端漂离手部快照
	if (!GetOwner()->IsA<ADeliveryCharacter>() && Body != GetOwner()->GetRootComponent())
		return false;
	if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner()))
	{
		return (!Character->TrunkPassenger || !Character->TrunkPassenger->IsPassenger()) && Character->GetActiveRagdoll() &&
		       Character->GetActiveRagdoll()->GetControlMode() == EDeliveryRagdollControlMode::Limp;
	}
	return true;
}

// 登记搬运者，普通物体首次被抓住时切换为跟随搬运
bool UDeliveryGrabbableComponent::RegisterGrabber(ADeliveryCharacter* Requester)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !CanGrab(Requester))
		return false;
	Grabbers.RemoveAll(
	    [](const TWeakObjectPtr<ADeliveryCharacter>& Entry)
	    {
		    return !Entry.IsValid();
	    });
	if (!Grabbers.Contains(Requester))
	{
		if (Grabbers.Num() >= 2)
			return false;
		Grabbers.Add(Requester);
	}
	GrabberCount = Grabbers.Num();
	if (!GetOwner()->IsA<ADeliveryCharacter>() && !bKinematicCarry)
	{
		bKinematicCarry = true;
		PreviousCarryLocation = GetOwner()->GetActorLocation();
		LastCarryVelocity = FVector::ZeroVector;
		ApplyCarryMode();
	}
	GetOwner()->ForceNetUpdate();
	return true;
}

// 移除搬运者，最后一人松手后恢复物理模拟
void UDeliveryGrabbableComponent::UnregisterGrabber(ADeliveryCharacter* Requester)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || bNpcCarry)
		return;
	Grabbers.RemoveAll(
	    [Requester](const TWeakObjectPtr<ADeliveryCharacter>& Entry)
	    {
		    return !Entry.IsValid() || Entry.Get() == Requester;
	    });
	GrabberCount = Grabbers.Num();
	if (bKinematicCarry && GrabberCount == 0)
	{
		bKinematicCarry = false;
		ApplyCarryMode();
	}
	GetOwner()->ForceNetUpdate();
}

// 携带状态同步

// 客户端收到搬运模式变化后更新物理和碰撞
void UDeliveryGrabbableComponent::OnRep_KinematicCarry()
{
	ApplyCarryMode();
}

// 搬运时暂停自由物理，松手时恢复碰撞并保留移动速度
void UDeliveryGrabbableComponent::ApplyCarryMode()
{
	AActor* Owner = GetOwner();
	if (!Owner || Owner->IsA<ADeliveryCharacter>())
		return;
	UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(Owner->GetRootComponent());
	if (!Body)
		return;
	if (bKinematicCarry || bNpcCarry)
	{
		TGuardValue<bool> ChangingGuard(bChangingCarryProvider, true);
		const TWeakObjectPtr<UDeliveryGrabbableComponent> WeakThis(this);
		const TWeakObjectPtr<UPrimitiveComponent> WeakBody(Body);
		if (!bHasSavedPawnResponse)
		{
			SavedPawnResponse = Body->GetCollisionResponseToChannel(ECC_Pawn);
			SavedPhysicsBodyResponse = Body->GetCollisionResponseToChannel(ECC_PhysicsBody);
			bHasSavedPawnResponse = true;
		}
		Body->SetSimulatePhysics(false);
		if (!WeakThis.IsValid() || !WeakBody.IsValid() || !(bKinematicCarry || bNpcCarry))
			return;
		Body->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		if (!WeakThis.IsValid() || !WeakBody.IsValid() || !(bKinematicCarry || bNpcCarry))
			return;
		Body->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Ignore);
	}
	else
	{
		RestoreCarryBody(Body);
	}
}

void UDeliveryGrabbableComponent::RestoreCarryBody(UPrimitiveComponent* Body)
{
	TGuardValue<bool> ChangingGuard(bChangingCarryProvider, true);
	const bool bRestoreResponses = bHasSavedPawnResponse;
	const ECollisionResponse PawnResponse = SavedPawnResponse;
	const ECollisionResponse PhysicsResponse = SavedPhysicsBodyResponse;
	bHasSavedPawnResponse = false;
	if (!IsValid(Body))
		return;
	const TWeakObjectPtr<UDeliveryGrabbableComponent> WeakThis(this);
	const TWeakObjectPtr<UPrimitiveComponent> WeakBody(Body);
	if (bRestoreResponses)
	{
		Body->SetCollisionResponseToChannel(ECC_Pawn, PawnResponse);
		if (!WeakThis.IsValid() || !WeakBody.IsValid())
			return;
		Body->SetCollisionResponseToChannel(ECC_PhysicsBody, PhysicsResponse);
		if (!WeakThis.IsValid() || !WeakBody.IsValid())
			return;
	}
	AActor* Owner = GetOwner();
	if (IsValid(Owner) && !Owner->IsActorBeingDestroyed())
	{
		const bool bAuthority = Owner->HasAuthority();
		Body->SetSimulatePhysics(true);
		if (WeakThis.IsValid() && WeakBody.IsValid() && bAuthority)
			Body->SetPhysicsLinearVelocity(LastCarryVelocity);
	}
}

// NPC 只提供姿势，登记和每帧都用真实角色位置、根刚体与遮挡复核
bool UDeliveryGrabbableComponent::ReadCarryProviderPose(UActorComponent* Provider, const FGuid& CarryId,
    UPrimitiveComponent* Body, FVector& OutTarget, FVector& OutForward) const
{
	AActor* Owner = GetOwner();
	AActor* ProviderActor = IsValid(Provider) ? Provider->GetOwner() : nullptr;
	IDeliveryObjectCarryProvider* Interface = IsValid(Provider) ? Cast<IDeliveryObjectCarryProvider>(Provider) : nullptr;
	UWorld* World = GetWorld();
	if (!IsValid(Owner) || Owner->IsActorBeingDestroyed() || !IsValid(ProviderActor) ||
	    ProviderActor->IsActorBeingDestroyed() || ProviderActor == Owner || !World ||
	    Provider->GetWorld() != World || ProviderActor->GetWorld() != World ||
	    !Owner->HasAuthority() || !ProviderActor->HasAuthority() || !Provider->IsRegistered() ||
	    !Provider->HasBegunPlay() || !Interface || !CarryId.IsValid() || !IsValid(Body) ||
	    !Body->IsRegistered() || !Body->IsPhysicsStateCreated() || !Body->IsQueryCollisionEnabled() ||
	    Body != Owner->GetRootComponent() || Body->IsA<USkeletalMeshComponent>() ||
	    Body->GetAttachParent() || Owner->GetOwner() || Owner->IsA<APawn>())
		return false;

	const TWeakObjectPtr<const UDeliveryGrabbableComponent> WeakThis(this);
	const TWeakObjectPtr<AActor> WeakOwner(Owner);
	const TWeakObjectPtr<UActorComponent> WeakProvider(Provider);
	const TWeakObjectPtr<UPrimitiveComponent> WeakBody(Body);
	if (!Interface->GetObjectCarryPose(Owner, CarryId, OutTarget, OutForward) ||
	    !WeakThis.IsValid() || !WeakOwner.IsValid() || !WeakProvider.IsValid() || !WeakBody.IsValid())
		return false;
	if (bEndingPlay || !Provider->IsRegistered() || !Provider->HasBegunPlay() || Provider->GetOwner() != ProviderActor ||
	    !IsValid(ProviderActor) || ProviderActor->IsActorBeingDestroyed() ||
	    !Body->IsRegistered() || !Body->IsPhysicsStateCreated() || !Body->IsQueryCollisionEnabled() ||
	    Body != Owner->GetRootComponent() || Body->GetAttachParent() || Owner->GetOwner())
		return false;

	const FVector ProviderLocation = ProviderActor->GetActorLocation();
	const FVector BodyLocation = Body->GetComponentLocation();
	const FVector BodyCenter = Body->Bounds.Origin;
	if (ProviderLocation.ContainsNaN() || BodyLocation.ContainsNaN() || BodyCenter.ContainsNaN() ||
	    OutTarget.ContainsNaN() || OutForward.ContainsNaN())
		return false;
	OutForward.Z = 0.f;
	if (!OutForward.Normalize())
		return false;
	const float MaxDistance = FMath::IsFinite(NpcCarryMaxDistance) ? FMath::Clamp(NpcCarryMaxDistance, 20.f, 300.f) : 200.f;
	if (FVector::DistSquared(ProviderLocation, BodyLocation) > FMath::Square(MaxDistance) ||
	    FVector::DistSquared(ProviderLocation, OutTarget) > FMath::Square(MaxDistance))
		return false;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(DeliveryNpcCarryReach), false, ProviderActor);
	Query.AddIgnoredActor(Owner);
	return !World->LineTraceTestByChannel(ProviderLocation, BodyCenter, ECC_Visibility, Query) &&
	       !World->LineTraceTestByChannel(ProviderLocation, OutTarget, ECC_Visibility, Query);
}

// 包围整个平移和转向范围，补上 Actor 平移扫掠没有覆盖的旋转净空
bool UDeliveryGrabbableComponent::IsNpcCarryMoveClear(UPrimitiveComponent* Body, const FVector& Location,
    const FQuat& Rotation) const
{
	UWorld* World = GetWorld();
	if (!World || !IsValid(Body) || Location.ContainsNaN() || Rotation.ContainsNaN() || !Rotation.IsNormalized())
		return false;
	FBodyInstance* Instance = Body->GetBodyInstance();
	if (!Instance || !Instance->IsValidBodyInstance())
		return false;
	FBox LocalBox = Body->CalcBounds(
	    FTransform(FQuat::Identity, FVector::ZeroVector, Body->GetComponentScale())).GetBox();
	// Chaos 的本地刚体包围盒已经包含碰撞几何的缩放，和可见网格一起覆盖
	const FBox PhysicsBox = Instance->GetBodyBoundsLocal();
	if (!PhysicsBox.IsValid || PhysicsBox.Min.ContainsNaN() || PhysicsBox.Max.ContainsNaN())
		return false;
	LocalBox += PhysicsBox;
	const FVector Extent = LocalBox.GetExtent();
	const FVector LocalCenter = LocalBox.GetCenter();
	const FVector OldLocation = Body->GetComponentLocation();
	const FQuat OldRotation = Body->GetComponentQuat();
	if (Extent.ContainsNaN() || LocalCenter.ContainsNaN() || Extent.GetMin() <= 0.f ||
	    OldLocation.ContainsNaN() || OldRotation.ContainsNaN() || !OldRotation.IsNormalized())
		return false;
	// 最远角点的转弯距离扩大包围盒，检查中间朝向，不能只检查最终角度
	const double Angle = OldRotation.AngularDistance(Rotation);
	const double TurnMargin = 2.0 * (Extent.Size() + LocalCenter.Size()) * FMath::Sin(Angle * .5);
	const FVector MotionHalf = Rotation.UnrotateVector(Location - OldLocation).GetAbs() * .5;
	const FVector QueryExtent = Extent + MotionHalf + FVector(TurnMargin);
	const FVector QueryCenter = (OldLocation + Location) * .5 + Rotation.RotateVector(LocalCenter);
	if (QueryExtent.ContainsNaN() || QueryCenter.ContainsNaN())
		return false;
	const FCollisionShape Shape = FCollisionShape::MakeBox(QueryExtent);
	FCollisionQueryParams Query(SCENE_QUERY_STAT(DeliveryNpcCarryClearance), false, GetOwner());
	Query.AddIgnoredActor(CarryProviderActor.Get());
	const FCollisionObjectQueryParams Objects(FCollisionObjectQueryParams::AllObjects);
	TArray<FOverlapResult> Overlaps;
	World->OverlapMultiByObjectType(Overlaps, QueryCenter, Rotation, Objects, Shape, Query);
	TArray<FOverlapResult> AboveSupport;
	bool bCheckedAboveSupport = false;
	for (const FOverlapResult& Overlap : Overlaps)
	{
		UPrimitiveComponent* Other = Overlap.GetComponent();
		const AActor* OtherActor = IsValid(Other) ? Other->GetOwner() : nullptr;
		if (!IsValid(Other) || !Other->IsQueryCollisionEnabled())
			continue;
		const bool bOtherPawn = OtherActor && OtherActor->IsA<APawn>();
		// 搬运时的 Pawn/PhysicsBody ignore 不参与这次复核，其他玩家和家具仍会挡住物体
		if (!bOtherPawn && Other->GetCollisionResponseToChannel(Body->GetCollisionObjectType()) != ECR_Block)
			continue;
		const ECollisionChannel ObjectType = Other->GetCollisionObjectType();
		FMTDResult Contact;
		if (bOtherPawn || Other->IsSimulatingPhysics() ||
		    (ObjectType != ECC_WorldStatic && ObjectType != ECC_WorldDynamic) ||
		    !Other->ComputePenetration(Contact, Shape, QueryCenter, Rotation) ||
		    Contact.Direction.ContainsNaN() || !FMath::IsFinite(Contact.Distance) ||
		    Contact.Direction.Z < .9 || Contact.Distance < 0.f || Contact.Distance > 1.f)
			return false;
		// 只容许 1 cm 内的底部支撑接触；同一个房屋组件还碰到侧墙时不能被忽略
		if (!bCheckedAboveSupport)
		{
			World->OverlapMultiByObjectType(AboveSupport, QueryCenter + FVector(0, 0, 1.1), Rotation, Objects, Shape, Query);
			bCheckedAboveSupport = true;
		}
		if (AboveSupport.ContainsByPredicate([Other](const FOverlapResult& Entry) { return Entry.GetComponent() == Other; }))
			return false;
	}
	return true;
}

bool UDeliveryGrabbableComponent::RegisterCarryProvider(UActorComponent* Provider, const FGuid& CarryId)
{
	AActor* Owner = GetOwner();
	if (!IsValid(Owner) || !Owner->HasAuthority() || !HasBegunPlay() || bEndingPlay || bChangingCarryProvider || bReleasingAllGrabbers ||
	    !IsValid(Provider) || !CarryId.IsValid())
		return false;
	if (IsCarriedByProvider(Provider, CarryId))
	{
		TGuardValue<bool> ChangingGuard(bChangingCarryProvider, true);
		const TWeakObjectPtr<UActorComponent> WeakProvider(Provider);
		const TWeakObjectPtr<UDeliveryGrabbableComponent> WeakThis(this);
		FVector Target, Forward;
		if (!ReadCarryProviderPose(Provider, CarryId, ProviderCarryBody.Get(), Target, Forward))
		{
			if (WeakThis.IsValid() && bNpcCarry && ProviderCarryId == CarryId && CarryProvider.HasSameIndexAndSerialNumber(WeakProvider))
				ReleaseCarryProvider(EDeliveryObjectCarryReleaseReason::InvalidPose);
			return false;
		}
		return IsCarriedByProvider(Provider, CarryId);
	}
	if (HasAnyCarrier() || Grabbers.ContainsByPredicate([](const TWeakObjectPtr<ADeliveryCharacter>& Entry)
	    { return Entry.IsValid(); }))
		return false;
	FName Bone;
	UPrimitiveComponent* Body = GetGrabBody(Bone);
	if (!IsValid(Body) || Body != Owner->GetRootComponent() || !Body->IsSimulatingPhysics(Bone))
		return false;

	TGuardValue<bool> ChangingGuard(bChangingCarryProvider, true);
	FVector Target, Forward;
	if (!ReadCarryProviderPose(Provider, CarryId, Body, Target, Forward))
		return false;
	Grabbers.Reset();
	GrabberCount = 0;
	CarryProvider = Provider;
	CarryProviderActor = Provider->GetOwner();
	ProviderCarryBody = Body;
	ProviderCarryId = CarryId;
	CarryProviderActor->OnEndPlay.AddUniqueDynamic(this, &UDeliveryGrabbableComponent::OnCarryProviderEndPlay);
	bNpcCarry = true;
	bKinematicCarry = true;
	PreviousCarryLocation = Body->GetComponentLocation();
	LastCarryVelocity = FVector::ZeroVector;
	const TWeakObjectPtr<UDeliveryGrabbableComponent> WeakThis(this);
	const TWeakObjectPtr<UActorComponent> WeakProvider(Provider);
	ApplyCarryMode();
	if (!WeakThis.IsValid())
		return false;
	if (bEndingPlay || !IsCarriedByProvider(Provider, CarryId))
	{
		if (bNpcCarry && ProviderCarryId == CarryId && CarryProvider.HasSameIndexAndSerialNumber(WeakProvider))
			ReleaseCarryProvider(EDeliveryObjectCarryReleaseReason::ProviderUnavailable);
		return false;
	}
	Owner->ForceNetUpdate();
	return true;
}

bool UDeliveryGrabbableComponent::IsCarriedByProvider(const UActorComponent* Provider, const FGuid& CarryId) const
{
	return bNpcCarry && bKinematicCarry && IsValid(Provider) && CarryId.IsValid() &&
	       CarryProvider.Get() == Provider && ProviderCarryId == CarryId;
}

bool UDeliveryGrabbableComponent::UnregisterCarryProvider(UActorComponent* Provider, const FGuid& CarryId,
    EDeliveryObjectCarryReleaseReason Reason)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !IsCarriedByProvider(Provider, CarryId))
		return false;
	ReleaseCarryProvider(Reason);
	return true;
}

void UDeliveryGrabbableComponent::ReleaseCarryProvider(EDeliveryObjectCarryReleaseReason Reason)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !bNpcCarry)
		return;
	TGuardValue<bool> ChangingGuard(bChangingCarryProvider, true);
	const TWeakObjectPtr<UActorComponent> OldProvider = CarryProvider;
	const TWeakObjectPtr<AActor> OldProviderActor = CarryProviderActor;
	const TWeakObjectPtr<UPrimitiveComponent> OldBody = ProviderCarryBody;
	const TWeakObjectPtr<AActor> OldOwner(GetOwner());
	const FGuid OldId = ProviderCarryId;
	// 先解除本次引用，恢复实物后才通知；回调中的旧取消不能碰到下一次搬运
	CarryProvider.Reset();
	CarryProviderActor.Reset();
	ProviderCarryBody.Reset();
	ProviderCarryId.Invalidate();
	bNpcCarry = false;
	bKinematicCarry = false;
	if (OldProviderActor.IsValid())
		OldProviderActor->OnEndPlay.RemoveDynamic(this, &UDeliveryGrabbableComponent::OnCarryProviderEndPlay);
	const TWeakObjectPtr<UDeliveryGrabbableComponent> WeakThis(this);
	// 外部系统已接走或附着实物时，不用旧搬运记录覆盖新持有者的物理模式
	if (OldBody.IsValid() && (OldBody->GetAttachParent() || (OldOwner.IsValid() && OldOwner->GetOwner())))
		bHasSavedPawnResponse = false;
	else
		RestoreCarryBody(OldBody.Get());
	if (!WeakThis.IsValid())
		return;
	if (OldOwner.IsValid())
		OldOwner->ForceNetUpdate();
	if (IDeliveryObjectCarryProvider* Interface = Cast<IDeliveryObjectCarryProvider>(OldProvider.Get()))
		Interface->OnObjectCarryReleased(OldOwner.Get(), OldId, Reason);
}

void UDeliveryGrabbableComponent::OnCarryProviderEndPlay(AActor* Actor, EEndPlayReason::Type Reason)
{
	if (Actor && CarryProviderActor.HasSameIndexAndSerialNumber(TWeakObjectPtr<AActor>(Actor)))
		ReleaseCarryProvider(EDeliveryObjectCarryReleaseReason::ProviderUnavailable);
}

// 释放全部抓取者

// 通知所有搬运者松手，并清空物体的抓取记录
void UDeliveryGrabbableComponent::ReleaseAllGrabbers()
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || bReleasingAllGrabbers)
		return;
	TGuardValue<bool> ReleasingGuard(bReleasingAllGrabbers, true);
	ReleaseCarryProvider(bEndingPlay ? EDeliveryObjectCarryReleaseReason::ObjectEndPlay : EDeliveryObjectCarryReleaseReason::ReleasedAll);
	// 松手会修改 Grabbers，先复制名单再逐个释放
	const TArray<TWeakObjectPtr<ADeliveryCharacter>> Current = Grabbers;
	for (const TWeakObjectPtr<ADeliveryCharacter>& Entry : Current)
	{
		if (ADeliveryCharacter* Character = Entry.Get())
		{
			if (UDeliveryGrabComponent* Grab = Character->GetGrabComponent())
				Grab->ForceRelease();
		}
	}
	Grabbers.Reset();
	GrabberCount = 0;
	if (bKinematicCarry)
	{
		bKinematicCarry = false;
		ApplyCarryMode();
	}
}
