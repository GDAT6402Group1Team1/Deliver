// Copyright Epic Games, Inc. All Rights Reserved.

#include "NPC/DeliveryPedestrianRoute.h"

#include "Animation/AnimSequence.h"
#include "Delivery.h"
#include "Components/SplineComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "NPC/DeliveryPedestrian.h"

ADeliveryPedestrianRoute::ADeliveryPedestrianRoute()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	Spline = CreateDefaultSubobject<USplineComponent>(TEXT("Route"));
	SetRootComponent(Spline);
	// 默认就是一条直线段：两个点、不闭合，拖端点即可改
	// 留着样条而不用两个裸坐标，是为了以后想让人流绕过花坛时不必换类
	Spline->ClearSplinePoints(false);
	Spline->AddSplinePoint(FVector::ZeroVector, ESplineCoordinateSpace::Local, false);
	Spline->AddSplinePoint(FVector(1200.f, 0.f, 0.f), ESplineCoordinateSpace::Local, false);
	Spline->SetSplinePointType(0, ESplinePointType::Linear, false);
	Spline->SetSplinePointType(1, ESplinePointType::Linear, false);
	Spline->SetClosedLoop(false, false);
	Spline->UpdateSpline();

	bReplicates = true;
}

void ADeliveryPedestrianRoute::BeginPlay()
{
	Super::BeginPlay();

	// 生成只在服务器做，路人自己复制过去；两边各生成一遍会出现双份人
	if (!HasAuthority())
	{
		SetActorTickEnabled(false);
		return;
	}

	ResetSpawnTimer();

	if (bPrewarmOnBeginPlay)
	{
		// 开局就铺满，位置沿线段随机。不铺的话玩家一进图看到的是空街，
		// 人要等十几秒才从端点陆续冒出来
		for (int32 Index = 0; Index < MaxAlive; ++Index)
		{
			SpawnOnePedestrian(false);
		}
	}
}

void ADeliveryPedestrianRoute::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!HasAuthority())
	{
		return;
	}

	SpawnCountdown -= DeltaTime;
	if (SpawnCountdown > 0.f)
	{
		return;
	}

	ResetSpawnTimer();

	if (CompactAlive() < MaxAlive)
	{
		SpawnOnePedestrian(true);
	}
}

float ADeliveryPedestrianRoute::GetRouteLength() const
{
	return Spline ? Spline->GetSplineLength() : 0.f;
}

FVector ADeliveryPedestrianRoute::GetLocationAtDistance(float Distance) const
{
	if (!Spline)
	{
		return GetActorLocation();
	}

	return Spline->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
}

FVector ADeliveryPedestrianRoute::GetDirectionAtDistance(float Distance) const
{
	if (!Spline)
	{
		return GetActorForwardVector();
	}

	// 不用 GetDirectionAtDistanceAlongSpline：本项目的样条上它返回过零向量
	// （见 CLAUDE.md 的「端点切线」一条），而方向错了表现是路人原地朝 +X 走
	const FVector Tangent =
	    Spline->GetTangentAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
	if (!Tangent.IsNearlyZero())
	{
		return Tangent.GetSafeNormal();
	}

	// 兜底用两个采样点作差，而不是退回一个固定轴向：
	// 固定轴向会把"取不到方向"伪装成"方向朝 +X"，查起来和真实原因毫无关系
	const float Length = GetRouteLength();
	const float A = FMath::Clamp(Distance - 10.f, 0.f, Length);
	const float B = FMath::Clamp(Distance + 10.f, 0.f, Length);
	const FVector Delta = GetLocationAtDistance(B) - GetLocationAtDistance(A);
	return Delta.IsNearlyZero() ? GetActorForwardVector() : Delta.GetSafeNormal();
}

void ADeliveryPedestrianRoute::NotifyPedestrianFinished(ADeliveryPedestrian* Pedestrian)
{
	Alive.RemoveAll([Pedestrian](const TWeakObjectPtr<ADeliveryPedestrian>& Entry)
	                { return !Entry.IsValid() || Entry.Get() == Pedestrian; });
}

int32 ADeliveryPedestrianRoute::CompactAlive()
{
	Alive.RemoveAll([](const TWeakObjectPtr<ADeliveryPedestrian>& Entry) { return !Entry.IsValid(); });
	return Alive.Num();
}

