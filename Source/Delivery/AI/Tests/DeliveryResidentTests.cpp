#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Residents/DeliveryResidentCharacter.h"
#include "AI/Residents/DeliveryResidentCombatComponent.h"
#include "AI/Residents/DeliveryNpcPresentationComponent.h"
#include "AI/Residents/DeliveryVehicleHoldComponent.h"
#include "AI/DeliveryNpcAgentComponent.h"
#include "AI/DeliveryAiGatewaySubsystem.h"
#include "AI/DeliveryAiSettings.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Character/DeliveryCharacter.h"
#include "Components/BoxComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryResidentIntegrationTest,"Delivery.AI.ResidentIntegration",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryResidentIntegrationTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>(); const bool OldEnabled=Settings->bEnabled;
    Settings->bEnabled=false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL()); World->GetWorldSettings()->NotifyBeginPlay();
    ON_SCOPE_EXIT { World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); Settings->bEnabled=OldEnabled; };
    auto* Floor=World->SpawnActor<AActor>(); auto* Box=NewObject<UBoxComponent>(Floor); Floor->SetRootComponent(Box);
    Box->SetBoxExtent(FVector(5000,5000,20)); Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics); Box->SetCollisionResponseToAllChannels(ECR_Block);
    Box->SetCollisionResponseToChannel(ECC_GameTraceChannel3,ECR_Ignore);
    Box->RegisterComponent(); Floor->SetActorLocation(FVector(0,0,-20));
    FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* A=World->SpawnActor<ADeliveryResidentCharacter>(FVector(0,0,90),FRotator::ZeroRotator,Spawn);
    auto* B=World->SpawnActor<ADeliveryResidentCharacter>(FVector(120,0,90),FRotator::ZeroRotator,Spawn);
    auto Tick=[&](float Seconds)
    { for (int i=0;i<FMath::CeilToInt(Seconds/.02f);++i) { ++GFrameCounter; World->Tick(LEVELTICK_All,.02f); } };
    TestTrue(TEXT("近距离居民可以挥拳"),A->Combat->CanStrike(B));
    const FGuid Attack=FGuid::NewGuid();
    TestTrue(TEXT("第一拳接受"),A->Combat->StartStrike(B,Attack));
    TestFalse(TEXT("挥拳窗口内重复请求拒绝"),A->Combat->StartStrike(B,Attack));
    Tick(.5f);
    TestTrue(TEXT("命中时刻后目标确实倒地"),B->IsKnockedDown());
    TestFalse(TEXT("倒地者不能被重复结算"),A->Combat->CanStrike(B));
    Tick(6.f);
    TestFalse(TEXT("到期恢复行动"),B->IsKnockedDown());
    B->SetActorLocation(FVector(1000,0,90)); TestFalse(TEXT("远距离拒绝攻击"),A->Combat->CanStrike(B));

    auto* Bike=World->SpawnActor<ADeliveryMotorbike>(FVector(2000,0,0),FRotator::ZeroRotator,Spawn);
    Bike->SetActorTickEnabled(false);
    auto* Driver=World->SpawnActorDeferred<ADeliveryCharacter>(ADeliveryCharacter::StaticClass(),FTransform(FVector(3000,0,90)),nullptr,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    Bike->Driver=Driver; // 只布置驾驶事实，装卸与上下车另有集成回归
    A->SetActorLocation(Bike->ResidentHold->GetHoldLocation(A));
    A->GetCharacterMovement()->StopMovementImmediately();
    TestTrue(TEXT("低速有人驾驶的车辆可被申请拽住"),Bike->ResidentHold->CanHold(A));
    auto ApproveHold=[&]()
    {
        A->ObserveFact(TEXT("SeenBike"),Bike,TEXT("看到低速车辆"),EDeliveryNpcEvidenceSource::Seen);
        Settings->bEnabled=true;
        const bool Queued=A->Agent->RequestDecision(); Settings->bEnabled=false;
        if (!Queued) { AddError(TEXT("Hold request not queued")); return false; }
        const auto Request=A->Agent->ActiveRequest;
        World->GetSubsystem<UDeliveryAiGatewaySubsystem>()->Cancel(A->Agent);
        const auto* Candidate=Request.Candidates.FindByPredicate([](const auto& C){ return C.Action==EDeliveryNpcAction::HoldVehicle; });
        if (!Candidate) { AddError(FString::Printf(TEXT("Hold candidate absent canHold=%d pos=%s bike=%s"),Bike->ResidentHold->CanHold(A),*A->GetActorLocation().ToString(),*Bike->GetActorLocation().ToString())); A->Agent->CancelDecision(); return false; }
        FDeliveryNpcDecisionProposal P; P.WorldEpoch=Request.WorldEpoch; P.NpcId=Request.NpcId; P.RequestId=Request.RequestId;
        P.ContextRevision=Request.ContextRevision; P.PlanRevision=Request.PlanRevision; P.Action=Candidate->Action;
        P.TargetId=Candidate->TargetId; P.EvidenceIds=Candidate->EvidenceIds; P.Duration=4;
        A->Agent->ReceiveDecision(P.RequestId,&P,NAME_None);
        FDeliveryNpcPublicState Action;
        return A->Agent->StartApprovedAction(Action) && A->BeginApprovedAction(Action);
    };
    TestTrue(TEXT("审批后执行拽车"),ApproveHold());
    TestTrue(TEXT("车辆占用存在"),Bike->ResidentHold->IsHeld());
    TestEqual(TEXT("限速倍率生效"),Bike->ResidentHold->GetSpeedScale(),.4f);
    TestFalse(TEXT("同一目标重复占用拒绝"),Bike->ResidentHold->TryHold(A,FGuid::NewGuid()));
    TestFalse(TEXT("第二居民不能争抢同一车辆"),Bike->ResidentHold->TryHold(B,FGuid::NewGuid()));
    for (int i=0;i<21;++i) Bike->ResidentHold->UpdateThrottle(1,.1f);
    TestFalse(TEXT("持续油门独立于车速挣脱"),Bike->ResidentHold->IsHeld());
    TestNull(TEXT("居民关联一起解除"),A->GetHeldVehicle());
    Bike->ResidentHold->Release(TEXT("RepeatedCleanup"));
    TestEqual(TEXT("解除后恢复原限速"),Bike->ResidentHold->GetSpeedScale(),1.f);
    TestTrue(TEXT("释放后能够重新申请"),ApproveHold());
    // 撤销未完成构造的占位驾驶事实，真实驾驶员的销毁恢复由多人 PIE 覆盖
    Bike->Driver=nullptr; Driver->Destroy();
    Bike->Destroy(); TestNull(TEXT("车辆销毁清除居民关联"),A->GetHeldVehicle());

    A->SetActorLocation(FVector(0,0,90)); B->SetActorLocation(FVector(120,0,90));
    const FGuid Turn=FGuid::NewGuid();
    TestTrue(TEXT("真实交谈事实可接收"),B->HearSpeech(A,TEXT("请让一下"),Turn,0));
    TestFalse(TEXT("重复对白不会再次改变记忆"),B->HearSpeech(A,TEXT("请让一下"),Turn,0));
    TestFalse(TEXT("超过连续对话轮数拒绝"),B->HearSpeech(A,TEXT("继续争吵"),FGuid::NewGuid(),4));
    TestTrue(TEXT("开始服务器对白"),A->Presentation->PublishSpeech(TEXT("这一句会被打断"),TEXT("Angry"),0));
    const FGuid Interrupted=A->Presentation->GetSpeech().TurnId; A->Presentation->CancelSpeech(); Tick(1);
    TestFalse(TEXT("被打断的整句不会进入对方记忆"),B->Agent->RecentEventIds.Contains(Interrupted));
    return true;
}
#endif
