// Copyright Epic Games, Inc. All Rights Reserved.
//
// 摩托车的整车缩放和后备箱装卸货
//
// 和 DeliveryMotorbikeRider.cpp 一样是 ADeliveryMotorbike 的一部分，分出来只是因为
// DeliveryMotorbike.cpp 已经很长，而这两件事（尺寸、后备箱）各自是独立的一小块。

#include "Vehicle/Motorbike/DeliveryMotorbike.h"
#include "Vehicle/Summon/DeliveryVehicleSummonComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Delivery.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/PlayerState.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/Items/DeliveryPackageItem.h"

// 把视觉缩放写进缩放层，并把整体往下挪，保持车底仍贴着地面
void ADeliveryMotorbike::ApplyVisualScale()
{
	const float Scale = FMath::Max(VisualScale, 0.01f);

	if (CollisionBox)
	{
		CollisionBox->SetBoxExtent(BaseCollisionExtent * Scale);
	}
	if (!MeshScale)
	{
		return;
	}

	// 兼容已有蓝图实例保存的层级：缩放层是后加的，老实例上 MeshAlign 还挂在 MeshRoot 下
	if (MeshAlign && MeshAlign->GetAttachParent() != MeshScale)
	{
		MeshAlign->AttachToComponent(MeshScale, FAttachmentTransformRules::KeepRelativeTransform);
	}

	/**
	 * 量出车底在 MeshRoot 空间里的高度，再按缩放补偿
	 *
	 * 缩放绕 MeshScale 原点做，车底原来在 z = Bottom（负数），缩完变成 Bottom * Scale，
	 * 也就是往上抬了 Bottom * (Scale - 1)。把缩放层整体下移同样的量就抵掉了，
	 * 不用去动 HoverHeight——那是"车辆原点离地多高"，和模型多大无关。
	 * MeshAlign 只有 Yaw（车头朝向校正），所以它的旋转不改变 Z，直接加它的相对高度就行。
	 */
	float Bottom = 0.0f;
	bool bHasBottom = false;
	for (const TObjectPtr<UStaticMesh>& Soft : BodyMeshes)
	{
		if (const UStaticMesh* Mesh = Soft.Get())
		{
			const float MeshBottom = Mesh->GetBoundingBox().Min.Z;
			Bottom = bHasBottom ? FMath::Min(Bottom, MeshBottom) : MeshBottom;
			bHasBottom = true;
		}
	}
	float Drop = 0.0f;
	if (bHasBottom)
	{
		const float AlignZ = MeshAlign ? MeshAlign->GetRelativeLocation().Z : 0.0f;
		Drop = (AlignZ + Bottom) * (1.0f - Scale);
	}
	MeshScale->SetRelativeScale3D(FVector(Scale));
	MeshScale->SetRelativeLocation(FVector(0.0f, 0.0f, Drop));
}

// 现在能不能上车：有人骑、晕倒、手上拿着快递、正在装卸都不行
bool ADeliveryMotorbike::CanDriveNow(const ADeliveryCharacter* Character) const
{
	// 只有处于正常碰撞状态的角色能上车，客户端退出时可确定地恢复步行碰撞
	if (!AllowsPlayerUse() || !IsValid(Character) || !Character->GetActorEnableCollision() || Driver || IsTrunkBusy())
	{
		return false;
	}
	if (Character->IsIncapacitated())
	{
		return false;
	}

	/**
	 * 手上还拿着快递时不给上车——先走到车尾按左键装进后备箱
	 *
	 * 查的是背包手持槽，不是 GetCarriedTaskPackage()：后者在骑车时会去查车上的货，
	 * 而这里问的恰恰是"这个人手上有没有东西"。
	 */
	if (const UDeliveryInventoryComponent* Inventory = Character->GetInventoryComponent())
	{
		if (Cast<ADeliveryPackageItem>(Inventory->GetHeldItem()) ||
		    (HasCargo() && Inventory->GetStoredTaskPackage()))
		{
			return false;
		}
	}
	return true;
}

bool ADeliveryMotorbike::CanLoadTrunk(const ADeliveryCharacter* Character) const
{
	if (!AllowsPlayerUse() || !IsValid(Character) || Driver || IsTrunkBusy() || HasCargo() || HasTrunkPassenger() || Character->IsIncapacitated())
	{
		return false;
	}
	const UDeliveryInventoryComponent* Inventory = Character->GetInventoryComponent();

	return Inventory && Cast<ADeliveryPackageItem>(Inventory->GetHeldItem()) != nullptr;
}

