#if WITH_DEV_AUTOMATION_TESTS
#include "Vehicle/DeliveryMotorbike.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryMotorbikeObstacleTest,
                                 "Delivery.Vehicle.ObstacleEscape",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FDeliveryMotorbikeObstacleTest::RunTest(const FString&)
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
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->EndPlay(EEndPlayReason::Quit);
		World->DestroyWorld(false);
	};

	AActor* Post = World->SpawnActor<AActor>();
	UBoxComponent* PostBody = NewObject<UBoxComponent>(Post);
	Post->SetRootComponent(PostBody);
	PostBody->SetBoxExtent(FVector(30.f, 30.f, 120.f));
	PostBody->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	PostBody->SetCollisionObjectType(ECC_WorldStatic);
	PostBody->SetCollisionResponseToAllChannels(ECR_Block);
	PostBody->RegisterComponent();
	Post->SetActorLocation(FVector(170.f, 0.f, 100.f));

	ADeliveryMotorbike* Bike = World->SpawnActor<ADeliveryMotorbike>();
	if (!TestNotNull(TEXT("摩托车"), Bike))
	{
		return false;
	}
	Bike->MaxStepHeight = 0.f;
	const float HalfLength =
	    Bike->CollisionBox ? Bike->CollisionBox->GetScaledBoxExtent().X : 95.f;
	// 车头刚好顶到柱子的位置：再往前一点都算穿进去了
	const float Contact = 170.f - 30.f - HalfLength;

	for (const ECollisionChannel ObstacleType : {ECC_WorldStatic, ECC_WorldDynamic})
	{
		// 地形类和花坛、电线杆等独立网格使用不同对象类型，都应能脱困
		PostBody->SetCollisionObjectType(ObstacleType);
		Bike->SetActorLocationAndRotation(FVector(0.f, 0.f, 100.f), FRotator::ZeroRotator,
		                                  false, nullptr, ETeleportType::TeleportPhysics);
		Bike->CurrentSpeed = 500.f;
		Bike->SteerInput = 0.f;
		Bike->UpdateGroundAndMove(0.1f);

		// 柱子仍然挡住前进——绕行只许侧移，不许从柱子里穿过去
		TestTrue(TEXT("障碍仍会挡住摩托车"), Bike->GetActorLocation().X < Contact + 5.f);
		// **但正面顶住细柱子不该判成顶住。** 沿表面切向滑那一步对细柱子恒等于零
		// （法线正对车头、投影为零），所以必须靠侧向绕行；这条断言盯的就是绕行真的发生了
		TestTrue(TEXT("正面撞细柱子时往侧面绕开"),
		         FMath::Abs(Bike->GetActorLocation().Y) > 1.f);
		TestFalse(TEXT("绕开了就不算顶住"), Bike->bBlockedByWall);
		// 绕开不该白掉速度：撞一下就归零的话，玩家每次擦到路灯都要重新加速
		TestTrue(TEXT("绕开时保住了车速"), Bike->CurrentSpeed > 400.f);

		// 关掉绕行开关要回到原来的行为：顶住、掉速、靠转向脱困
		Bike->bDeflectAroundNarrowObstacles = false;
		Bike->SetActorLocationAndRotation(FVector(0.f, 0.f, 100.f), FRotator::ZeroRotator,
		                                  false, nullptr, ETeleportType::TeleportPhysics);
		Bike->CurrentSpeed = 500.f;
		Bike->UpdateGroundAndMove(0.1f);
		TestTrue(TEXT("关掉绕行后正面顶住"), Bike->bBlockedByWall);
		TestTrue(TEXT("顶住那一帧扣一次速度"), Bike->CurrentSpeed < 500.f * 0.9f);

		// **顶着不许每帧继续衰减。** 原来每帧乘 WallHitSpeedScale，60 fps 下十来帧
		// 就把车速压到千分之一，玩家松开障碍还要从 0 重新加速——这就是"卡住很久"
		// **不要在循环里给油门。** 每帧把车速顶回去的话，旧实现（每帧乘 0.5）
		// 也会停在"油门值的一半"上，这条断言就永远是绿的、什么都没验到
		const float AfterFirstHit = Bike->CurrentSpeed;
		for (int32 Frame = 0; Frame < 30; ++Frame)
		{
			// **每帧要把高度按回去。** 这个测试世界里除了柱子没有地面，
			// UpdateGroundAndMove 的重力会让车一路往下掉；掉到柱子下面之后
			// 就不再顶着东西了，blocked 变 false、转向也恢复成按真实车速算，
			// 看着像"代码没生效"，其实是测试场景没有地板（实测 30 帧掉了 548 cm）
			FVector Held = Bike->GetActorLocation();
			Held.Z = 100.f;
			Bike->SetActorLocation(Held, false, nullptr, ETeleportType::TeleportPhysics);
			Bike->UpdateGroundAndMove(1.f / 60.f);
		}
		// 顶着的这 30 帧里速度不许再掉：旧实现 0.5^30 ≈ 2e-7，等于归零，
		// 玩家松开障碍后还得从 0 重新加速——这就是"撞到路灯卡很久"
		TestEqual(TEXT("一直顶着也不再继续掉速"), Bike->CurrentSpeed, AfterFirstHit, 0.01f);
		TestTrue(TEXT("顶住期间保住余速"), Bike->CurrentSpeed > 1.f);

		// 低速顶住时仍要能明显打把，否则转不开、出不来
		TestTrue(TEXT("顶了 30 帧之后仍然判为顶住"), Bike->bBlockedByWall);
		Bike->CurrentSpeed = 20.f;
		Bike->SteerInput = 1.f;
		const float YawBefore = Bike->GetActorRotation().Yaw;
		Bike->UpdateSteering(0.1f);
		TestTrue(TEXT("低速受阻时仍能明显转向"),
		         Bike->GetActorRotation().Yaw - YawBefore > 4.f);

		Bike->bDeflectAroundNarrowObstacles = true;
		Bike->SteerInput = 0.f;
	}

	// 对照组：一整面墙两侧都没有空档，必须还是顶住，不能被当成柱子绕过去
	PostBody->SetCollisionObjectType(ECC_WorldStatic);
	PostBody->SetBoxExtent(FVector(30.f, 2000.f, 120.f));
	Bike->SetActorLocationAndRotation(FVector(0.f, 0.f, 100.f), FRotator::ZeroRotator,
	                                  false, nullptr, ETeleportType::TeleportPhysics);
	Bike->CurrentSpeed = 500.f;
	Bike->UpdateGroundAndMove(0.1f);
	TestTrue(TEXT("撞一整面墙仍判为顶住"), Bike->bBlockedByWall);
	TestTrue(TEXT("撞墙不会侧向滑走"), FMath::Abs(Bike->GetActorLocation().Y) < 5.f);

	return true;
}
#endif
