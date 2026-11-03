#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Camera/DeliveryCameraComponent.h"
#include "Camera/DeliveryCameraConfig.h"
#include "Camera/CameraComponent.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/WorldSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryCameraImpactTest, "Delivery.Camera.Impact",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryCameraImpactTest::RunTest(const FString&)
{
    const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
        ERHIFeatureLevel::Num, &Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    APawn* Pawn = World->SpawnActor<APawn>();
    auto* Boom = NewObject<USpringArmComponent>(Pawn);
    Pawn->SetRootComponent(Boom);
    Boom->RegisterComponent();
    auto* Camera = NewObject<UCameraComponent>(Pawn);
    Camera->SetupAttachment(Boom, USpringArmComponent::SocketName);
    Camera->RegisterComponent();
    auto* Component = NewObject<UDeliveryCameraComponent>(Pawn);
    Component->RegisterComponent();
    auto* Config = NewObject<UDeliveryCameraConfig>(Component);
    Config->Distance = 500.0f;
    Config->FieldOfView = 85.0f;
    Component->Config = Config;
    auto* Controller = World->SpawnActor<APlayerController>();
    Controller->SetPlayer(NewObject<ULocalPlayer>(GEngine));
    Controller->Possess(Pawn);
    TestTrue(TEXT("测试角色由本机控制"), Pawn->IsLocallyControlled());
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();

    TestEqual(TEXT("配置应用到弹簧臂"), Boom->TargetArmLength, 500.0f);
    TestEqual(TEXT("配置应用到相机"), Camera->FieldOfView, 85.0f);
    TestFalse(TEXT("空闲时不更新"), Component->IsComponentTickEnabled());
    Component->PlayImpact();
    Component->TickComponent(.1f, LEVELTICK_All, nullptr);
    TestTrue(TEXT("撞击后镜头拉远"), Boom->TargetArmLength > 500.0f);

    // 连续撞击不能不断累加距离，也不能修改共用配置
    Component->PlayImpact();
    TestEqual(TEXT("连续撞击保留最初距离"), Component->BaseDistance, 500.0f);
    TestEqual(TEXT("配置保持不变"), Config->Distance, 500.0f);
    Component->HoldUntil = -1.0f;
    for (int32 Step = 0; Step < 100; ++Step)
    {
        Component->TickComponent(.1f, LEVELTICK_All, nullptr);
    }
    TestEqual(TEXT("结束后恢复原距离"), Boom->TargetArmLength, 500.0f);
    TestFalse(TEXT("结束后关闭更新"), Component->IsComponentTickEnabled());

    // 失去控制时立即清理，未被本机控制的角色不能播放反馈
    Component->PlayImpact();
    Component->TickComponent(.1f, LEVELTICK_All, nullptr);
    Controller->UnPossess();
    Component->TickComponent(.1f, LEVELTICK_All, nullptr);
    TestEqual(TEXT("失去控制后恢复距离"), Boom->TargetArmLength, 500.0f);
    Component->PlayImpact();
    TestFalse(TEXT("非本机角色不启动反馈"), Component->IsComponentTickEnabled());
    Component->Config = nullptr;
    Boom->TargetArmLength = 650.0f;
    Component->ApplyConfig();
    TestEqual(TEXT("未配置时保留蓝图设置"), Boom->TargetArmLength, 650.0f);

    World->DestroyWorld(false);
    GEngine->DestroyWorldContext(World);
    return true;
}
#endif