bool ADeliveryMotorbike::CanUnloadTrunk(const ADeliveryCharacter* Character) const
{
	if (!AllowsPlayerUse() || !IsValid(Character) || Driver || IsTrunkBusy() || !HasCargo() || Character->IsIncapacitated())
	{
		return false;
	}
	// 手上已经有东西就不能再接一件
	const UDeliveryInventoryComponent* Inventory = Character->GetInventoryComponent();

	return Inventory && !Inventory->HasHeldItem() && !Inventory->GetStoredTaskPackage();
}

// 左键交互的条件：装和卸任一能做就算可用
bool ADeliveryMotorbike::CanUseTrunk(APawn* Interactor, bool /*bCheckTaskAvailability*/)
{
	const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(Interactor);

	return CanLoadTrunk(Character) || CanUnloadTrunk(Character);
}

// 左键交互的执行：按当前状态决定这一下是装还是卸
bool ADeliveryMotorbike::UseTrunk(APawn* Interactor)
{
	ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(Interactor);
	if (CanLoadTrunk(Character))
	{
		return BeginTrunkLoad(Character);
	}

	return BeginTrunkUnload(Character);
}

// 按当前状态刷新"左键装货/取货"的文字和开关
void ADeliveryMotorbike::ApplyTrunkPresentation()
{
	if (!TrunkInteractable)
	{
		return;
	}
	/**
	 * 文字只按"箱子里有没有货"切，不按玩家手上有什么
	 *
	 * 提示浮窗是在本机按当前瞄到的目标推的，而能不能按由 CanUseTrunk 管；
	 * 这里再按玩家状态分一次会出现两处判断，表现是"写着取货、按下去装货"。
	 */
	TrunkInteractable->PromptText = HasCargo() ? TrunkUnloadPromptText : TrunkLoadPromptText;
	// 有人骑着的时候别人不该还能在车尾开箱
	TrunkInteractable->bInteractEnabled = AllowsPlayerUse() && Driver == nullptr;
}

// 服务器：把手上的快递装进后备箱，并让两端开始播动画
bool ADeliveryMotorbike::BeginTrunkLoad(ADeliveryCharacter* Character)
{
	if (!HasAuthority() || !CanLoadTrunk(Character))
	{
		return false;
	}
	UDeliveryInventoryComponent* Inventory = Character->GetInventoryComponent();
	ADeliveryPackageItem* Package =
	    Inventory ? Cast<ADeliveryPackageItem>(Inventory->GetHeldItem()) : nullptr;
	if (!Package)
	{
		return false;
	}

    const TWeakObjectPtr<ADeliveryMotorbike> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCharacter> WeakCharacter(Character);
    const TWeakObjectPtr<ADeliveryPackageItem> WeakPackage(Package);
    const TWeakObjectPtr<APlayerState> LoadingPlayer(Character->GetTaskPlayerState());
    if (!LoadingPlayer.IsValid() || LoadingPlayer->IsInactive() || LoadingPlayer->GetWorld() != GetWorld()) return false;
    const FVector DropLocation=Character->GetActorLocation() +
        Character->GetActorForwardVector().GetSafeNormal2D()*Inventory->GetDropForwardDistance() + FVector::UpVector*Inventory->GetDropUpOffset();
    // 清槽通知可能改变角色或车辆，只有本次实际装货成功才写入玩家登记
    if (!Inventory->ConsumeHeldItem(Package)) return false;
    if (!WeakThis.IsValid() || IsActorBeingDestroyed() || !WeakCharacter.IsValid() ||
        !WeakPackage.IsValid() || Character->GetTaskPlayerState() != LoadingPlayer.Get() ||
        !LoadingPlayer.IsValid() || LoadingPlayer->IsInactive() || Driver || IsTrunkBusy() ||
        !AttachCargoForPlayer(Package, LoadingPlayer.Get()))
    {
        if (WeakPackage.IsValid() && !Package->GetOwner())
        {
            if (WeakCharacter.IsValid() && LoadingPlayer.IsValid() && Character->GetTaskPlayerState()==LoadingPlayer.Get())
                Character->TryReceiveTaskPackage(Package);
            if (WeakPackage.IsValid() && !Package->GetOwner()) Package->DropFromInventory(DropLocation,FVector::ZeroVector);
        }
        return false;
    }

    if (!WeakThis.IsValid() || IsActorBeingDestroyed() || !WeakCharacter.IsValid() ||
        !WeakPackage.IsValid() || GetCargo() != Package) return false;
    if (auto* Summon=Character->FindComponentByClass<UDeliveryVehicleSummonComponent>()) Summon->RegisterVehicle(this);
	TrunkPendingReceiver.Reset();
	MulticastTrunkAction(EDeliveryTrunkPhase::Loading);

	return true;
}