USkeletalMesh* ADeliveryPedestrianRoute::PickKind(float& OutHeightScale) const
{
	OutHeightScale = 1.f;

	float TotalWeight = 0.f;
	for (const FDeliveryPedestrianKind& Kind : Kinds)
	{
		if (Kind.Weight > 0.f && !Kind.Mesh.IsNull())
		{
			TotalWeight += Kind.Weight;
		}
	}

	if (TotalWeight <= 0.f)
	{
		return nullptr;
	}

	float Roll = FMath::FRand() * TotalWeight;
	for (const FDeliveryPedestrianKind& Kind : Kinds)
	{
		if (Kind.Weight <= 0.f || Kind.Mesh.IsNull())
		{
			continue;
		}

		Roll -= Kind.Weight;
		if (Roll <= 0.f)
		{
			OutHeightScale = FMath::Max(Kind.HeightScale, 0.1f);
			// 软引用现在才同步加载：这些资产由导入脚本现建，
			// 硬引用会让"还没导入"变成关卡加载报错
			return Kind.Mesh.LoadSynchronous();
		}
	}

	return nullptr;
}

void ADeliveryPedestrianRoute::ResetSpawnTimer()
{
	const float Min = FMath::Max(SpawnIntervalMin, 0.f);
	const float Max = FMath::Max(SpawnIntervalMax, Min);
	SpawnCountdown = FMath::FRandRange(Min, Max);
}

ADeliveryPedestrian* ADeliveryPedestrianRoute::SpawnOnePedestrian(bool bAtEntrance)
{
	UWorld* World = GetWorld();
	if (!World || !HasAuthority())
	{
		return nullptr;
	}

	const float Length = GetRouteLength();
	if (Length <= KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogDelivery, Warning, TEXT("[Pedestrian] %s 的线段长度为 0，摆两个端点才会有人出现。"),
		       *GetName());
		return nullptr;
	}

	float HeightScale = 1.f;
	USkeletalMesh* PickedMesh = PickKind(HeightScale);
	if (!PickedMesh)
	{
		// 这种空配置最难查：点摆了、脚本跑了，却一个人都不出现而且没有任何报错
		UE_LOG(LogDelivery, Warning,
		       TEXT("[Pedestrian] %s 没有可用的路人外观（Kinds 为空、权重全为零，或资产抽不出来）。"),
		       *GetName());
		return nullptr;
	}

	const bool bWalkForward = bBothDirections ? (FMath::FRand() < 0.5f) : true;
	const float StartDistance = bAtEntrance ? (bWalkForward ? 0.f : Length)
	                                        : FMath::FRandRange(0.f, Length);

	FVector SpawnLocation = GetLocationAtDistance(StartDistance);

	// 横向抖一下，免得人流排成一条笔直的线
	if (LateralJitter > 0.f)
	{
		const FVector Forward = GetDirectionAtDistance(StartDistance);
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward).GetSafeNormal();
		SpawnLocation += Right * FMath::FRandRange(-LateralJitter, LateralJitter);
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = this;
	// 路人只是布景，落点被别的东西占着也照样出现：挤一下总比"人流时断时续"好
	SpawnParams.SpawnCollisionHandlingOverride =
	    ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// 三元里混用 TSubclassOf 和 UClass* 会编不过（两者可以互相转换，表达式有歧义），
	// 所以显式写成 UClass*
	UClass* ClassToSpawn = PedestrianClass.Get();
	if (!ClassToSpawn)
	{
		ClassToSpawn = ADeliveryPedestrian::StaticClass();
	}

	// 身高写进出生变换，而不是生成之后再 SetActorScale3D：
	// 路人在 BeginPlay 里把当时的缩放记成 BaseScale，出入场曲线乘在它上面。
	// 生成后再改的话 BaseScale 已经被记成 1，这一下会被出入场缩放当场覆盖掉
	const FTransform SpawnTransform(GetDirectionAtDistance(StartDistance).Rotation(), SpawnLocation,
	                                FVector(HeightScale));
	ADeliveryPedestrian* Pedestrian =
	    World->SpawnActor<ADeliveryPedestrian>(ClassToSpawn, SpawnTransform, SpawnParams);
	if (!Pedestrian)
	{
		return nullptr;
	}

	const float Speed = FMath::FRandRange(FMath::Max(WalkSpeedMin, 1.f),
	                                      FMath::Max(WalkSpeedMax, FMath::Max(WalkSpeedMin, 1.f)));

	Pedestrian->BeginWalk(this, PickedMesh, WalkAnim.LoadSynchronous(), StartDistance, bWalkForward,
	                      Speed, WalkAnimReferenceSpeed);

	Alive.Add(Pedestrian);
	return Pedestrian;
}

