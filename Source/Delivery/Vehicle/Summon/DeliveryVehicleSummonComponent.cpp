// Copyright Epic Games, Inc. All Rights Reserved.

#include "Vehicle/Summon/DeliveryVehicleSummonComponent.h"
#include "Delivery.h"
#include "Character/DeliveryCharacter.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "Inventory/Items/DeliveryPackageItem.h"
#include "Net/UnrealNetwork.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Vehicle/Motorbike/DeliveryMotorbike.h"
#include "Vehicle/Trunk/DeliveryTrunkPassengerComponent.h"

namespace
{
bool CarriesRegisteredPlayerPackage(const ADeliveryMotorbike* Bike, const APlayerState* Player)
{
    if (!IsValid(Bike) || Bike->IsActorBeingDestroyed() || Bike->GetWorld()!=Player->GetWorld() ||
        !Bike->CanBeUsedByPlayers() || Bike->GetRegisteredPlayer()!=Player || Bike->GetCargoPlayerState()!=Player)
        return false;

    const auto* Package=Bike->GetCargo();
    if (!IsValid(Package) || Package->IsActorBeingDestroyed() || Package->GetWorld()!=Bike->GetWorld() ||
        Package->GetOwner()!=Bike) return false;
    const auto* Item=Package->GetDeliveryItemComponent();
    const auto* Task=IsValid(Item)?Item->OwningTask.Get():nullptr;
    const auto* Tasks=UDeliveryTaskManagerComponent::Get(Bike);
    return IsValid(Task) && Tasks && Tasks->GetActiveTask()==Task;
}
}

UDeliveryVehicleSummonComponent::UDeliveryVehicleSummonComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
	DefaultVehicleClass = TSoftClassPtr<ADeliveryMotorbike>(
	    FSoftObjectPath(TEXT("/Game/Vehicle/Motorbike/BP_Motorbike.BP_Motorbike_C")));

}

// 同步召唤车辆引用和冷却时间
void UDeliveryVehicleSummonComponent::GetLifetimeReplicatedProps(
    TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// 只发给拥有者：别人的冷却和我无关，没必要占带宽
	DOREPLIFETIME_CONDITION(UDeliveryVehicleSummonComponent, ReadyServerTime, COND_OwnerOnly);
}

void UDeliveryVehicleSummonComponent::RegisterVehicle(ADeliveryMotorbike* Bike)
{
    const auto* Character = Cast<ADeliveryCharacter>(GetOwner());
    auto* Player = Character ? Character->GetTaskPlayerState() : nullptr;
    if (!Character || !Character->HasAuthority() || !IsValid(Bike) || Bike->IsActorBeingDestroyed() ||
        Bike->GetWorld() != GetWorld() || !Bike->CanBeUsedByPlayers() || !IsValid(Player) ||
        Player->IsInactive() || Player->GetWorld() != GetWorld()) return;
    Bike->RegisterPlayerUse(Player);
    if (Bike->GetRegisteredPlayer() != Player) return;
    RegisteredVehicle = Bike;
    // 玩家改骑空车时保留仍有原订单实物的车，卸下后直接按实物状态失效
    if (!CarriesRegisteredPlayerPackage(RegisteredCargoVehicle.Get(),Player) && CarriesRegisteredPlayerPackage(Bike,Player))
        RegisteredCargoVehicle = Bike;
}

ADeliveryMotorbike* UDeliveryVehicleSummonComponent::GetRegisteredVehicle() const
{
    const auto* Character = Cast<ADeliveryCharacter>(GetOwner());
    auto* Player = Character ? Character->GetTaskPlayerState() : nullptr;
    auto* Bike = RegisteredVehicle.Get();
    return IsValid(Player) && !Player->IsInactive() && Player->GetWorld() == GetWorld() &&
        IsValid(Bike) && !Bike->IsActorBeingDestroyed() && Bike->GetWorld() == GetWorld() &&
        Bike->CanBeUsedByPlayers() && Bike->GetRegisteredPlayer() == Player ? Bike : nullptr;
}

ADeliveryMotorbike* UDeliveryVehicleSummonComponent::GetRegisteredCargoVehicle() const
{
    const auto* Character=Cast<ADeliveryCharacter>(GetOwner());
    const auto* Player=IsValid(Character)?Character->GetTaskPlayerState():nullptr;
    if (!IsValid(Player) || Player->IsInactive() || Player->GetWorld()!=GetWorld()) return nullptr;
    if (auto* CargoBike=RegisteredCargoVehicle.Get(); CarriesRegisteredPlayerPackage(CargoBike,Player)) return CargoBike;
    auto* RecentBike=RegisteredVehicle.Get();
    return CarriesRegisteredPlayerPackage(RecentBike,Player)?RecentBike:nullptr;
}

float UDeliveryVehicleSummonComponent::GetCooldownRemaining() const
{
    const UWorld* World = GetWorld();
    if (!World) return 0.0f;
    const AGameStateBase* State = World->GetGameState();
    const float Now = State ? State->GetServerWorldTimeSeconds() : World->GetTimeSeconds();
    return FMath::Max(0.0f, ReadyServerTime - Now);
}