// 服务器：开盖把快递交回角色手上
bool ADeliveryMotorbike::BeginTrunkUnload(ADeliveryCharacter* Character)
{
	if (!HasAuthority() || !CanUnloadTrunk(Character))
	{
		return false;
	}

	// 快递先留在车上，等合盖那一刻再交接；中途被打断的话它仍然在箱子里
	TrunkPendingReceiver = Character;
	MulticastTrunkAction(EDeliveryTrunkPhase::Unloading);

	return true;
}

// 两端各自从头播同一条时间线，不同步中间进度——这是个一秒多的表现动画
void ADeliveryMotorbike::MulticastTrunkAction_Implementation(EDeliveryTrunkPhase Phase)
{
	TrunkPhase = Phase;
	TrunkElapsed = 0.0f;
	if (ADeliveryPackageItem* Package = GetCargo(); Package && Package->GetOwner() == this)
	{
		// 动画期间看得见：装是从箱口落进去，卸是从箱里升出来
		Package->SetActorHiddenInGame(false);
	}
	ApplyTrunkPresentation();
}

// 箱盖抬起来的是铰链对面那条边；局部 +Y 朝车头
FVector ADeliveryMotorbike::GetTrunkLidLiftDirection() const
{
	return bTrunkLidHingeAtFront ? -FVector::YAxisVector : FVector::YAxisVector;
}

/**
 * 箱盖角度：0 = 关好，1 = 开到 TrunkLidOpenAngle
 *
 * **用 FQuat 绕 X 轴转，不用 FRotator 的 Roll**：绕 +X 的四元数旋转在代数上一定是
 * +Y → +Z（θ=90° 时），所以"要抬起来的那条边"朝 −Y 时就得取负角。方向是推得出来的，
 * 不依赖 FRotator 里 Roll 的正负约定——那个约定本仓库的注释和引擎实现曾经对不上，
 * 而"盖子往反方向翻、穿进车身"这种错只能靠看才发现。
 */
void ADeliveryMotorbike::ApplyTrunkLidAlpha(float Alpha)
{
	CurrentTrunkLidAlpha = FMath::Clamp(Alpha, 0.f, 1.f);
	if (TrunkLidPivot)
	{
		// 抬 −Y 那条边要反向转：正角把 +Y 抬向 +Z
		const float Sign = GetTrunkLidLiftDirection().Y >= 0.0f ? 1.0f : -1.0f;
		TrunkLidPivot->SetRelativeRotation(FQuat(
		    FVector::XAxisVector, FMath::DegreesToRadians(FMath::Abs(TrunkLidOpenAngle) * Alpha * Sign)));
	}
}

/**
 * 推进装卸动画
 *
 * 时间线（装和卸共用，只是快递走的方向相反）：
 *   [0, 盖子时长)                 开盖
 *   [盖子时长, +快递时长)         快递在箱口和箱内之间移动
 *   [..., +盖子时长]              合盖
 * 结束那一刻才做真正的交接（装：藏起来；卸：交回手上），所以动画被打断时
 * 快递仍然在箱子里，不会凭空出现在两个地方。
 */