void ADeliveryPedestrianRoute::GetEndpoints(FVector& OutStart, FVector& OutFinish) const
{
	OutStart = GetLocationAtDistance(0.f);
	OutFinish = GetLocationAtDistance(GetRouteLength());
}

void ADeliveryPedestrianRoute::BuildConnections()
{
	bConnectionsBuilt = true;
	Connections[0].Reset();
	Connections[1].Reset();

	const UWorld* World = GetWorld();
	if (!World || GetRouteLength() <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	FVector MyEnds[2];
	GetEndpoints(MyEnds[0], MyEnds[1]);

	const float ConnectSq = FMath::Square(FMath::Max(ConnectDistance, 1.f));

	for (TActorIterator<ADeliveryPedestrianRoute> It(GetWorld()); It; ++It)
	{
		ADeliveryPedestrianRoute* Other = *It;
		if (!Other || Other == this || Other->GetRouteLength() <= KINDA_SMALL_NUMBER)
		{
			continue;
		}

		FVector OtherEnds[2];
		Other->GetEndpoints(OtherEnds[0], OtherEnds[1]);

		for (int32 MyEnd = 0; MyEnd < 2; ++MyEnd)
		{
			for (int32 OtherEnd = 0; OtherEnd < 2; ++OtherEnd)
			{
				// 只认**端点到端点**。中段靠得近的两条路不算连通，
				// 否则路人会在路中央凭空拐上另一条线
				if (FVector::DistSquared(MyEnds[MyEnd], OtherEnds[OtherEnd]) > ConnectSq)
				{
					continue;
				}
				FConnection Connection;
				Connection.Route = Other;
				// 从对方的哪一头进去，就从那一头开始走
				Connection.bEnterAtStart = (OtherEnd == 0);
				Connections[MyEnd].Add(Connection);
			}
		}
	}

	UE_LOG(LogDelivery, Verbose, TEXT("[Pedestrian] %s 连通表：起点端 %d 条、终点端 %d 条。"),
	       *GetName(), Connections[0].Num(), Connections[1].Num());
}

bool ADeliveryPedestrianRoute::HasContinuation(bool bFromFinishEnd)
{
	if (!bAllowHandoff)
	{
		return false;
	}
	if (!bConnectionsBuilt)
	{
		BuildConnections();
	}

	const TArray<FConnection>& Candidates = Connections[bFromFinishEnd ? 1 : 0];
	for (const FConnection& Connection : Candidates)
	{
		if (Connection.Route.IsValid())
		{
			return true;
		}
	}
	return false;
}

bool ADeliveryPedestrianRoute::FindContinuation(bool bFromFinishEnd,
                                                ADeliveryPedestrianRoute*& OutRoute,
                                                bool& OutEnterAtStart)
{
	OutRoute = nullptr;
	OutEnterAtStart = true;

	if (!bAllowHandoff)
	{
		return false;
	}
	if (!bConnectionsBuilt)
	{
		BuildConnections();
	}

	// 随机挑一条能走的，不总是挑第一条——固定挑第一条的话所有人走同一条路线，
	// 路网画了也看不出分叉
	TArray<const FConnection*> Usable;
	for (const FConnection& Connection : Connections[bFromFinishEnd ? 1 : 0])
	{
		ADeliveryPedestrianRoute* Candidate = Connection.Route.Get();
		if (!Candidate)
		{
			continue;
		}
		// 目标路线满员就不接：人数上限仍然要守，否则热门路线会越挤越多
		if (Candidate->CompactAlive() >= Candidate->MaxAlive)
		{
			continue;
		}
		Usable.Add(&Connection);
	}

	if (Usable.IsEmpty())
	{
		return false;
	}

	const FConnection* Picked = Usable[FMath::RandRange(0, Usable.Num() - 1)];
	OutRoute = Picked->Route.Get();
	OutEnterAtStart = Picked->bEnterAtStart;
	return OutRoute != nullptr;
}

void ADeliveryPedestrianRoute::AcceptHandoff(ADeliveryPedestrian* Pedestrian)
{
	if (Pedestrian)
	{
		Alive.AddUnique(Pedestrian);
	}
}
