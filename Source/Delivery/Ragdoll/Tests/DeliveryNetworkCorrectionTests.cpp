#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Ragdoll/Debug/DeliveryPoseDebug.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "PhysicsControlComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNetworkCorrectionTest, "Delivery.Ragdoll.NetworkCorrection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryNetworkCorrectionTest::RunTest(const FString&)
{
    UClass* Class = LoadClass<ADeliveryCharacter>(nullptr,
        TEXT("/Game/Blueprint/Character/DeliverMan/BP_DeliveryMan.BP_DeliveryMan_C"));
    if (!TestNotNull(TEXT("production character"), Class))
        return false;
    const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(true).ShouldSimulatePhysics(true).CreateNavigation(false).CreateAISystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
        ERHIFeatureLevel::Num, &Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    AActor* Floor = World->SpawnActor<AActor>();
    auto* Box = NewObject<UBoxComponent>(Floor);
    Floor->SetRootComponent(Box);
    Box->SetBoxExtent(FVector(20000, 20000, 50));
    Box->SetWorldLocation(FVector(0, 0, -50));
    Box->SetCollisionProfileName(TEXT("BlockAll"));
    Box->RegisterComponent();
    auto* Character = World->SpawnActor<ADeliveryCharacter>(Class, FVector(0, 0, 150), FRotator::ZeroRotator);
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();
    auto* Rag = Character->GetActiveRagdoll();
    auto* Mesh = Character->GetMesh();
    Rag->StartRagdoll();
    auto* Controls = Rag->PhysicsControl.Get();


#if WITH_EDITOR
    // 诊断读取不得修改步态或身体，只返回与当前刚体一致的位置
    FDeliveryPoseDebug DebugPose;
    const FVector BeforeDebug = Mesh->GetCenterOfMass(Rag->Bones.Hips);
    const float BeforeStep = Rag->Movement.LeftFoot.Alpha;
    TestTrue(TEXT("debug snapshot is available for active ragdoll"), Rag->ReadPoseDebug(DebugPose));
    TestTrue(TEXT("debug snapshot uses actual pelvis"), DebugPose.Hips.Equals(BeforeDebug));
    TestTrue(TEXT("debug read preserves body position"), Mesh->GetCenterOfMass(Rag->Bones.Hips).Equals(BeforeDebug));
    TestEqual(TEXT("debug read preserves step progress"), Rag->Movement.LeftFoot.Alpha, BeforeStep);

#endif

    // 长帧不能消耗超过实际物理模拟步长的恢复时间
    Rag->Recovery.ReportIssue(EDeliveryRecoveryIssue::FootBlocked, 1.f);
    const auto* Settings = UPhysicsSettings::Get();
    const float MaxStep = Settings->bSubstepping
        ? Settings->MaxSubstepDeltaTime * Settings->MaxSubsteps : Settings->MaxPhysicsDeltaTime;
    Rag->UpdateControlTargets(.2f);
    const float ExpectedStep = MaxStep > 0.f ? FMath::Min(.2f, MaxStep) : .2f;
    TestTrue(TEXT("gait timing follows the physics step during a long frame"),
        FMath::IsNearlyEqual(Rag->Recovery.GetReleaseRemaining(), 1.f - ExpectedStep));
    Rag->Recovery.ResetFeet();

    // 身体发生小幅偏移后，平地站立目标仍留在原位置
    Rag->UpdateControlTargets(.016f);
    TestTrue(TEXT("flat ground holds the standing position"), Rag->Movement.bStandAnchored);
    const FVector OriginalAnchor = Rag->Movement.StandAnchor;
    Rag->Movement.SmoothedBalanceOffset = FVector(8, 0, 0);
    Rag->UpdateControlTargets(.016f);
    TestTrue(TEXT("balance adjustment does not move the standing anchor"),
        Rag->Movement.StandAnchor.Equals(OriginalAnchor, .01));

    // 在迈步中途修正位置，检查脚与目标的距离和动作进度保持不变
    Rag->SetMoveInput(FVector2D(0, 1));
    Rag->UpdateControlTargets(.016f);
    TestFalse(TEXT("movement releases the standing anchor"), Rag->Movement.bStandAnchored);
    Rag->Movement.StartupPlantRemaining = 0;
    TestTrue(TEXT("can start a step"), Rag->BeginStep(Rag->Movement.LeftFoot, FVector::ForwardVector));
    Rag->UpdateFootTarget(Rag->Movement.LeftFoot, .1f);
    FPhysicsControlTarget BeforeTarget;
    Controls->GetControlTarget(Rag->Movement.LeftFoot.Control, BeforeTarget);
    const FVector BeforeFoot = Mesh->GetCenterOfMass(Rag->Movement.LeftFoot.Bone);
    const FVector BeforeStart = Rag->Movement.LeftFoot.Start;
    const FVector BeforeLanding = Rag->Movement.LeftFoot.Target;
    const float BeforeAlpha = Rag->Movement.LeftFoot.Alpha;
    const FVector BeforeVelocity = Mesh->GetPhysicsLinearVelocity(Rag->Movement.LeftFoot.Bone);
    Controls->SetControlEnabled(Rag->Movement.RightFoot.Control, false, true, false);
    Rag->ApplyOwnerTranslation(FVector(120, -50, 10));
    FPhysicsControlTarget AfterTarget;
    Controls->GetControlTarget(Rag->Movement.LeftFoot.Control, AfterTarget);
    TestTrue(TEXT("foot target keeps the same distance from the body"),
        (BeforeTarget.TargetPosition - BeforeFoot).Equals(
            AfterTarget.TargetPosition - Mesh->GetCenterOfMass(Rag->Movement.LeftFoot.Bone), .01));
    TestTrue(TEXT("swing path keeps the same length"), (BeforeLanding - BeforeStart).Equals(
        Rag->Movement.LeftFoot.Target - Rag->Movement.LeftFoot.Start, .01));
    TestEqual(TEXT("step progress is preserved"), Rag->Movement.LeftFoot.Alpha, BeforeAlpha);
    TestTrue(TEXT("body velocity is preserved"), BeforeVelocity.Equals(
        Mesh->GetPhysicsLinearVelocity(Rag->Movement.LeftFoot.Bone), .01));
    TestTrue(TEXT("target velocity does not gain a teleport impulse"),
        BeforeTarget.TargetVelocity.Equals(AfterTarget.TargetVelocity, .01));
    TestTrue(TEXT("swing foot remains driven"), Controls->GetControlEnabled(Rag->Movement.LeftFoot.Control));
    TestFalse(TEXT("planted foot is not enabled by correction"), Controls->GetControlEnabled(Rag->Movement.RightFoot.Control));
    TestTrue(TEXT("ragdoll remains physical"), Mesh->IsSimulatingPhysics());
    // 远端回放必须有整条腿的姿态，不能只发髋部和脚
    Rag->CaptureNetworkSnapshot();
    for (const FName Bone : {FName(TEXT("LeftUpLeg")), FName(TEXT("LeftLeg")),
        FName(TEXT("RightUpLeg")), FName(TEXT("RightLeg"))})
    {
        TestTrue(TEXT("snapshot contains each leg body"), Rag->ReplicatedSnapshot.Bodies.ContainsByPredicate(
            [Bone](const FDeliveryRagdollBodyState& Body) { return Body.Bone == Bone; }));
    }

    // 本机小幅校正不能直接改写骨骼位置，同一包快照也不能反复加速
    Character->SetAutonomousProxy(true);
    Character->SwapRoles();
    FDeliveryRagdollSnapshot Snapshot;
    Snapshot.Sequence = 1;
    auto& State = Snapshot.Bodies.AddDefaulted_GetRef();
    State.Bone = Rag->Bones.Hips;
    FBodyInstance* Hips = Mesh->GetBodyInstance(Rag->Bones.Hips);
    const FTransform BeforeTransform = Hips->GetUnrealWorldTransform();
    State.Position = BeforeTransform.GetLocation() + FVector(30, 0, 0);
    State.LinearVelocity = FVector::ZeroVector;
    Rag->Network.Receive(Snapshot, World->GetTimeSeconds());
    Rag->ApplyNetworkSnapshot(.05f);
    TestTrue(TEXT("soft correction does not teleport the body"),
        Hips->GetUnrealWorldTransform().Equals(BeforeTransform, .01));
    const FVector CorrectedVelocity = Hips->GetPhysicsActor()->GetGameThreadAPI().LinearImpulseVelocity();
    TestTrue(TEXT("soft correction moves toward the server"), CorrectedVelocity.X > 0.f);
    Rag->ApplyNetworkSnapshot(.05f);
    TestTrue(TEXT("same snapshot is not applied twice"),
        CorrectedVelocity.Equals(Hips->GetPhysicsActor()->GetGameThreadAPI().LinearImpulseVelocity(), .01));
    // 起身交接必须等待 Active 之后的新快照，序号回绕也不能卡住
    Rag->ReplicatedControlMode = EDeliveryRagdollControlMode::Recovering;
    Rag->OnRep_ControlMode();
    Rag->ReplicatedSnapshot.Sequence = MAX_uint16;
    Rag->OnRep_RagdollSnapshot();
    TestTrue(TEXT("recovery plays server snapshots"), Rag->Network.IsPlayingRecovery());
    TestFalse(TEXT("recovery disables local pelvis drive"), Controls->GetControlEnabled(Rag->PelvisControl));
    Rag->ReplicatedControlMode = EDeliveryRagdollControlMode::Active;
    Rag->OnRep_ControlMode();
    Rag->OnRep_RagdollSnapshot();
    TestTrue(TEXT("repeated old snapshot cannot resume prediction"), Rag->Network.IsAwaitingResume());
    Rag->ReplicatedSnapshot.Sequence = 0;
    Rag->OnRep_RagdollSnapshot();
    TestFalse(TEXT("new snapshot resumes prediction after sequence wrap"), Rag->Network.IsPlayingRecovery());
    TestFalse(TEXT("handoff request is consumed"), Rag->Network.IsAwaitingResume());
    TestTrue(TEXT("local pelvis drive resumes after handoff"), Controls->GetControlEnabled(Rag->PelvisControl));
    TestTrue(TEXT("handoff keeps physics enabled"), Mesh->IsSimulatingPhysics());
    Character->SwapRoles();
    World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false);
    GEngine->DestroyWorldContext(World);
    return true;
}
#endif