bool UDeliveryVehicleSummonComponent::CanOfferSummon() const
{
    if (!DefaultVehicleClass.IsNull()) return true;
    if (UWorld* World = GetWorld())
    {
        for (TActorIterator<ADeliveryMotorbike> It(World); It; ++It)
        {
            if (IsValid(*It) && !It->IsActorBeingDestroyed() && It->CanBeUsedByPlayers()) return true;
        }
    }
    return false;
}

// 客户端收到新的冷却结束时间后，通知提示组件重新读取
void UDeliveryVehicleSummonComponent::OnRep_ReadyServerTime()
{
    OnCooldownChanged.Broadcast();
}

// 将本机召唤意图提交给服务器
void UDeliveryVehicleSummonComponent::RequestSummon()
{
	// 本机先检查冷却，减少无效请求；服务器收到后还会再次检查
	if (GetCooldownRemaining() > 0.0f)
	{
		return;
	}
	ServerSummon();
}

// 查找范围内最近的空闲摩托车
ADeliveryMotorbike* UDeliveryVehicleSummonComponent::FindSummonTarget() const
{
	UWorld* World = GetWorld();
	const AActor* Owner = GetOwner();
	if (!World || !Owner)
	{
		return nullptr;
	}

	const FVector From = Owner->GetActorLocation();
	ADeliveryMotorbike* Best = nullptr;
	float BestDistSq = TNumericLimits<float>::Max();

	for (TActorIterator<ADeliveryMotorbike> It(World); It; ++It)
	{
		ADeliveryMotorbike* Bike = *It;
		// 别人正骑着的车不能抽走
		if (!IsValid(Bike) || !Bike->CanBeUsedByPlayers() || Bike->GetDriver() != nullptr || Bike->HasTrunkPassenger())
		{
			continue;
		}
		const float DistSq = FVector::DistSquared(From, Bike->GetActorLocation());
		if (SearchRadius > 0.0f && DistSq > FMath::Square(SearchRadius))
		{
			continue;
		}
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = Bike;
		}
	}
	return Best;
}

// 由服务器检查冷却和位置，再创建或移动车辆
void UDeliveryVehicleSummonComponent::ServerSummon_Implementation()
{
    const auto* PassengerOwner = Cast<ADeliveryCharacter>(GetOwner());
    if (PassengerOwner && PassengerOwner->TrunkPassenger && PassengerOwner->TrunkPassenger->IsPassenger()) return;
	UWorld* World = GetWorld();
	AActor* Owner = GetOwner();
	if (!World || !Owner)
	{
		return;
	}

	const float Now = World->GetTimeSeconds();
	if (Now < ReadyServerTime)
	{
		return;
	}

	// 召唤位置取玩家前方，车头保持与玩家相同的水平朝向
	const FRotator OwnerRot = Owner->GetActorRotation();
	const FVector Forward = FRotator(0.0f, OwnerRot.Yaw, 0.0f).Vector();
	const FVector Target = Owner->GetActorLocation() + Forward * PlaceDistance;

	ADeliveryMotorbike* Bike = FindSummonTarget();
	bool bSpawnedBike = false;
	if (!Bike)
	{
		// 只补地图缺车的情况；已有车被占用或超出搜索范围时不能靠新生成绕过规则
		for (TActorIterator<ADeliveryMotorbike> It(World); It; ++It)
		{
			if (IsValid(*It) && It->CanBeUsedByPlayers())
			{
				UE_LOG(LogDelivery, Log, TEXT("召唤失败：没有可召回的空闲摩托车。"));
				return;
			}
		}
		UClass* VehicleClass = DefaultVehicleClass.LoadSynchronous();
		if (!VehicleClass)
		{
			UE_LOG(LogDelivery, Warning, TEXT("召唤失败：地图没有摩托车，默认车型无法加载：%s"),
			       *DefaultVehicleClass.ToString());
			return;
		}
		FActorSpawnParameters SpawnParams;
		// SummonTo 负责探地落位；失败时销毁临时车辆，不留下空中车或消耗冷却
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Bike = World->SpawnActor<ADeliveryMotorbike>(VehicleClass, Target, FRotator(0.0f, OwnerRot.Yaw, 0.0f),
		                                             SpawnParams);
		if (!Bike)
		{
			UE_LOG(LogDelivery, Warning, TEXT("召唤失败：无法生成默认摩托车。"));
			return;
		}
		bSpawnedBike = true;
	}

	// 找不到可用落点时取消召唤，不消耗冷却时间
	if (!Bike->SummonTo(Target, OwnerRot.Yaw))
	{
		UE_LOG(LogDelivery, Log, TEXT("召唤失败：%s 的落点探不到地面，不计冷却。"), *Bike->GetName());
		if (bSpawnedBike)
		{
			Bike->Destroy();
		}
		return;
	}
	RegisterVehicle(Bike);
	Bike->ForceNetUpdate();

	ReadyServerTime = Now + Cooldown;
    // 房主不经过属性复制回调，因此服务器修改后也要发送通知
    OnCooldownChanged.Broadcast();
    Owner->ForceNetUpdate();
}