void ADeliveryMotorbike::UpdateTrunkAnimation(float DeltaSeconds)
{
    if (HasAuthority() && CargoHandoffId.IsValid())
    {
        const FGuid Id = CargoHandoffId;
        const TWeakObjectPtr<ADeliveryMotorbike> WeakThis(this);
        if (!IsCargoHandoffCurrent(Id))
        {
            if (WeakThis.IsValid()) CancelCargoHandoff(Id);
            return;
        }
    }
	if (TrunkPhase == EDeliveryTrunkPhase::Idle)
	{
		return;
	}

	const float LidTime = FMath::Max(TrunkLidDuration, 0.01f);
	const float MoveTime = FMath::Max(TrunkMoveDuration, 0.01f);
	const float Total = LidTime * 2.0f + MoveTime;
	TrunkElapsed = FMath::Min(TrunkElapsed + DeltaSeconds, Total);

	// 盖子：开、全开、合
	float LidAlpha = 1.0f;
	if (TrunkElapsed < LidTime)
	{
		LidAlpha = TrunkElapsed / LidTime;
	}
	else if (TrunkElapsed > LidTime + MoveTime)
	{
		LidAlpha = 1.0f - (TrunkElapsed - LidTime - MoveTime) / LidTime;
	}
	ApplyTrunkLidAlpha(FMath::Clamp(LidAlpha, 0.0f, 1.0f));

	// 快递：在箱口和箱内之间走一趟。位置是相对 CargoPivot（它就在箱内）的偏移
	if (ADeliveryPackageItem* Package = GetCargo(); Package && Package->GetOwner() == this)
	{
		const float MoveAlpha =
		    FMath::Clamp((TrunkElapsed - LidTime) / MoveTime, 0.0f, 1.0f);
		// 装货时从箱口往箱内走，卸货时反过来
		const float OutsideWeight =
		    TrunkPhase == EDeliveryTrunkPhase::Loading ? 1.0f - MoveAlpha : MoveAlpha;
		Package->SetActorRelativeLocation((TrunkOpeningOffset - TrunkInsideOffset) * OutsideWeight);
	}

	if (TrunkElapsed < Total)
	{
		return;
	}

    if (HasAuthority() && CargoHandoffId.IsValid())
    {
        CompleteCargoHandoff();
        return;
    }

	// 时间线走完：收尾
	const EDeliveryTrunkPhase Finished = TrunkPhase;
	TrunkPhase = EDeliveryTrunkPhase::Idle;
	TrunkElapsed = 0.0f;
	ApplyTrunkLidAlpha(0.0f);

    const TWeakObjectPtr<ADeliveryMotorbike> WeakThis(this);
    if (Finished == EDeliveryTrunkPhase::Unloading && HasAuthority())
    {
        const TWeakObjectPtr<ADeliveryCharacter> Receiver=TrunkPendingReceiver;
        TrunkPendingReceiver.Reset();
        const TWeakObjectPtr<APlayerState> LoadingPlayer(GetCargoPlayerState());
        // 记录箱口当前真实位置，回装失败时恢复这件实物，不能用旧拾取点
        const FVector ReleaseLocation=GetCargo()?GetCargo()->GetActorLocation():GetTaskPackageTransferPoint();
        const TWeakObjectPtr<ADeliveryPackageItem> Released(ReleaseCargo());
        const bool bReceived=WeakThis.IsValid() && !IsActorBeingDestroyed() && Released.IsValid() && Receiver.IsValid() &&
            Receiver->ReceiveTaskPackage(Released.Get(),false);
        if (!bReceived && Released.IsValid())
        {
            if (!Released->GetOwner() && WeakThis.IsValid() && !IsActorBeingDestroyed())
                AttachCargoForPlayer(Released.Get(),LoadingPlayer.Get());
            const bool bRestored=WeakThis.IsValid() && !IsActorBeingDestroyed() && Released.IsValid() &&
                GetCargo()==Released.Get() && Released->GetOwner()==this;
            if (bRestored)
            {
                UE_LOG(LogDelivery,Log,TEXT("%s：后备箱取货失败，原包裹已确认留在车上。"),*GetNameSafe(WeakThis.Get()));
            }
            else if (Released.IsValid() && !Released->GetOwner())
            {
                Released->DropFromInventory(ReleaseLocation,FVector::ZeroVector);
                UE_LOG(LogDelivery,Warning,TEXT("后备箱取货回装失败，已为无人持有的原包裹请求恢复箱口处的物理掉落：%s"),*GetNameSafe(Released.Get()));
            }
            else
            {
                UE_LOG(LogDelivery,Warning,TEXT("后备箱取货回调改变了原包裹归属，保留当前实际持有者：%s"),
                    Released.IsValid()?*GetNameSafe(Released->GetOwner()):TEXT("包裹已销毁"));
            }
        }
    }
    if (!WeakThis.IsValid() || IsActorBeingDestroyed()) return;

	// 装好之后把快递藏进关好的箱子；取货失败重新入箱时同样要藏
	ApplyCargoPresentation();
}
