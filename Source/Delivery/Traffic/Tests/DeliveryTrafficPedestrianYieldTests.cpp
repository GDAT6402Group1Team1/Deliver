#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Traffic/DeliveryTrafficCarComponent.h"
#include "NPC/DeliveryPedestrian.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/ScopeExit.h"

/**
 * 交通车为行人让行
 *
 * 要盯住的是"主要那种情况"：绿灯了，但斑马线上还有人。所以这里**没有**路口段、
 * 没有红灯——全程绿灯，车该停下来纯粹是因为前方有人。
 *
 * 为什么必须有自动化：这套逻辑的失败模式是**静默**的。按通道查而不是按物体类型查
 * 会让扫描恒为空（本仓库栽过三次），而表现是"车照常开过去"——和"没写这个功能"
 * 一模一样，在 PIE 里分不出来。
 *
 * 不替代实机验收：真实斑马线上的人是走动的，停车位置好不好看、后面那队车排得齐不齐
 * 只能在关卡里看。
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryTrafficPedestrianYieldTest,
                                 "Delivery.Traffic.PedestrianYield",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FDeliveryTrafficPedestrianYieldTest::RunTest(const FString&)
{
	const auto Settings = UWorld::InitializationValues()
	                          .AllowAudioPlayback(false)
	                          .CreatePhysicsScene(true)
	                          .ShouldSimulatePhysics(false)
	                          .CreateNavigation(false)
	                          .CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
	                                    ERHIFeatureLevel::Num, &Settings);
	if (!TestNotNull(TEXT("测试世界"), World))
		return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->EndPlay(EEndPlayReason::Quit);
		World->DestroyWorld(false);
	};

	// 车身要有真的碰撞盒，否则后车那段的前车扫描（按 ECC_Vehicle 查）什么都扫不到，
	// "后车也不强行通过"这条断言会因为"后车根本没被挡住"而假通过
	auto MakeCar = [World](const FVector& Where) -> UDeliveryTrafficCarComponent*
	{
		AActor* Actor = World->SpawnActor<AActor>();
		if (!Actor)
		{
			return nullptr;
		}
		UBoxComponent* Box = NewObject<UBoxComponent>(Actor);
		Actor->SetRootComponent(Box);
		Box->SetBoxExtent(FVector(130.f, 68.f, 70.f));
		Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Box->SetCollisionObjectType(ECC_Vehicle);
		Box->SetCollisionResponseToAllChannels(ECR_Ignore);
		Box->SetCollisionResponseToChannel(ECC_Vehicle, ECR_Block);
		Box->RegisterComponent();
		Actor->SetActorLocation(Where);
		Actor->SetActorRotation(FRotator::ZeroRotator);  // 车头朝 +X

		UDeliveryTrafficCarComponent* Comp = NewObject<UDeliveryTrafficCarComponent>(Actor);
		Comp->bScaleInOnSpawn = false;
		Comp->bScaleOutOnRouteLost = false;
		Comp->bSpawnExtraWaves = false;
		Comp->SpeedMultiplierChangeRate = 100.f;  // 系数一帧到位，断言看目标值不看过程
		Comp->FollowAccelerationPause = 0.f;
		Comp->RegisterComponent();
		Comp->SetComponentTickEnabled(false);
		return Comp;
	};

	UDeliveryTrafficCarComponent* Traffic = MakeCar(FVector::ZeroVector);
	if (!TestNotNull(TEXT("车"), Traffic))
		return false;
	AActor* Car = Traffic->GetOwner();

	World->InitializeActorsForPlay(FURL());
	World->GetWorldSettings()->NotifyBeginPlay();

	// TickComponent 开头就按这个早退，为 false 的话后面全部断言都会"通过得莫名其妙"
	if (!TestTrue(TEXT("测试世界里的车有权威"), Car->HasAuthority()))
		return false;

	const float ScanStart = Traffic->TraceStartForwardOffset;

	TestEqual(TEXT("前方没人时不返回目标"), Traffic->GetDistanceToPedestrianAhead(), -1.0f);

	ADeliveryPedestrian* Walker = World->SpawnActor<ADeliveryPedestrian>();
	if (!TestNotNull(TEXT("行人"), Walker))
		return false;
	// BeginPlay 把人缩到看不见（进场动画的起点），胶囊跟着缩成几乎没有体积，
	// 不还原尺寸的话下面每一次扫描都扫不到——断言会全绿但什么都没验到
	Walker->SetActorScale3D(FVector::OneVector);

	// 超出探测距离：探测长度是固定的，不跟车速走，所以这条边界是确定的
	Walker->SetActorLocation(FVector(ScanStart + Traffic->PedestrianScanDistance + 300.0f, 0, 0));
	TestEqual(TEXT("探测距离之外的人不管"), Traffic->GetDistanceToPedestrianAhead(), -1.0f);

	// 身后的人不管：让行只看前方，否则车会被刚走过去的人一直拦着
	Walker->SetActorLocation(FVector(-400, 0, 0));
	TestEqual(TEXT("身后的人不管"), Traffic->GetDistanceToPedestrianAhead(), -1.0f);

	// 横向偏开探测半径的人不管——这就是"算不算在我这条车道上"的判据，
	// 给大了会替隔壁车道和人行道上的人停车
	Walker->SetActorLocation(FVector(500, Traffic->PedestrianScanRadius * 3.0f, 0));
	TestEqual(TEXT("隔壁车道的人不管"), Traffic->GetDistanceToPedestrianAhead(), -1.0f);

	// 远处正前方：探到了，但还没到该停住的距离，应该只是减速
	Walker->SetActorLocation(FVector(ScanStart + 800.0f, 0, 0));
	const float FarDistance = Traffic->GetDistanceToPedestrianAhead();
	TestTrue(TEXT("正前方远处的人探到了"), FarDistance > Traffic->PedestrianStopDistance);

	Traffic->TickComponent(0.1f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("远处有人时减速但不停"),
	         Traffic->GetSpeedMultiplier() > 0.01f && Traffic->GetSpeedMultiplier() < 0.99f);
	TestFalse(TEXT("远处有人还不算在让行"), Traffic->IsYieldingToPedestrian());

	// 走到停车距离以内：该彻底停住
	Walker->SetActorLocation(FVector(ScanStart + 150.0f, 0, 0));
	const float NearDistance = Traffic->GetDistanceToPedestrianAhead();
	TestTrue(TEXT("近处的人在停车距离以内"),
	         NearDistance >= 0.0f && NearDistance < Traffic->PedestrianStopDistance);

	Traffic->TickComponent(0.1f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("人就在前面时完全停住"), Traffic->GetSpeedMultiplier(), 0.0f, 0.001f);
	TestTrue(TEXT("正在为行人让行"), Traffic->IsYieldingToPedestrian());
	// 让行要沿车队传递，否则队尾会把前面这辆当成死锁压上来
	TestTrue(TEXT("让行期间按合法等待对待"), Traffic->IsStoppedAtIntersection());

	// **等人不设强行通过的超时。** 车队死锁那条兜底是为了解两车互为前车，
	// 等行人不是死锁，是在等一件一定会结束的事；而行人侧的规则是人不让车，
	// 所以"互相礼让"在规则上不存在。等到远超 MaxBlockedTime 也必须还停着。
	const float LongWait = Traffic->MaxBlockedTimeAtIntersection + Traffic->StalledTimeout + 5.0f;
	for (int32 Step = 0; Step < FMath::CeilToInt(LongWait / 0.1f); ++Step)
	{
		Traffic->TickComponent(0.1f, LEVELTICK_All, nullptr);
	}
	TestTrue(TEXT("等再久也不强行通过"), Traffic->IsYieldingToPedestrian());
	TestEqual(TEXT("等再久速度系数也是 0"), Traffic->GetSpeedMultiplier(), 0.0f, 0.001f);
	// 等人期间不许累计停滞计时：否则车会被停滞检测瞬移回出生点，
	// 表现是"车等人等到自己消失了"，比强行通过还难查
	TestEqual(TEXT("等人期间不累计停滞计时"), Traffic->StalledElapsedTime, 0.0f);
	TestFalse(TEXT("等人期间没有被判定要回出生点"), Traffic->bRouteLostPending);

	// 后车：自己看不见那个人（关掉让行开关），纯粹被前车挡住。
	// 前车在等人时它也不许强行通过，否则兜底会从队尾一辆辆把车推进斑马线
	UDeliveryTrafficCarComponent* Follower = MakeCar(FVector(-600, 0, 0));
	if (TestNotNull(TEXT("后车"), Follower))
	{
		Follower->bYieldToPedestrians = false;
		auto TickQueue = [&](float Seconds)
		{
			for (int32 Step = 0; Step < FMath::CeilToInt(Seconds / 0.1f); ++Step)
			{
				// 先前车后后车：后车要读到本帧的前车状态
				Traffic->TickComponent(0.1f, LEVELTICK_All, nullptr);
				Follower->TickComponent(0.1f, LEVELTICK_All, nullptr);
			}
		};
		TickQueue(0.2f);
		TestTrue(TEXT("后车确实被前车挡住了"),
		         Follower->DebugDistanceAhead >= 0.0f &&
		             Follower->DebugDistanceAhead < Follower->MinFollowDistance);
		TestFalse(TEXT("后车自己没在让行人"), Follower->IsYieldingToPedestrian());
		TestTrue(TEXT("后车知道前车在等行人"), Follower->bLeaderYieldingToPedestrian);

		TickQueue(Traffic->MaxBlockedTimeAtIntersection + 5.0f);
		TestEqual(TEXT("前车在等人时后车也不强行通过"), Follower->GetSpeedMultiplier(), 0.0f, 0.001f);

		// 对照组：人走开、前车不再让行之后，死锁兜底必须还能正常放行——
		// 否则上一条断言只是证明我把兜底整个弄坏了
		Walker->SetActorLocation(FVector(-5000, 0, 0));
		TickQueue(0.2f);
		TestFalse(TEXT("人走开后前车不再让行"), Traffic->IsYieldingToPedestrian());
		TestFalse(TEXT("人走开后后车也不再认为前车在等人"),
		          Follower->bLeaderYieldingToPedestrian);
		TickQueue(Traffic->MaxBlockedTimeAtIntersection + 5.0f);
		TestTrue(TEXT("不涉及行人时死锁兜底照旧放行"), Follower->GetSpeedMultiplier() > 0.99f);

		Follower->GetOwner()->Destroy();
	}

	// 人回到车前面：前车重新让行
	Walker->SetActorLocation(FVector(ScanStart + 150.0f, 0, 0));
	Traffic->TickComponent(0.1f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("人再回到前面时重新让行"), Traffic->IsYieldingToPedestrian());

	// 倒地的人不拦车：他自己走不开，拦着只会把整条队堵到超时
	Walker->ReceiveImpact(FVector(0, 0, 1), Walker->GetActorLocation());
	TestTrue(TEXT("行人已倒地"), Walker->IsKnockedDown());
	TestEqual(TEXT("倒地的人不拦车"), Traffic->GetDistanceToPedestrianAhead(), -1.0f);

	// 关掉开关要完全回到原来的行为
	Walker->SetActorLocation(FVector(ScanStart + 150.0f, 0, 0));
	Traffic->bYieldToPedestrians = false;
	TestEqual(TEXT("关掉开关后不再探测行人"), Traffic->GetDistanceToPedestrianAhead(), -1.0f);

	return true;
}
#endif
