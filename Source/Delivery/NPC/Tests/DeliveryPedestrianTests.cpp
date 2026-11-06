// 验证人行路线的生成上限、沿线段推进、出入场缩放与到端点回收
#include "NPC/DeliveryPedestrianRoute.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Components/BoxComponent.h"
#include "Components/PoseableMeshComponent.h"
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
	// 本段保留长样条与接驳的回归检查；普通街道路人默认走局部游走模式。
	Route->bRoamInArea = false;
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
	// 同一张临时网格没有物理资产；受击时仍应播放程序化跌倒与爬起。

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
		RouteB->bRoamInArea = false;
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
		// 逼它马上停：走 1 cm 就触发，停 1 秒。
		// **注意这里改的是"下一次"的间隔，改不掉已经抽好的那一次**：
		// BeginWalk 在生成时就调过一次 ScheduleNextPause，用的是默认区间
		// （700~2600 cm），所以第一次停顿仍然要走完那个随机距离。
		// 下面那个循环的预算必须盖得住 PauseIntervalMax，否则抽到大值就失败——
		// 实测这个测试原来的预算只有 2000 cm，抽到 >2250 的约 18% 会偶发红，
		// 而症状（"走出窗口之后会停" 为假）看着像让路逻辑把人卡住了，极易误判
		Stroller->PauseIntervalMin = 1.f;
		Stroller->PauseIntervalMax = 1.f;
		Stroller->PauseDurationMin = 1.f;
		Stroller->PauseDurationMax = 1.f;
		// 这里只测随机停顿，让路有单独的断言；隔离世界里残留碰撞不该影响它。
		Stroller->bYieldToObstacles = false;

		// 先走出出入场窗口（窗口里不许停，这是刻意的——
		// 在里面停住会卡成一个半大小的人站着不动）
		while (Stroller->GetDistanceAlongRoute() < Route->AppearDistance + 50.f &&
		       !Stroller->IsActorBeingDestroyed())
		{
			Stroller->Tick(0.05f);
		}
		TestFalse(TEXT("出入场窗口里不停"), Stroller->IsPaused());

		// 每帧走 10 cm，700 帧 = 7000 cm，盖得住 PauseIntervalMax 的 2600 和
		// 出入场窗口那一次 200 的重置，不依赖随机抽到多少
		for (int32 Step = 0; Step < 700 && !Stroller->IsPaused(); ++Step)
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

	// 让路：前方有别的路人就停下来等，但**不能永久互相礼让**。
	// 两个人面对面是最容易死锁的情形，交通车那边栽过同一个坑
	ClearAll();
	{
		// **必须先把两个人弄出出入场窗口、并恢复原尺寸**，否则这一段测的是空气：
		//   ① 窗口里刻意不让路（那两个窗口按距离算缩放，在里面停住会卡成半个人）；
		//   ② 窗口起点的缩放只有 MinVisibleScale（2%），胶囊只有半厘米高，
		//      探测球扫不到——而那和"让路逻辑没生效"长得一模一样。
		// 第一版就是直接在端点上摆两个人，三条断言全红。
		const float SavedAppearDistance = Route->AppearDistance;
		Route->AppearDistance = 1.f;

		ADeliveryPedestrian* Front = Route->SpawnOnePedestrian(true);
		ADeliveryPedestrian* Back = Route->SpawnOnePedestrian(true);
		if (TestNotNull(TEXT("让路：前车"), Front) && TestNotNull(TEXT("让路：后车"), Back))
		{
			Front->PauseDurationMax = 0.f;
			Front->PauseDurationMin = 0.f;
			Back->PauseDurationMax = 0.f;
			Back->PauseDurationMin = 0.f;
			// 布置期间先关掉让路：两个人这会儿是重叠在同一个端点上的，
			// 开着的话他们会当场互相礼让、谁都走不出窗口
			Front->bYieldToObstacles = false;
			Back->bYieldToObstacles = false;
			for (int32 Step = 0; Step < 5; ++Step)
			{
				Front->Tick(0.1f);
				Back->Tick(0.1f);
			}
			TestTrue(TEXT("让路测试前已走出窗口、恢复原尺寸"),
			         Back->GetAppearAlpha() > 0.95f && Front->GetAppearAlpha() > 0.95f);

			Back->bYieldToObstacles = true;
			// 挡在正前方：探测距离 95，放到 60 cm 处一定命中
			Front->SetActorLocation(Back->GetActorLocation() +
			                        Back->GetActorForwardVector() * 60.f);
			TestTrue(TEXT("前方有人时判定为被挡住"), Back->IsBlockedAhead());

			// 让够 MaxYieldSeconds 之前不该前进
			Back->MaxYieldSeconds = 1.f;
			const float BeforeYield = Back->GetDistanceAlongRoute();
			Back->Tick(0.1f);
			TestEqual(TEXT("被挡住时停下来等"), Back->GetDistanceAlongRoute(), BeforeYield, 0.01f);
			TestTrue(TEXT("正在让路"), Back->IsYielding());

			// 让超时之后必须硬挤过去，否则两个人会贴在一起永久不动
			for (int32 Step = 0; Step < 20; ++Step)
			{
				Back->Tick(0.1f);
				// Back 每帧都会被 UpdateTransform 拉回样条上，所以要跟着把
				// 挡路的人重新摆到它正前方，不然"挡住"这件事自己就消失了
				Front->SetActorLocation(Back->GetActorLocation() +
				                        Back->GetActorForwardVector() * 60.f);
			}
			TestTrue(TEXT("让超时后硬挤过去"), Back->GetDistanceAlongRoute() > BeforeYield + 1.f);

			// 挡路的人躺下之后不用再让：绕不过去，而且他过几秒自己就起来了
			Front->ReceiveImpact(FVector(0.f, 0.f, 1000.f), Front->GetActorLocation());
			Front->SetActorLocation(Back->GetActorLocation() +
			                        Back->GetActorForwardVector() * 60.f);
			TestFalse(TEXT("躺着的人不用让"), Back->IsBlockedAhead());
		}

		ClearAll();
		ADeliveryPedestrian* StaticWalker = Route->SpawnOnePedestrian(true);
		if (TestNotNull(TEXT("固定障碍：路人"), StaticWalker))
		{
			StaticWalker->PauseDurationMin = 0.f;
			StaticWalker->PauseDurationMax = 0.f;
			StaticWalker->MaxYieldSeconds = 0.1f;
			for (int32 Step = 0; Step < 5; ++Step)
			{
				StaticWalker->Tick(0.1f);
			}
			AActor* Wall = World->SpawnActor<AActor>();
			UBoxComponent* Box = NewObject<UBoxComponent>(Wall);
			Wall->SetRootComponent(Box);
			Box->SetBoxExtent(FVector(20.f, 40.f, 80.f));
			Box->SetCollisionObjectType(ECC_WorldStatic);
			Box->SetCollisionResponseToAllChannels(ECR_Block);
			Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Box->RegisterComponent();
			Wall->SetActorLocation(StaticWalker->GetActorLocation() +
			                       StaticWalker->GetActorForwardVector() * 60.f + FVector(0.f, 0.f, 70.f));

			bool bFixed = false;
			TestTrue(TEXT("房屋类障碍挡住前方"), StaticWalker->IsBlockedAhead(&bFixed));
			TestTrue(TEXT("房屋类障碍被识别为固定障碍"), bFixed);
			const float BeforeWall = StaticWalker->GetDistanceAlongRoute();
			for (int32 Step = 0; Step < 20; ++Step)
			{
				StaticWalker->Tick(0.1f);
			}
			TestEqual(TEXT("固定障碍超过让路时间也不穿过"), StaticWalker->GetDistanceAlongRoute(),
			          BeforeWall, 0.01f);
			Wall->Destroy();
			StaticWalker->Tick(0.1f);
			TestTrue(TEXT("障碍移开后继续走"), StaticWalker->GetDistanceAlongRoute() > BeforeWall);
		}

		Route->AppearDistance = SavedAppearDistance;
	}

	// 被打倒：没有物理资产的网格也必须保留并爬起。
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
	// 倒地期间不从在场名单摘掉，避免路线补出新人。
	TestEqual(TEXT("会起身时不摘出在场名单"), Route->GetAlivePedestrians().Num(), 1);

	// 再被打一次只是多吃冲量，**不能重置躺着的计时**：
	// 在车流里被反复蹭的路人会一直躺着起不来
	Victim->Tick(1.0f);
	TestTrue(TEXT("跌倒动画已完成"), Victim->GetFallAlpha() > 0.99f);
	TestTrue(TEXT("受击后仍是正常尺寸"), Victim->GetAppearAlpha() > 0.95f);
	TestTrue(TEXT("身体已经倾倒"), FVector::DotProduct(Victim->GetActorUpVector(), FVector::UpVector) < 0.5f);
	const float RemainingAfterOneSecond = Victim->GetKnockedRemaining();
	Victim->ReceiveImpact(FVector(0.f, 0.f, 1000.f), Victim->GetActorLocation());
	TestEqual(TEXT("再被打不重置躺着的计时"), Victim->GetKnockedRemaining(), RemainingAfterOneSecond,
	          0.01f);

	// 被打飞时人会被推离样条，起身要能回到线上。
	// 挪到横向 600 cm 外，验证起身结束时横向偏移被收掉
	Victim->SetActorLocation(Victim->GetActorLocation() + FVector(0.f, 600.f, 0.f));

	// 躺够之后进入起身，**不是销毁**
	for (int32 Step = 0; Step < 200 && !Victim->IsStandingUp(); ++Step)
	{
		Victim->Tick(0.1f);
	}
	TestTrue(TEXT("躺够之后进入起身而不是消失"), Victim->IsStandingUp());
	TestFalse(TEXT("起身时已不算被打飞"), Victim->IsKnockedDown());
	TestFalse(TEXT("起身期间不销毁"), Victim->IsActorBeingDestroyed());
	TestEqual(TEXT("起身阶段"), static_cast<int32>(Victim->GetPhase()),
	          static_cast<int32>(EDeliveryPedestrianPhase::StandingUp));
	// 起身一开始身体还是躺着的朝向，所以不能一上来就是直立
	TestTrue(TEXT("起身进度从零开始"), Victim->GetGetUpAlpha() < 0.2f);

	// 推进到起身结束：必须回到 Walking，并且立起来、回到样条上
	for (int32 Step = 0; Step < 400 && Victim->IsStandingUp(); ++Step)
	{
		Victim->Tick(0.02f);
	}
	TestEqual(TEXT("起身完回到行走"), static_cast<int32>(Victim->GetPhase()),
	          static_cast<int32>(EDeliveryPedestrianPhase::Walking));
	TestFalse(TEXT("起身完没被销毁"), Victim->IsActorBeingDestroyed());
	// 站直：俯仰和翻滚都该收掉。躺着那会儿是侧倒/趴着的朝向，
	// 不收掉的表现是人立起来了但整个身体是歪的
	const FRotator Upright = Victim->GetActorRotation();
	TestTrue(TEXT("起身完站直（俯仰）"), FMath::Abs(Upright.Pitch) < 2.f);
	TestTrue(TEXT("起身完站直（翻滚）"), FMath::Abs(Upright.Roll) < 2.f);
	// 回到线上：这条直线样条在 Y=0 上，所以横向偏移应该已经收掉
	TestTrue(TEXT("起身完回到样条上"), FMath::Abs(Victim->GetActorLocation().Y) < 50.f);
	// 缩放要回到原尺寸：被打飞那条路没碰过 AppearAlpha，但不起身那条路会把它缩下去，
	// 两条路共用同一组字段，漏了归位的表现是起身之后人只有半个大小
	TestTrue(TEXT("起身完恢复原尺寸"), Victim->GetAppearAlpha() > 0.95f);

	// 起身之后还能再被打倒一次：挨打的胶囊必须还回来了，
	// 不还的表现是站起来的人再也打不中（而那和"命中窗口没对上"长得一模一样）
	Victim->ReceiveImpact(FVector(0.f, 0.f, 1000.f), Victim->GetActorLocation());
	TestTrue(TEXT("起身后还能再被打倒"), Victim->IsKnockedDown());

	// 第二次受击也会再次起身，不得在地上缩小消失。
	for (int32 Step = 0; Step < 200 && !Victim->IsStandingUp(); ++Step)
	{
		Victim->Tick(0.1f);
	}
	TestTrue(TEXT("第二次受击后仍会起身"), Victim->IsStandingUp());
	TestFalse(TEXT("反复受击不销毁"), Victim->IsActorBeingDestroyed());
	World->Tick(LEVELTICK_All, 0.01f);

	// 用真正的 NPC 模型检查显示槽位：旧测试只用没有骨骼的临时网格，
	// 无法发现受击时把唯一可见的身体隐藏掉。
	ClearAll();
	USkeletalMesh* RealMesh = LoadObject<USkeletalMesh>(nullptr,
	    TEXT("/Game/Characters/NPC/SK_NpcMan.SK_NpcMan"));
	if (!TestNotNull(TEXT("实际路人模型"), RealMesh))
		return false;
	Route->Kinds[0].Mesh = RealMesh;
	ADeliveryPedestrian* VisibleVictim = Route->SpawnOnePedestrian(true);
	if (!TestNotNull(TEXT("实际路人生成"), VisibleVictim))
		return false;
	UPoseableMeshComponent* VisibleBody = VisibleVictim->FindComponentByClass<UPoseableMeshComponent>();
	if (!TestNotNull(TEXT("实际路人可见身体"), VisibleBody))
		return false;
	TestTrue(TEXT("行走时保留可见模型"), VisibleBody->IsVisible());
	VisibleVictim->ReceiveImpact(FVector(1200.f, 0.f, 200.f), VisibleVictim->GetActorLocation());
	VisibleVictim->Tick(1.f);
	TestTrue(TEXT("跌倒后模型仍可见"), VisibleBody->IsVisible());
	TestTrue(TEXT("跌倒后模型仍有网格"), VisibleBody->GetSkinnedAsset() == RealMesh);
	TestFalse(TEXT("跌倒后不销毁实际路人"), VisibleVictim->IsActorBeingDestroyed());
	ClearAll();
	Route->Kinds[0].Mesh = DummyMesh;

	// 线段退化成一个点时不该生成，也不该崩
	Spline->ClearSplinePoints(false);
	Spline->AddSplinePoint(FVector::ZeroVector, ESplineCoordinateSpace::World, false);
	Spline->AddSplinePoint(FVector::ZeroVector, ESplineCoordinateSpace::World, false);
	Spline->UpdateSpline();
	TestNull(TEXT("零长度线段不生成"), Route->SpawnOnePedestrian(true));

	// 普通路人只在各自的小圆内走走停停，不能接驳到另一条长路线。
	ClearAll();
	Route->bRoamInArea = true;
	Route->RoamRadius = 300.f;
	Route->RoamPauseDistanceMin = 100.f;
	Route->RoamPauseDistanceMax = 150.f;
	ADeliveryPedestrian* Roamer = Route->SpawnOnePedestrian(true);
	if (!TestNotNull(TEXT("游走：零长度样条也能生成"), Roamer))
		return false;
	Roamer->bYieldToObstacles = false;
	bool bSawPause = false;
	float Furthest = 0.f;
	for (int32 Step = 0; Step < 500; ++Step)
	{
		Roamer->Tick(0.05f);
		bSawPause |= Roamer->IsPaused();
		Furthest = FMath::Max(Furthest,
		                      FVector::Dist2D(Roamer->GetActorLocation(), Route->GetActorLocation()));
	}
	TestTrue(TEXT("游走：确实走了一段"), Roamer->GetDistanceAlongRoute() > 300.f);
	TestTrue(TEXT("游走：中途停过"), bSawPause);
	TestTrue(TEXT("游走：始终在本点半径内"), Furthest <= Route->RoamRadius + 1.f);
	TestEqual(TEXT("游走：不接驳别的路线"), Roamer->GetHandoffCount(), 0);
	TestFalse(TEXT("游走：走久了也不在端点消失"), Roamer->IsActorBeingDestroyed());

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
