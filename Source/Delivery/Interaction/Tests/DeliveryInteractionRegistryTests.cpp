#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/Pawn.h"
#include "Components/SceneComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Interaction/DeliveryInteractionRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryInteractionRegistryTest, "Delivery.Interaction.WorldRegistry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryInteractionRegistryTest::RunTest(const FString&)
{
    const auto Settings = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(true).ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
    UWorld* Worlds[2] = {};
    ON_SCOPE_EXIT
    {
        for (UWorld* World : Worlds)
        {
            if (!World) continue;
            World->EndPlay(EEndPlayReason::Quit);
            GEngine->DestroyWorldContext(World);
            World->DestroyWorld(false);
        }
    };
    for (UWorld*& World : Worlds)
    {
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
            ERHIFeatureLevel::Num, &Settings);
        if (!TestNotNull(TEXT("world created"), World)) return false;
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
        World->GetWorldSettings()->NotifyBeginPlay();
    }
    auto SpawnTarget = [](UWorld* World, float ActorX, float AnchorX)
    {
        AActor* Actor = World->SpawnActor<AActor>();
        auto* Anchor = NewObject<USceneComponent>(Actor);
        Actor->SetRootComponent(Anchor);
        Anchor->RegisterComponent();
        Actor->SetActorLocation(FVector(ActorX, 0, 0));
        auto* Point = NewObject<USceneComponent>(Actor);
        Point->SetupAttachment(Anchor);
        Point->RegisterComponent();
        Point->SetWorldLocation(FVector(AnchorX, 0, 0));
        auto* Action = NewObject<UDeliveryInteractableComponent>(Actor);
        Actor->AddInstanceComponent(Action);
        Action->InteractAnchor = Point;
        Action->InteractRadius = 2000.f;
        Action->RegisterComponent();
        return Action;
    };
    APawn* SeekerA = Worlds[0]->SpawnActor<APawn>();
    APawn* SeekerB = Worlds[1]->SpawnActor<APawn>();
    auto* RegistryA = Worlds[0]->GetSubsystem<UDeliveryInteractionRegistry>();
    auto* RegistryB = Worlds[1]->GetSubsystem<UDeliveryInteractionRegistry>();
    if (!TestNotNull(TEXT("first registry"), RegistryA) ||
        !TestNotNull(TEXT("second registry"), RegistryB)) return false;

    auto* Near = SpawnTarget(Worlds[0], 1000.f, 10.f);
    auto* Far = SpawnTarget(Worlds[0], 20.f, 100.f);
    auto* OtherWorld = SpawnTarget(Worlds[1], 1.f, 1.f);
    TArray<UDeliveryInteractableComponent*> Candidates;
    RegistryA->Register(Near);
    RegistryA->Register(OtherWorld);
    RegistryA->GetCandidates(SeekerA, EDeliveryInteractionKey::GeneralF, Candidates);
    TestEqual(TEXT("automatic registration is unique and isolated by world"), Candidates.Num(), 2);
    TestTrue(TEXT("nearest uses the anchor instead of the actor origin"),
        RegistryA->FindNearest(SeekerA, EDeliveryInteractionKey::GeneralF) == Near);
    TestTrue(TEXT("second world sees only its own target"),
        RegistryB->FindNearest(SeekerB, EDeliveryInteractionKey::GeneralF) == OtherWorld);
    TestNull(TEXT("queries cannot use a pawn from another world"),
        RegistryA->FindNearest(SeekerB, EDeliveryInteractionKey::GeneralF));

    Near->bInteractEnabled = false;
    TestTrue(TEXT("disabled targets are excluded"),
        RegistryA->FindNearest(SeekerA, EDeliveryInteractionKey::GeneralF) == Far);
    Near->bInteractEnabled = true;
    Near->GetOwner()->Destroy();
    RegistryA->GetCandidates(SeekerA, EDeliveryInteractionKey::GeneralF, Candidates);
    TestEqual(TEXT("destroyed actor unregisters its component"), Candidates.Num(), 1);
    RegistryA->Unregister(Far);
    TestNull(TEXT("unregistered targets are not returned"),
        RegistryA->FindNearest(SeekerA, EDeliveryInteractionKey::GeneralF));
    TestTrue(TEXT("cleanup in one world does not affect the other"),
        RegistryB->FindNearest(SeekerB, EDeliveryInteractionKey::GeneralF) == OtherWorld);
    return true;
}
#endif
