#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/PlayerController.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Player/DeliverPlayerState.h"
#include "AbilitySystem/DeliverAbilitySystemComponent.h"
#include "AbilitySystem/DeliverAttributeSet.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Combat/DeliveryRagdollCombatComponent.h"
#include "PhysicsEngine/PhysicsSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryCombatTimingTest, "Delivery.Boxing.LowFrameRate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryCombatTimingTest::RunTest(const FString&)
{
    UClass* CharacterClass = LoadClass<ADeliveryCharacter>(nullptr,
        TEXT("/Game/Blueprint/Character/DeliverMan/BP_DeliveryMan.BP_DeliveryMan_C"));
    if (!TestNotNull(TEXT("production character"), CharacterClass)) return false;
    for (int32 Case = 0; Case < 3; ++Case)
    {
        const float Step = Case == 0 ? 1.f / 60.f : .1f;
        const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false)
            .CreatePhysicsScene(true).ShouldSimulatePhysics(true).CreateNavigation(false).CreateAISystem(false);
        UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
            ERHIFeatureLevel::Num, &Init);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        ON_SCOPE_EXIT { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); };
        auto* Floor = World->SpawnActor<AActor>();
        auto* Box = NewObject<UBoxComponent>(Floor);
        Floor->SetRootComponent(Box);
        Box->SetBoxExtent(FVector(20000, 20000, 50));
        Box->SetWorldLocation(FVector(0, 0, -50));
        Box->SetCollisionProfileName(TEXT("BlockAll"));
        Box->RegisterComponent();

        // 匀速刚体用于检查物理时间，不能只检查配置中的子步数量
        auto* Probe = World->SpawnActor<AActor>();
        auto* Sphere = NewObject<USphereComponent>(Probe);
        Probe->SetRootComponent(Sphere);
        Sphere->SetMobility(EComponentMobility::Movable);
        Sphere->SetSphereRadius(10);
        Sphere->SetWorldLocation(FVector(0, 0, 1000));
        Sphere->SetCollisionProfileName(TEXT("PhysicsActor"));
        Sphere->SetEnableGravity(false);
        Sphere->SetLinearDamping(0);
        Sphere->RegisterComponent();
        Sphere->SetSimulatePhysics(true);
        Sphere->SetPhysicsLinearVelocity(FVector(100, 0, 0));

        FActorSpawnParameters Spawn;
        Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto* Attacker = World->SpawnActor<ADeliveryCharacter>(CharacterClass, FVector(0, 0, 150), FRotator::ZeroRotator, Spawn);
        auto* Target = World->SpawnActor<ADeliveryCharacter>(CharacterClass, FVector(75, 0, 150), FRotator(0, 180, 0), Spawn);
        auto* AttackerState = World->SpawnActor<ADeliverPlayerState>();
        auto* TargetState = World->SpawnActor<ADeliverPlayerState>();
        auto* Controller = World->SpawnActor<APlayerController>();
        Controller->SetAsLocalPlayerController();
        Controller->Possess(Attacker);
        AttackerState->SetOwner(Controller);
        Attacker->SetPlayerState(AttackerState);
        Target->SetPlayerState(TargetState);
        auto* SourceASC = CastChecked<UDeliverAbilitySystemComponent>(AttackerState->GetAbilitySystemComponent());
        auto* TargetASC = CastChecked<UDeliverAbilitySystemComponent>(TargetState->GetAbilitySystemComponent());
        World->InitializeActorsForPlay(FURL());
        World->GetWorldSettings()->NotifyBeginPlay();
        SourceASC->InitializeAbilityActor(AttackerState, Attacker);
        TargetASC->InitializeAbilityActor(TargetState, Target);

        Attacker->GetActiveRagdoll()->StartRagdoll();
        Target->GetActiveRagdoll()->StartRagdoll();
        if (!Attacker->GetRagdollCombat()->HasBegunPlay()) Attacker->GetRagdollCombat()->BeginPlay();
        Sphere->SetPhysicsLinearVelocity(FVector(100, 0, 0));
        for (int32 Frame = 0; Frame < FMath::RoundToInt(2.f / Step); ++Frame)
        {
            ++GFrameCounter;
            World->Tick(LEVELTICK_All, Step);
        }
        AddInfo(FString::Printf(TEXT("physics probe x=%.2f time=%.2f step=%.3f maxSteps=%d substep=%.4f canPunch=%d combatBegun=%d"),
            Sphere->GetComponentLocation().X, World->GetTimeSeconds(), Step, UPhysicsSettings::Get()->MaxSubsteps,
            UPhysicsSettings::Get()->MaxSubstepDeltaTime, Attacker->GetActiveRagdoll()->CanDrivePunch(-1.f),
            Attacker->GetRagdollCombat()->HasBegunPlay()));
        TestTrue(*FString::Printf(TEXT("physics advances two real seconds at %.0f FPS"), 1.f / Step),
            FMath::IsNearlyEqual(Sphere->GetComponentLocation().X, 200.f, 15.f));

        // 使用项目中的技能、伤害效果和生命事件，验证命中后能进入晕倒
        AddInfo(FString::Printf(TEXT("ability=%s local=%d owner=%s avatar=%s"),
            *GetNameSafe(Attacker->GetPunchRightAbilityClass()), SourceASC->AbilityActorInfo->IsLocallyControlled(),
            *GetNameSafe(SourceASC->GetOwnerActor()), *GetNameSafe(SourceASC->GetAvatarActor())));
        TargetASC->SetNumericAttributeBase(UDeliverAttributeSet::GetHealthAttribute(), Case == 2 ? 100.f : 5.f);
        TestTrue(TEXT("punch ability activates"), SourceASC->TryActivateAbilityByClass(Attacker->GetPunchRightAbilityClass(), false));
        TestTrue(TEXT("ability starts the combat component"), Attacker->GetRagdollCombat()->IsPunching());
        bool bStunned = false;
        bool bCheckedRepeat = false;
        for (int32 Frame = 0; Frame < FMath::RoundToInt(1.5f / Step); ++Frame)
        {
            ++GFrameCounter;
            World->Tick(LEVELTICK_All, Step);
            bStunned |= TargetASC->IsStunned();
            if (Case == 2 && !bCheckedRepeat && TargetASC->GetNumericAttribute(UDeliverAttributeSet::GetHealthAttribute()) < 100.f)
            {
                // 保持同一帧的碰撞结果，重复通知不能再次扣血
                bCheckedRepeat = true;
                TargetASC->SetNumericAttributeBase(UDeliverAttributeSet::GetHealthAttribute(), 100.f);
                for (int32 Repeat = 0; Repeat < 5; ++Repeat)
                    Attacker->GetRagdollCombat()->OnPunchHitWindow.Broadcast(EMeleeHand::Right);
                TestEqual(TEXT("repeated contact within one punch does not deal damage again"),
                    TargetASC->GetNumericAttribute(UDeliverAttributeSet::GetHealthAttribute()), 100.f);
                SourceASC->CancelAbilities();
                TestFalse(TEXT("cancel ends the punch"), Attacker->GetRagdollCombat()->IsPunching());
                TestTrue(TEXT("cancel closes collision detection"),
                    Attacker->GetRagdollCombat()->GatherHits(EMeleeHand::Right).IsEmpty());
            }
        }
        AddInfo(FString::Printf(TEXT("FPS=%.0f target health=%.2f stunned=%d distance=%.1f"),
            1.f / Step, TargetASC->GetNumericAttribute(UDeliverAttributeSet::GetHealthAttribute()), bStunned,
            FVector::Dist(Attacker->GetMesh()->GetBoneLocation(TEXT("Hips")), Target->GetMesh()->GetBoneLocation(TEXT("Hips")))));
        if (Case == 2)
            TestTrue(TEXT("repeated contact was exercised"), bCheckedRepeat);
        else
        {
            TestTrue(TEXT("punch damage triggers stun through the health delegate"), bStunned);
            TestEqual(TEXT("stun disables active body control"), Target->GetActiveRagdoll()->GetControlMode(), EDeliveryRagdollControlMode::Limp);
        }
    }
    return true;
}
#endif
