#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Actions/DeliveryVehicleHoldComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Character/DeliveryCharacter.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
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
    auto* A=World->SpawnActor<ADeliveryNpcCharacter>(FVector(0,0,90),FRotator::ZeroRotator,Spawn);
    auto* B=World->SpawnActor<ADeliveryNpcCharacter>(FVector(120,0,90),FRotator::ZeroRotator,Spawn);
    auto Tick=[&](float Seconds)
    { for (int i=0;i<FMath::CeilToInt(Seconds/.02f);++i) { ++GFrameCounter; World->Tick(LEVELTICK_All,.02f); } };
    TestTrue(TEXT("导航已将近处目标判定为到达"),A->HasReachedMoveTarget(B));
    TestFalse(TEXT("已到达的目标不再提供移动动作"),A->CanExecute(EDeliveryNpcAction::MoveTo,B));
    B->SetActorLocation(FVector(250,0,90));
    TestTrue(TEXT("尚未到达的目标仍可选择移动"),A->CanExecute(EDeliveryNpcAction::MoveTo,B));
    B->GetCapsuleComponent()->SetCapsuleSize(100,110);
    TestTrue(TEXT("较大目标的到达判断包含目标碰撞半径"),A->HasReachedMoveTarget(B));
    TestFalse(TEXT("站在较大目标旁边也不再重复移动"),A->CanExecute(EDeliveryNpcAction::MoveTo,B));
    B->GetCapsuleComponent()->SetCapsuleSize(34,88);B->SetActorLocation(FVector(120,0,90));
    FDeliveryNpcDecisionRequest SilentRequest;
    FGuid SilentMessage;
    auto StartSilentReply=[&](bool bStream)
    {
        A->Agent->CancelPlan(TEXT("NextReplyTest"));
        SilentMessage=FGuid::NewGuid();
        FDeliveryNpcObservation Message;Message.EventId=SilentMessage;Message.Kind=TEXT("PlayerSpeech");
        Message.Subject=B;Message.Source=EDeliveryNpcEvidenceSource::Heard;Message.Description=TEXT("你看一下他");
        A->Agent->ObserveEvent(Message);
        Settings->bEnabled=true;
        const bool Queued=A->Agent->RequestDecision();Settings->bEnabled=false;
        if (!Queued) return false;
        SilentRequest=A->Agent->ActiveRequest;
        World->GetSubsystem<UDeliveryAiGatewaySubsystem>()->Cancel(A->Agent);
        TestFalse(TEXT("给模型的候选中不包含已到达目标的移动"),SilentRequest.Candidates.ContainsByPredicate([](const auto& C){return C.Action==EDeliveryNpcAction::MoveTo;}));
        const auto* Candidate=SilentRequest.Candidates.FindByPredicate([](const auto& C){return C.Action==EDeliveryNpcAction::LookAt;});
        if (!Candidate) return false;
        FDeliveryNpcDecisionProposal Proposal;Proposal.WorldEpoch=SilentRequest.WorldEpoch;Proposal.NpcId=SilentRequest.NpcId;
        Proposal.RequestId=SilentRequest.RequestId;Proposal.ContextRevision=SilentRequest.ContextRevision;Proposal.PlanRevision=SilentRequest.PlanRevision;
        Proposal.Duration=1;Proposal.Action=EDeliveryNpcAction::Wait;
        FName Reason;
        TestFalse(TEXT("玩家尚未获得回应时拒绝无台词等待"),A->Agent->TryAcceptProposal(Proposal,Reason));
        TestEqual(TEXT("空等被拒绝的原因明确"),Reason,FName(TEXT("MissingPlayerReply")));
        Proposal.Action=Candidate->Action;Proposal.TargetId=Candidate->TargetId;Proposal.EvidenceIds=Candidate->EvidenceIds;
        if (bStream)
        {
            if (!A->Agent->ReceiveStreamDecision(Proposal.RequestId,Proposal)) return false;
        }
        else A->Agent->ReceiveDecision(Proposal.RequestId,&Proposal,NAME_None);
        FDeliveryNpcPublicState Action;
        return A->Agent->StartAction(Action) && A->ExecuteAction(Action);
    };
    auto ReplyCount=[&]()
    {
        return A->Agent->Evidence.FilterByPredicate([&](const auto& Item)
            {return Item.Kind==TEXT("ActionReply") && Item.ReplyToEventId==SilentMessage;}).Num();
    };
    if (!TestTrue(TEXT("无台词动作可以作为非流式建议开始"),StartSilentReply(false))) return false;
    TestEqual(TEXT("动作开始不等于回应完成"),ReplyCount(),0);
    const FGuid FirstSilentAction=A->Agent->GetPublicState().ActionId;
    A->Agent->TryFinishAction(FirstSilentAction,EDeliveryNpcActionStatus::Succeeded,NAME_None);
    TestEqual(TEXT("非流式动作成功后记录回应"),ReplyCount(),1);
    A->Agent->TryFinishAction(FirstSilentAction,EDeliveryNpcActionStatus::Succeeded,NAME_None);
    TestEqual(TEXT("同一动作结束通知不会重复记录回应"),ReplyCount(),1);
    Settings->bEnabled=true;
    const bool FollowupQueued=A->Agent->RequestDecision();Settings->bEnabled=false;
    if (!TestTrue(TEXT("动作回应完成后可以继续请求"),FollowupQueued)) return false;
    const auto Followup=A->Agent->ActiveRequest;
    World->GetSubsystem<UDeliveryAiGatewaySubsystem>()->Cancel(A->Agent);
    FDeliveryNpcDecisionProposal QuietWait;QuietWait.WorldEpoch=Followup.WorldEpoch;QuietWait.NpcId=Followup.NpcId;
    QuietWait.RequestId=Followup.RequestId;QuietWait.ContextRevision=Followup.ContextRevision;QuietWait.PlanRevision=Followup.PlanRevision;
    QuietWait.Action=EDeliveryNpcAction::Wait;QuietWait.Duration=1;
    FName QuietWaitReason;
    TestTrue(TEXT("已经用动作回应过的问题允许安静等待"),A->Agent->TryAcceptProposal(QuietWait,QuietWaitReason));
    TestFalse(TEXT("安静等待不重复关联已回应的玩家消息"),A->Agent->GetPublicState().ReplyToEventId.IsValid());
    if (!TestTrue(TEXT("准备动作失败的无台词回应"),StartSilentReply(false))) return false;
    A->Agent->TryFinishAction(A->Agent->GetPublicState().ActionId,EDeliveryNpcActionStatus::Failed,TEXT("TestFailure"));
    TestEqual(TEXT("失败动作不能算已回应"),ReplyCount(),0);
    if (!TestTrue(TEXT("准备流式先结束的无台词回应"),StartSilentReply(true))) return false;
    A->Agent->FinishReply(SilentRequest.RequestId,true,0);
    TestEqual(TEXT("收到零句回复时仍要等待动作完成"),ReplyCount(),0);
    TestTrue(TEXT("零句回复保留当前动作直到执行结束"),A->Presentation->IsReplyWithoutSpeech(A->Agent->GetPublicState().ActionId));
    A->Agent->TryFinishAction(A->Agent->GetPublicState().ActionId,EDeliveryNpcActionStatus::Succeeded,NAME_None);
    TestEqual(TEXT("流先结束、动作后成功时记录一次回应"),ReplyCount(),1);
    if (!TestTrue(TEXT("准备动作先结束的无台词回应"),StartSilentReply(true))) return false;
    A->Agent->TryFinishAction(A->Agent->GetPublicState().ActionId,EDeliveryNpcActionStatus::Succeeded,NAME_None);
    TestEqual(TEXT("动作先成功时不能猜测模型是否还会发来台词"),ReplyCount(),0);
    A->Agent->FinishReply(SilentRequest.RequestId,true,0);
    TestEqual(TEXT("动作先结束、流后确认零句时记录一次回应"),ReplyCount(),1);
    if (!TestTrue(TEXT("准备后端失败的无台词回应"),StartSilentReply(true))) return false;
    A->Agent->TryFinishAction(A->Agent->GetPublicState().ActionId,EDeliveryNpcActionStatus::Succeeded,NAME_None);
    A->Agent->FinishReply(SilentRequest.RequestId,false,0);
    TestEqual(TEXT("后端失败没有确认零句，不能假定回应完成"),ReplyCount(),0);
    if (!TestTrue(TEXT("准备包含台词的流式回应"),StartSilentReply(true))) return false;
    FDeliveryNpcSentence Spoken;Spoken.Sequence=0;Spoken.Text=TEXT("我看着呢");
    A->Agent->ReceiveSentence(SilentRequest.RequestId,Spoken);
    A->Agent->FinishReply(SilentRequest.RequestId,true,1);
    A->Agent->TryFinishAction(A->Agent->GetPublicState().ActionId,EDeliveryNpcActionStatus::Succeeded,NAME_None);
    TestEqual(TEXT("有台词的回复等待台词播放结束，不提前记作无声回应"),ReplyCount(),0);
    TestFalse(TEXT("无台词动作没有伪造说话记录"),A->Agent->Evidence.ContainsByPredicate([](const auto& Item){return Item.Kind==TEXT("SelfSpeech");}));
    A->Agent->CancelPlan(TEXT("ReplyTestsFinished"));
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
        return A->Agent->StartAction(Action) && A->ExecuteAction(Action);
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
