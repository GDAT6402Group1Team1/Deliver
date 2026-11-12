#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Vehicle/DeliveryTrunkPassengerComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Player/DeliverPlayerState.h"
#include "AbilitySystem/DeliverAbilitySystemComponent.h"
#include "AbilitySystem/DeliverAttributeSet.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Grab/DeliveryGrabbableComponent.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "GameplayEffect.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryTrunkPassengerTest,"Delivery.Vehicle.TrunkPassenger",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryTrunkPassengerTest::RunTest(const FString&)
{
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(true).CreateNavigation(false).CreateAISystem(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    if (!TestNotNull(TEXT("physics world"),World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT { World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
    auto* Floor=World->SpawnActor<AActor>(); auto* Ground=NewObject<UBoxComponent>(Floor);
    Floor->SetRootComponent(Ground); Ground->SetBoxExtent(FVector(3000,3000,20));
    Ground->SetCollisionProfileName(TEXT("BlockAll")); Ground->RegisterComponent(); Floor->SetActorLocation(FVector(0,0,-20));
    auto* BikeClass=LoadClass<ADeliveryMotorbike>(nullptr,TEXT("/Game/Vehicle/Motorbike/BP_Motorbike.BP_Motorbike_C"));
    auto* CharacterClass=LoadClass<ADeliveryCharacter>(nullptr,TEXT("/Game/Blueprint/Character/DeliverMan/BP_DeliveryMan.BP_DeliveryMan_C"));
    if (!TestNotNull(TEXT("bike class"),BikeClass)||!TestNotNull(TEXT("player class"),CharacterClass)) return false;
    FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Bike=World->SpawnActor<ADeliveryMotorbike>(BikeClass,FVector(0,0,70),FRotator::ZeroRotator,Spawn);
    const FVector Anchor=Bike->GetPassengerFrame().GetLocation();
    auto* Victim=World->SpawnActor<ADeliveryCharacter>(CharacterClass,Anchor+FVector(-70,80,20),FRotator::ZeroRotator,Spawn);
    auto* User=World->SpawnActor<ADeliveryCharacter>(CharacterClass,Anchor+FVector(-130,0,0),FRotator::ZeroRotator,Spawn);
    auto* PS=World->SpawnActor<ADeliverPlayerState>(); Victim->SetPlayerState(PS);
    auto* UserPS=World->SpawnActor<ADeliverPlayerState>(); User->SetPlayerState(UserPS);
    World->InitializeActorsForPlay(FURL()); World->GetWorldSettings()->NotifyBeginPlay();
    auto* ASC=CastChecked<UDeliverAbilitySystemComponent>(PS->GetAbilitySystemComponent()); ASC->InitializeAbilityActor(PS,Victim);
    CastChecked<UDeliverAbilitySystemComponent>(UserPS->GetAbilitySystemComponent())->InitializeAbilityActor(UserPS,User);
    auto* Ragdoll=Victim->GetActiveRagdoll(); Ragdoll->StartRagdoll();
    const auto PawnResponse=Victim->GetMesh()->GetCollisionResponseToChannel(ECC_Pawn);
    User->GetActiveRagdoll()->StopRagdoll(); User->SetActorEnableCollision(false);
    ASC->SetNumericAttributeBase(UDeliverAttributeSet::GetHealthAttribute(),0.f);
    ASC->SetNumericAttributeBase(UDeliverAttributeSet::GetHealthRegenRateAttribute(),0.f);
    auto* Grab=User->GetGrabComponent();
    auto PrepareGrab=[&]()
    {
        Grab->GrabTarget=Victim; Grab->WantedHands=3;
        Victim->GetGrabbableComponent()->RegisterGrabber(User);
    };
    PrepareGrab();
    const FVector UserPosition=User->GetActorLocation();
    User->SetActorLocation(UserPosition+FVector(1000,0,0));
    TestFalse(TEXT("remote load rejected without releasing grab"),Bike->UsePassengerTrunk(User));
    TestTrue(TEXT("rejected request retains drag"),Grab->GrabTarget==Victim);
    User->SetActorLocation(UserPosition);
    auto* Wall=World->SpawnActor<AActor>(); auto* WallBody=NewObject<UBoxComponent>(Wall);
    Wall->SetRootComponent(WallBody); WallBody->SetBoxExtent(FVector(8,100,200));
    WallBody->SetCollisionProfileName(TEXT("BlockAll")); WallBody->RegisterComponent();
    Wall->SetActorLocation(FMath::Lerp(User->GetPawnViewLocation(),Anchor,.5f));
    TestFalse(TEXT("load through wall rejected"),Bike->UsePassengerTrunk(User));
    Wall->Destroy();
    auto* Rival=World->SpawnActor<ADeliveryCharacter>(CharacterClass,FVector(1000,0,200),FRotator::ZeroRotator,Spawn);
    if (!TestNotNull(TEXT("rival player"),Rival)) return false;
    Victim->GetGrabbableComponent()->RegisterGrabber(Rival);
    TestEqual(TEXT("two draggers registered"),Victim->GetGrabbableComponent()->GrabberCount,2);
    TestFalse(TEXT("contested victim rejected"),Bike->UsePassengerTrunk(User));
    Victim->GetGrabbableComponent()->UnregisterGrabber(Rival);
    Rival->Destroy();
    auto* Package=World->SpawnActor<ADeliveryPackageItem>();
    TestTrue(TEXT("package occupies trunk"),Bike->AttachCargo(Package));
    TestFalse(TEXT("package and passenger mutually exclusive"),Bike->UsePassengerTrunk(User));
    Bike->ReleaseCargo();
    TestTrue(TEXT("load dragged stunned player"),Bike->UsePassengerTrunk(User));
    TestFalse(TEXT("repeated request rejected"),Bike->UsePassengerTrunk(User));
    TestFalse(TEXT("cannot put package over passenger"),Bike->AttachCargo(Package));
    TestFalse(TEXT("cannot summon occupied vehicle"),Bike->SummonTo(FVector(500,0,70),0));
    TestEqual(TEXT("grab transaction releases hands"),Victim->GetGrabbableComponent()->GrabberCount,0);
    TestTrue(TEXT("physics never disabled"),Victim->GetMesh()->IsSimulatingPhysics(TEXT("Hips")));
    // 真实伤害 GE 必须被拦截，回血仍可触发 40% 的出箱流程
    ASC->SetNumericAttributeBase(UDeliverAttributeSet::GetHealthAttribute(),20.f);
    auto* Damage=NewObject<UGameplayEffect>(); Damage->DurationPolicy=EGameplayEffectDurationType::Instant;
    auto& Modifier=Damage->Modifiers.AddDefaulted_GetRef(); Modifier.Attribute=UDeliverAttributeSet::GetHealthAttribute();
    Modifier.ModifierOp=EGameplayModOp::Additive; Modifier.ModifierMagnitude=FScalableFloat(-10.f);
    ASC->ApplyGameplayEffectToSelf(Damage,1.f,ASC->MakeEffectContext());
    TestEqual(TEXT("damage blocked in trunk"),ASC->GetNumericAttribute(UDeliverAttributeSet::GetHealthAttribute()),20.f);
    for (int I=0; I<75; ++I)
    {
        ++GFrameCounter; World->Tick(LEVELTICK_All,1.f/30.f);
        if (I%15==0) AddInfo(FString::Printf(TEXT("TrunkPhysics phase=%d error=%.1f hips=%s frame=%s"),
            int(Victim->TrunkPassenger->GetPhase()),Ragdoll->GetTrunkPoseError(),
            *Ragdoll->GetPhysicalPelvisLocation().ToString(),*Bike->GetPassengerFrame().GetLocation().ToString()));
    }
    TestTrue(TEXT("loading settles instead of emergency release"),Victim->TrunkPassenger->GetPhase()==EDeliveryPassengerPhase::Riding);
    ASC->SetNumericAttributeBase(UDeliverAttributeSet::GetHealthAttribute(),39.f);
    TestTrue(TEXT("39 percent remains in trunk"),Victim->TrunkPassenger->GetPhase()==EDeliveryPassengerPhase::Riding);
    ASC->SetNumericAttributeBase(UDeliverAttributeSet::GetHealthAttribute(),40.f);
    TestTrue(TEXT("40 percent requests exit before waking"),Victim->TrunkPassenger->IsPassenger() && ASC->IsStunned());
    for (int I=0;I<90;++I) { ++GFrameCounter; World->Tick(LEVELTICK_All,1.f/30.f); }
    TestFalse(TEXT("passenger exited"),Victim->TrunkPassenger->IsPassenger());
    TestFalse(TEXT("vehicle occupancy cleared"),Bike->HasTrunkPassenger());
    TestFalse(TEXT("40 percent wakes only after exit"),ASC->IsStunned());
    TestTrue(TEXT("transport motors cleaned"),Ragdoll->TrunkControls.IsEmpty());
    TestTrue(TEXT("physical body remains live after exit"),Victim->GetMesh()->IsSimulatingPhysics(TEXT("Hips")));
    TestEqual(TEXT("original pawn collision restored"),Victim->GetMesh()->GetCollisionResponseToChannel(ECC_Pawn),PawnResponse);
    TestTrue(TEXT("package can load after passenger leaves"),Bike->AttachCargo(Package));
    Bike->ReleaseCargo();
    auto Tick=[&](int Count,float Dt=1.f/30.f)
    { for (int I=0;I<Count;++I) { ++GFrameCounter; World->Tick(LEVELTICK_All,Dt); } };
    auto LoadAgain=[&]()
    {
        ASC->SetNumericAttributeBase(UDeliverAttributeSet::GetHealthAttribute(),0.f);
        Ragdoll->EmergencyMoveTrunkBody(Anchor+FVector(-70,80,0));
        PrepareGrab();
        return Bike->UsePassengerTrunk(User);
    };
    TestTrue(TEXT("load again after automatic release"),LoadAgain());
    Tick(75);
    TestTrue(TEXT("other player can request early removal"),Bike->UsePassengerTrunk(User));
    Tick(90);
    TestFalse(TEXT("early removal clears passenger"),Victim->TrunkPassenger->IsPassenger());
    TestTrue(TEXT("early removal keeps stun"),ASC->IsStunned());
    ASC->SetNumericAttributeBase(UDeliverAttributeSet::GetHealthAttribute(),20.f);
    ASC->ApplyGameplayEffectToSelf(Damage,1.f,ASC->MakeEffectContext());
    TestEqual(TEXT("damage protection removed after exit"),ASC->GetNumericAttribute(UDeliverAttributeSet::GetHealthAttribute()),10.f);
    TestTrue(TEXT("reload for recovery during loading"),LoadAgain());
    ASC->SetNumericAttributeBase(UDeliverAttributeSet::GetHealthAttribute(),40.f);
    Tick(90);
    TestFalse(TEXT("wake during loading finishes safe release"),Victim->TrunkPassenger->IsPassenger());
    TestFalse(TEXT("wake during loading clears stun after release"),ASC->IsStunned());
    TestTrue(TEXT("reload for low frame rate"),LoadAgain());
    Tick(30,1.f/10.f);
    TestTrue(TEXT("10 FPS loading converges"),Victim->TrunkPassenger->GetPhase()==EDeliveryPassengerPhase::Riding);
    Bike->Destroy(); Tick(2);
    Victim->TrunkPassenger->Release(true);
    TestFalse(TEXT("destroy vehicle cleans occupant idempotently"),Victim->TrunkPassenger->IsPassenger());
    TestTrue(TEXT("destroy vehicle cleans motors"),Ragdoll->TrunkControls.IsEmpty());
    TestTrue(TEXT("destroy vehicle retains normal stun"),ASC->IsStunned());
    Bike=World->SpawnActor<ADeliveryMotorbike>(BikeClass,FVector(0,0,70),FRotator::ZeroRotator,Spawn);
    TestTrue(TEXT("load before disconnect cleanup"),LoadAgain());
    Victim->Destroy(); Tick(2);
    TestFalse(TEXT("passenger destruction clears occupancy"),Bike->HasTrunkPassenger());
    TestTrue(TEXT("cargo available after passenger destruction"),Bike->AttachCargo(Package));
    return true;
}
#endif
