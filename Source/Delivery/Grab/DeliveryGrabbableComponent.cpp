#include "Grab/DeliveryGrabbableComponent.h"

#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Net/UnrealNetwork.h"
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
	ReleaseAllGrabbers();
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
	FName Bone;
	UPrimitiveComponent* Body = GetGrabBody(Bone);
	if (!Body || Body != Owner->GetRootComponent())
		return;
	FVector Target = FVector::ZeroVector;
	FVector TargetForward = FVector::ZeroVector;
	int32 Contributors = 0;
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
	if (!Contributors)
		return;
	// 多人搬运时取各自期望位置的平均值，避免物体只跟随其中一人
	Target /= Contributors;
	const FVector OldLocation = Body->GetComponentLocation();
	const FVector Next = FMath::VInterpTo(OldLocation, Target, DeltaTime, CarryFollowSpeed);
	FRotator NextRotation = Owner->GetActorRotation();
	if (!TargetForward.IsNearlyZero())
	{
		const FRotator Upright(0.0f, TargetForward.Rotation().Yaw, 0.0f);
		NextRotation = FMath::RInterpTo(NextRotation, Upright, DeltaTime, CarryFollowSpeed);
	}
	FHitResult Hit;
	Owner->SetActorLocationAndRotation(Next, NextRotation, true, &Hit, ETeleportType::None);
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
		if (Body && (Body->IsSimulatingPhysics() || (bKinematicCarry && Body == Owner->GetRootComponent())) &&
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
	FName Bone;
	UPrimitiveComponent* Body = GetGrabBody(Bone);
	if (!Body || (!Body->IsSimulatingPhysics(Bone) && !bKinematicCarry))
		return false;
	// Actor 的移动复制只保证根刚体；子组件单独模拟会在其他客户端漂离手部快照
	if (!GetOwner()->IsA<ADeliveryCharacter>() && Body != GetOwner()->GetRootComponent())
		return false;
	if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner()))
	{
		return Character->GetActiveRagdoll() &&
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
	if (!GetOwner() || !GetOwner()->HasAuthority())
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
	if (bKinematicCarry)
	{
		if (!bHasSavedPawnResponse)
		{
			SavedPawnResponse = Body->GetCollisionResponseToChannel(ECC_Pawn);
			SavedPhysicsBodyResponse = Body->GetCollisionResponseToChannel(ECC_PhysicsBody);
			bHasSavedPawnResponse = true;
		}
		Body->SetSimulatePhysics(false);
		Body->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		Body->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Ignore);
	}
	else
	{
		if (bHasSavedPawnResponse)
		{
			Body->SetCollisionResponseToChannel(ECC_Pawn, SavedPawnResponse);
			Body->SetCollisionResponseToChannel(ECC_PhysicsBody, SavedPhysicsBodyResponse);
			bHasSavedPawnResponse = false;
		}
		Body->SetSimulatePhysics(true);
		if (Owner->HasAuthority())
			Body->SetPhysicsLinearVelocity(LastCarryVelocity);
	}
}

// 释放全部抓取者

// 通知所有搬运者松手，并清空物体的抓取记录
void UDeliveryGrabbableComponent::ReleaseAllGrabbers()
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
		return;
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
