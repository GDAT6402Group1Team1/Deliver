#include "Input/DeliveryInputComponent.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "EnhancedInputComponent.h"
#include "UObject/UnrealType.h"
#include "Character/DeliveryCharacter.h"
#include "Player/DeliveryPlayerController.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "UI/DeliveryHUD.h"
#include "Game/DeliveryGameMode.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryInputLifecycleTest, "Delivery.Systems.InputLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryInputLifecycleTest::RunTest(const FString&)
{
    const auto Settings = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(true).ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
        ERHIFeatureLevel::Num, &Settings);
    if (!TestNotNull(TEXT("World"), World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); };
    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ADeliveryCharacter* Character = World->SpawnActor<ADeliveryCharacter>(Spawn);
    ADeliveryPlayerController* Player = World->SpawnActor<ADeliveryPlayerController>(Spawn);
    ADeliveryMotorbike* Bike = World->SpawnActor<ADeliveryMotorbike>(Spawn);
    if (!Character || !Player || !Bike) return false;
    Player->Possess(Character);
    UDeliveryInputComponent* Input = Character->GetPlayerInput();
    UEnhancedInputComponent* Bindings = NewObject<UEnhancedInputComponent>(Character);
    Input->Bind(Bindings);
    const int32 BindingCount = Bindings->GetActionEventBindings().Num();
    TestTrue(TEXT("Gameplay actions bound"), BindingCount > 0);
    Input->Bind(Bindings);
    TestEqual(TEXT("Repeated setup does not duplicate bindings"), Bindings->GetActionEventBindings().Num(), BindingCount);

    // 角色失去控制权时，尚未判定的单键不能在车辆上继续触发
    Input->MousePressed(true);
    TestTrue(TEXT("Chord wait started"), World->GetTimerManager().IsTimerActive(Input->MouseChordTimer));
    Player->Possess(Bike);
    TestFalse(TEXT("Possession clears chord timer"), World->GetTimerManager().IsTimerActive(Input->MouseChordTimer));
    TestFalse(TEXT("Possession clears held buttons"), Input->bLeftMouseDown || Input->bRightMouseDown);
    Input->MouseReleased(true);
    TestFalse(TEXT("Release after reset does not resolve an attack"), Input->bSingleMouseResolved);
    Player->Possess(Character);
    Input->MousePressed(true);
    Input->MousePressed(false);
    TestTrue(TEXT("Chord works after returning to character"), Character->IsGrabChordHeld());
    Input->ResetInput();
    TestFalse(TEXT("Reset releases chord"), Character->IsGrabChordHeld());

    // HUD 在步行和驾驶时必须找到同一个角色，避免切换到车辆背包
    TestEqual(TEXT("Walking HUD source"), ADeliveryHUD::ResolveCharacter(Character), Character);
    const FObjectPropertyBase* Driver = FindFProperty<FObjectPropertyBase>(ADeliveryMotorbike::StaticClass(), TEXT("Driver"));
    if (!TestNotNull(TEXT("Driver property"), Driver)) return false;
    Driver->SetObjectPropertyValue_InContainer(Bike, Character);
    TestEqual(TEXT("Riding HUD source"), ADeliveryHUD::ResolveCharacter(Bike), Character);
    Driver->SetObjectPropertyValue_InContainer(Bike, nullptr);
    TestNull(TEXT("No stale HUD source on empty bike"), ADeliveryHUD::ResolveCharacter(Bike));

    UClass* GameModeClass = LoadClass<ADeliveryGameMode>(nullptr,
        TEXT("/Game/Blueprint/GameMode/BP_DeliverGameMode.BP_DeliverGameMode_C"));
    // 同时检查正式蓝图，防止蓝图中保留旧 HUD 配置
    TestTrue(TEXT("Native game mode uses Delivery HUD"), GetDefault<ADeliveryGameMode>()->HUDClass->IsChildOf(ADeliveryHUD::StaticClass()));
    TestNotNull(TEXT("Production game mode loads"), GameModeClass);
    if (GameModeClass)
        TestTrue(TEXT("Blueprint game mode uses Delivery HUD"), GameModeClass->GetDefaultObject<ADeliveryGameMode>()->HUDClass->IsChildOf(ADeliveryHUD::StaticClass()));
    return true;
}
#endif
