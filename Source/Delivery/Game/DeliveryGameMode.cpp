// Copyright Epic Games, Inc. All Rights Reserved.

#include "Game/DeliveryGameMode.h"
#include "Game/DeliveryStartupFlow.h"
#include "UI/DeliveryHUD.h"
#include "Character/DeliveryCharacter.h"
#include "Game/DeliveryGameState.h"
#include "Player/DeliveryPlayerController.h"
#include "Player/DeliverPlayerState.h"
#include "UObject/ConstructorHelpers.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"

ADeliveryGameMode::ADeliveryGameMode()
{
	// 创建 Pawn 和 PlayerController
	DefaultPawnClass = ADeliveryCharacter::StaticClass(); // 兜底从C++创建
	PlayerControllerClass = ADeliveryPlayerController::StaticClass();
	HUDClass = ADeliveryHUD::StaticClass();
	PlayerStateClass = ADeliverPlayerState::StaticClass();

	// 任务状态和电话队列挂在 GameState 上；关卡的任务清单要在 GameState 蓝图里配，
	// 所以实际使用时应在 BP_DeliverGameMode 里把这一项换成对应的蓝图子类
	GameStateClass = ADeliveryGameState::StaticClass();

	static ConstructorHelpers::FClassFinder<APawn> PawnBP(
	    TEXT("/Game/Blueprint/Character/Deliverman/BP_DeliveryMan")); // 正常从蓝图创建
	if (PawnBP.Succeeded())
	{
		DefaultPawnClass = PawnBP.Class;
	}

	static ConstructorHelpers::FClassFinder<APlayerController> ControllerBP(
	    TEXT("/Game/Blueprint/PlayerController/BP_DeliveryManPC"));
	if (ControllerBP.Succeeded())
	{
		PlayerControllerClass = ControllerBP.Class;
	}
}

void ADeliveryGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer)
{
	// 启动地图先加载地面，出生统一由启动服务在玩家就绪后执行
	if (ADeliveryStartupFlow::GetDeliveryStartupFlow(this)) return;
	Super::HandleStartingNewPlayer_Implementation(NewPlayer);
}

bool ADeliveryGameMode::PlayerCanRestart_Implementation(APlayerController* Player)
{
	if (const auto* Flow = ADeliveryStartupFlow::GetDeliveryStartupFlow(this))
		if (!Flow->CanSpawnPlayer(Player)) return false;
	return Super::PlayerCanRestart_Implementation(Player);
}

APawn* ADeliveryGameMode::SpawnDefaultPawnAtTransform_Implementation(AController* NewPlayer, const FTransform& SpawnTransform)
{
	UClass* PawnClass = GetDefaultPawnClassForController(NewPlayer);
	const APawn* Template = PawnClass ? PawnClass->GetDefaultObject<APawn>() : nullptr;
	const auto* Capsule = Template ? Cast<UCapsuleComponent>(Template->GetRootComponent()) : nullptr;
	if (!Capsule || !GetWorld()) return Super::SpawnDefaultPawnAtTransform_Implementation(NewPlayer, SpawnTransform);

	const FVector Origin = SpawnTransform.GetLocation();
	const FRotator Rotation = SpawnTransform.Rotator();
	// 正常出生沿用引擎流程；PIE 的 PlayFromHere 会给多个玩家返回同一个起点
	if (!GetWorld()->EncroachingBlockingGeometry(Template, Origin, Rotation))
		if (APawn* Pawn = Super::SpawnDefaultPawnAtTransform_Implementation(NewPlayer, SpawnTransform)) return Pawn;

	float Radius, HalfHeight;
	Capsule->GetScaledCapsuleSize(Radius, HalfHeight);
	const float Spacing = FMath::Max(200.f, Radius * 2.f + 50.f);
	FCollisionQueryParams Query(SCENE_QUERY_STAT(DeliveryPlayerSpawn), false);
	// 有限的两圈搜索，只在当前出生点附近找可站立地面，不移动已有玩家或关闭碰撞
	for (int32 Index = 0; Index < 16; ++Index)
	{
		const float Angle = (Index % 8) * UE_PI / 4.f;
		FVector Candidate = Origin + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0) * Spacing * (1 + Index / 8);
		FHitResult Ground;
		if (!GetWorld()->LineTraceSingleByObjectType(Ground, Candidate + FVector(0, 0, HalfHeight),
			Candidate - FVector(0, 0, 500), FCollisionObjectQueryParams(ECC_WorldStatic), Query)
			|| !Ground.GetComponent() || Ground.ImpactNormal.Z < .7f
			|| Ground.GetComponent()->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block) continue;
		Candidate.Z = Ground.ImpactPoint.Z + HalfHeight + 2.f;
		if (GetWorld()->EncroachingBlockingGeometry(Template, Candidate, Rotation)) continue;
		FTransform SafeTransform = SpawnTransform;
		SafeTransform.SetLocation(Candidate);
		if (APawn* Pawn = Super::SpawnDefaultPawnAtTransform_Implementation(NewPlayer, SafeTransform)) return Pawn;
	}
	UE_LOG(LogGameMode, Warning, TEXT("No clear player spawn near %s for %s"), *Origin.ToString(), *GetNameSafe(NewPlayer));
	return nullptr;
}
