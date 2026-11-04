// 验证人行路线的生成上限、沿线段推进、出入场缩放与到端点回收
#include "NPC/DeliveryPedestrianRoute.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Components/SplineComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "NPC/DeliveryPedestrian.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryPedestrianWalkTest, "Delivery.NPC.PedestrianWalk",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryPedestrianWalkTest::RunTest(const FString&)
{
	const auto Settings = UWorld::InitializationValues()
	                          .AllowAudioPlayback(false)
	                          .CreatePhysicsScene(true)
	                          .ShouldSimulatePhysics(false)
	                          .CreateNavigation(false)
	                          .CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
	                                    ERHIFeatureLevel::Num, &Settings);
	if (!TestNotNull(TEXT("test world"), World))
		return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->EndPlay(EEndPlayReason::Quit);
		World->DestroyWorld(false);
	};

	ADeliveryPedestrianRoute* Route = World->SpawnActor<ADeliveryPedestrianRoute>();
	if (!TestNotNull(TEXT("route"), Route))
		return false;

	// 一条 1000 cm 的直线段，只朝一个方向走，方便断言距离单调
	USplineComponent* Spline = Route->GetSpline();
	if (!TestNotNull(TEXT("spline"), Spline))
		return false;
	Spline->ClearSplinePoints(false);
	Spline->AddSplinePoint(FVector::ZeroVector, ESplineCoordinateSpace::World, false);
	Spline->AddSplinePoint(FVector(1000.f, 0.f, 0.f), ESplineCoordinateSpace::World, false);
	Spline->SetSplinePointType(0, ESplinePointType::Linear, false);
	Spline->SetSplinePointType(1, ESplinePointType::Linear, false);
	Spline->UpdateSpline();
	TestEqual(TEXT("route length"), Spline->GetSplineLength(), 1000.f, 1.f);

	Route->bBothDirections = false;
	Route->bPrewarmOnBeginPlay = false;
	// 这一段只验直线行走，先关掉走走停停和接驳；它们各有单独的断言
	Route->bAllowHandoff = false;
	Route->MaxAlive = 3;
	Route->LateralJitter = 0.f;
	Route->WalkSpeedMin = 200.f;
	Route->WalkSpeedMax = 200.f;
	Route->AppearDistance = 200.f;
	Route->MinVisibleScale = 0.02f;

	// 外观用一张临时骨骼网格：测试不依赖导入脚本有没有跑过，
	// 否则这条断言会在没导入 NPC 的机器上失败，而真正的逻辑并没有坏
	USkeletalMesh* DummyMesh = NewObject<USkeletalMesh>(World);
	if (!TestNotNull(TEXT("dummy mesh"), DummyMesh))
		return false;
	// 这张临时网格一根骨骼都没有，所以路人会退回"静态外观"并点名一次。
	// 登记成预期消息，而不是让测试带着警告过。
	// 这条断言本身就是回归点：**把没有骨骼的网格交给 PoseableMesh 会崩**
	// （AllocateTransformData → FillComponentSpaceTransforms 按骨骼数索引，
	// 数组大小 0 时直接断言），第一版就是这么崩在自动化里的
	AddExpectedMessage(TEXT("没有骨骼"), EAutomationExpectedMessageFlags::Contains, 0);
	// 同一张临时网格也没有物理资产，被打飞时会点名并退回"原地退场"。
	// 这条也是回归点：没有物理资产时**不能崩**，也不能静默站着不动
	AddExpectedMessage(TEXT("没有物理资产"), EAutomationExpectedMessageFlags::Contains, 0);

	FDeliveryPedestrianKind Kind;
	Kind.Mesh = DummyMesh;
	Kind.Weight = 1.f;
	Route->Kinds.Add(Kind);

	// 每一段开头清场。**不能跨段做相对运算**：第一版在"人数上限"那段把路线填满到 3，
	// 后面几段还按 AliveBefore-1 断言，于是数字全错——而错的是测试，不是代码
	const auto ClearAll = [&]()
	{
		for (TActorIterator<ADeliveryPedestrian> It(World); It; ++It)
		{
			It->Destroy();
		}
		World->Tick(LEVELTICK_All, 0.01f);
		Route->NotifyPedestrianFinished(nullptr);
	};

	const auto CountPedestrians = [&]()
	{
		int32 Count = 0;
		for (TActorIterator<ADeliveryPedestrian> It(World); It; ++It)
		{
			++Count;
		}
		return Count;
	};

	// 空配置必须报不出人而不是崩：权重全零时一个都不该生成
	Route->Kinds[0].Weight = 0.f;
	TestNull(TEXT("权重为零时不生成"), Route->SpawnOnePedestrian(true));
	Route->Kinds[0].Weight = 1.f;

	ADeliveryPedestrian* Walker = Route->SpawnOnePedestrian(true);
	if (!TestNotNull(TEXT("spawn one"), Walker))
		return false;
	// 停顿会让"走了多远"的断言失效，所以这一段把它关掉（PauseDurationMax = 0）
	Walker->PauseDurationMax = 0.f;
	Walker->PauseDurationMin = 0.f;
	TestEqual(TEXT("从端点进场"), Walker->GetDistanceAlongRoute(), 0.f, 1.f);
	TestTrue(TEXT("单向时正向走"), Walker->IsWalkingForward());
	// 进场那一刻必须是看不见的，否则生成那一帧会有一个原尺寸的人闪一下
	TestTrue(TEXT("进场缩放接近零"), Walker->GetAppearAlpha() < 0.05f);
	TestEqual(TEXT("进场阶段"), static_cast<int32>(Walker->GetPhase()),
	          static_cast<int32>(EDeliveryPedestrianPhase::Appearing));

	// 推进 1 秒（200 cm），应当还在路上、已经完全放大
	float LastDistance = Walker->GetDistanceAlongRoute();
	for (int32 Step = 0; Step < 10; ++Step)
	{
		Walker->Tick(0.1f);
		if (!Walker->IsActorBeingDestroyed())
		{
			TestTrue(TEXT("沿线段单调前进"), Walker->GetDistanceAlongRoute() > LastDistance);
			LastDistance = Walker->GetDistanceAlongRoute();
		}
	}
	TestEqual(TEXT("1 秒后的距离"), Walker->GetDistanceAlongRoute(), 200.f, 5.f);
	TestTrue(TEXT("走出窗口后已满尺寸"), Walker->GetAppearAlpha() > 0.95f);
	TestEqual(TEXT("行走阶段"), static_cast<int32>(Walker->GetPhase()),
	          static_cast<int32>(EDeliveryPedestrianPhase::Walking));
	// 位置要真的沿着线段走，而不是只有内部计数在变
	// UE5 的 FVector 分量是 double（大世界坐标），和 float 字面量混用会让 TestEqual 有歧义
	TestEqual(TEXT("世界位置跟上"), static_cast<float>(Walker->GetActorLocation().X), 200.f, 10.f);

	// 再推进到离终点不足一个窗口，应当进入消失阶段且缩放在回落
	while (Walker->GetDistanceAlongRoute() < 850.f && !Walker->IsActorBeingDestroyed())
	{
		Walker->Tick(0.1f);
	}
	TestEqual(TEXT("消失阶段"), static_cast<int32>(Walker->GetPhase()),
	          static_cast<int32>(EDeliveryPedestrianPhase::Disappearing));
	TestTrue(TEXT("接近终点时缩放回落"), Walker->GetAppearAlpha() < 0.8f);

	// 走到端点必须销毁并从在场列表里摘掉，否则人数上限会被已经退场的人占住
	for (int32 Step = 0; Step < 50 && !Walker->IsActorBeingDestroyed(); ++Step)
	{
		Walker->Tick(0.1f);
	}
	TestTrue(TEXT("到端点后销毁"), Walker->IsActorBeingDestroyed());
	World->Tick(LEVELTICK_All, 0.01f);
	TestEqual(TEXT("退场后在场列表为空"), Route->GetAlivePedestrians().Num(), 0);

	// 人数上限：连续补人不能超过 MaxAlive
	ClearAll();
	for (int32 Index = 0; Index < 10; ++Index)
	{
		if (Route->GetAlivePedestrians().Num() < Route->MaxAlive)
		{
			Route->SpawnOnePedestrian(true);
		}
	}
	TestEqual(TEXT("在场人数达到上限"), Route->GetAlivePedestrians().Num(), Route->MaxAlive);
	TestTrue(TEXT("世界里的路人不超过上限"), CountPedestrians() <= Route->MaxAlive);

	// 接驳：两条首尾相接的路线，走到头应当换过去继续走而不是销毁
	{
		ClearAll();
		TestEqual(TEXT("接驳段开始前已清场"), Route->GetAlivePedestrians().Num(), 0);
		ADeliveryPedestrianRoute* RouteB = World->SpawnActor<ADeliveryPedestrianRoute>();
		if (!TestNotNull(TEXT("route B"), RouteB))
			return false;
		USplineComponent* SplineB = RouteB->GetSpline();
		SplineB->ClearSplinePoints(false);
		// 从 A 的终点 (1000,0) 接出去
		SplineB->AddSplinePoint(FVector(1000.f, 0.f, 0.f), ESplineCoordinateSpace::World, false);
		SplineB->AddSplinePoint(FVector(1000.f, 900.f, 0.f), ESplineCoordinateSpace::World, false);
		SplineB->UpdateSpline();
		RouteB->Kinds.Add(Kind);
		RouteB->bPrewarmOnBeginPlay = false;
		RouteB->bBothDirections = false;
		RouteB->MaxAlive = 3;

		// A 这头要把线段恢复成 1000 长（上面退化成一个点了）
		Spline->ClearSplinePoints(false);
		Spline->AddSplinePoint(FVector::ZeroVector, ESplineCoordinateSpace::World, false);
		Spline->AddSplinePoint(FVector(1000.f, 0.f, 0.f), ESplineCoordinateSpace::World, false);
		Spline->UpdateSpline();
		Route->bAllowHandoff = true;
		Route->ConnectDistance = 400.f;
		Route->MaxHandoffs = 2;

		TestTrue(TEXT("终点端查到下家"), Route->HasContinuation(true));
		// 起点端没有东西接，走到那头就该退场
		TestFalse(TEXT("起点端没有下家"), Route->HasContinuation(false));

		ADeliveryPedestrian* Walker2 = Route->SpawnOnePedestrian(true);
		if (!TestNotNull(TEXT("spawn walker2"), Walker2))
			return false;
		Walker2->PauseDurationMax = 0.f;
		Walker2->PauseDurationMin = 0.f;

		// 走到 A 的终点
		for (int32 Step = 0; Step < 200 && Walker2->GetHandoffCount() == 0 &&
		                     !Walker2->IsActorBeingDestroyed();
		     ++Step)
		{
			Walker2->Tick(0.1f);
		}
		TestFalse(TEXT("接驳时不销毁"), Walker2->IsActorBeingDestroyed());
		TestEqual(TEXT("换线一次"), Walker2->GetHandoffCount(), 1);
		// 换线之后必须**满尺寸**：按原来的出入场逻辑，他正站在新路线的端点上、
		// 会在路口缩到看不见再长回来，这是接驳要专门处理的那个坑
		TestTrue(TEXT("换线后不淡入"), Walker2->GetAppearAlpha() > 0.95f);
		// 在场名单的归属也要跟着换，否则同一个人同时占两条路线的人数上限
		TestEqual(TEXT("已从 A 的名单摘掉"), Route->GetAlivePedestrians().Num(), 0);
		TestEqual(TEXT("已加进 B 的名单"), RouteB->GetAlivePedestrians().Num(), 1);
		RouteB->Destroy();

		Walker2->Destroy();
		World->Tick(LEVELTICK_All, 0.01f);
		Route->bAllowHandoff = false;
	}

	// 走走停停：停着时不前进，停够之后继续
	{
		ClearAll();
		Spline->ClearSplinePoints(false);
		Spline->AddSplinePoint(FVector::ZeroVector, ESplineCoordinateSpace::World, false);
		Spline->AddSplinePoint(FVector(4000.f, 0.f, 0.f), ESplineCoordinateSpace::World, false);
		Spline->UpdateSpline();

		ADeliveryPedestrian* Stroller = Route->SpawnOnePedestrian(true);
		if (!TestNotNull(TEXT("spawn stroller"), Stroller))
			return false;
		// 逼它马上停：走 1 cm 就触发，停 1 秒
		Stroller->PauseIntervalMin = 1.f;
		Stroller->PauseIntervalMax = 1.f;
		Stroller->PauseDurationMin = 1.f;
		Stroller->PauseDurationMax = 1.f;

		// 先走出出入场窗口（窗口里不许停，这是刻意的——
		// 在里面停住会卡成一个半大小的人站着不动）
		while (Stroller->GetDistanceAlongRoute() < Route->AppearDistance + 50.f &&
		       !Stroller->IsActorBeingDestroyed())
		{
			Stroller->Tick(0.05f);
		}
		TestFalse(TEXT("出入场窗口里不停"), Stroller->IsPaused());

		for (int32 Step = 0; Step < 200 && !Stroller->IsPaused(); ++Step)
		{
			Stroller->Tick(0.05f);
		}
		TestTrue(TEXT("走出窗口之后会停"), Stroller->IsPaused());

		const float FrozenAt = Stroller->GetDistanceAlongRoute();
		Stroller->Tick(0.2f);
		TestEqual(TEXT("停着不前进"), Stroller->GetDistanceAlongRoute(), FrozenAt, 0.01f);

		for (int32 Step = 0; Step < 40 && Stroller->IsPaused(); ++Step)
		{
			Stroller->Tick(0.05f);
		}
		TestFalse(TEXT("停够之后继续"), Stroller->IsPaused());
		Stroller->Tick(0.1f);
		TestTrue(TEXT("继续之后又在前进"), Stroller->GetDistanceAlongRoute() > FrozenAt);

		Stroller->Destroy();
		World->Tick(LEVELTICK_All, 0.01f);
	}

	// 被打飞：没有物理资产的网格也不能崩（临时网格就是这种），
	// 而且必须立刻从在场名单里摘掉——不摘的话躺在地上的人会占着人数上限，越打街越空
	ClearAll();
	ADeliveryPedestrian* Victim = Route->SpawnOnePedestrian(true);
	if (!TestNotNull(TEXT("spawn victim"), Victim))
		return false;
	TestEqual(TEXT("清场后只有这一个"), Route->GetAlivePedestrians().Num(), 1);
	TestFalse(TEXT("刚生成时没被打飞"), Victim->IsKnockedDown());
	Victim->ReceiveImpact(FVector(0.f, 0.f, 1000.f), Victim->GetActorLocation());
	TestTrue(TEXT("受击后进入被打飞状态"), Victim->IsKnockedDown());
	TestEqual(TEXT("被打飞阶段"), static_cast<int32>(Victim->GetPhase()),
	          static_cast<int32>(EDeliveryPedestrianPhase::KnockedDown));
	TestEqual(TEXT("立刻从在场名单摘掉"), Route->GetAlivePedestrians().Num(), 0);

	// 再被打一次只是多吃冲量，**不能重置退场计时**：
	// 在车流里被反复蹭的路人会永远不退场、一直占着人数上限
	Victim->Tick(1.0f);
	const float RemainingAfterOneSecond = Victim->GetKnockedRemaining();
	Victim->ReceiveImpact(FVector(0.f, 0.f, 1000.f), Victim->GetActorLocation());
	TestEqual(TEXT("再被打不重置退场计时"), Victim->GetKnockedRemaining(), RemainingAfterOneSecond,
	          0.01f);

	// 躺够之后缩掉并销毁
	for (int32 Step = 0; Step < 200 && !Victim->IsActorBeingDestroyed(); ++Step)
	{
		Victim->Tick(0.1f);
	}
	TestTrue(TEXT("躺够之后销毁"), Victim->IsActorBeingDestroyed());
	World->Tick(LEVELTICK_All, 0.01f);

	// 线段退化成一个点时不该生成，也不该崩
	Spline->ClearSplinePoints(false);
	Spline->AddSplinePoint(FVector::ZeroVector, ESplineCoordinateSpace::World, false);
	Spline->AddSplinePoint(FVector::ZeroVector, ESplineCoordinateSpace::World, false);
	Spline->UpdateSpline();
	TestNull(TEXT("零长度线段不生成"), Route->SpawnOnePedestrian(true));

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
