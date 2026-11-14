#include "Misc/AutomationTest.h"
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcDecisionTraceTest,"Delivery.AI.DecisionTrace",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryNpcDecisionTraceTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
    const bool Enabled=Settings->bEnabled;
    Settings->bEnabled=false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL()); World->GetWorldSettings()->NotifyBeginPlay();
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
        Settings->bEnabled=Enabled;
    };
    FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Npc=World->SpawnActor<ADeliveryNpcCharacter>(FVector(0,0,90),FRotator::ZeroRotator,Spawn);
    if (!TestNotNull(TEXT("NPC created"),Npc)) return false;
    Npc->SetActorTickEnabled(false);
    auto* Agent=Npc->Agent.Get();
    auto* Gateway=World->GetSubsystem<UDeliveryAiGatewaySubsystem>();
    FDeliveryNpcDecisionRequest Base;
    if (!TestTrue(TEXT("Real request identity"),Agent->BuildDecisionRequest(Gateway->GetWorldEpoch(),Base))) return false;
    Base.SelfName=FString::ChrN(200,TEXT('N'));
    Base.Evidence.Reset(); Base.Subjects.Reset(); Base.Candidates.Reset();
    for (int32 Index=0;Index<20;++Index)
    {
        FDeliveryNpcEvidence Evidence;
        Evidence.EventId=FGuid::NewGuid(); Evidence.Description=FString::Printf(TEXT("event-%d "),Index)+FString::ChrN(400,TEXT('x'));
        Evidence.bHasObservedLocation=true; Evidence.ObservedLocation=FVector(Index,2,3);
        Evidence.ObservedAt=Index; Evidence.ExpiresAt=Index+30;
        Base.Evidence.Add(Evidence);
        FDeliveryNpcSubject Subject; Subject.SubjectId=FGuid::NewGuid(); Subject.Name=FString::ChrN(200,TEXT('s')); Subject.Kind=TEXT("Npc");
        Base.Subjects.Add(Subject);
        FDeliveryNpcCandidate Candidate; Candidate.Action=EDeliveryNpcAction::Wait; Candidate.TargetId=Subject.SubjectId;
        Base.Candidates.Add(Candidate);
    }
    Base.bRunningCompanion=true; Base.bHasCompanionContext=true;
    Base.CompanionMode=TEXT("Follow"); Base.CompanionErrandAction=TEXT("FetchPackage");
    Base.CompanionRelationshipStage=TEXT("Acquainted"); Base.CompanionTaskTitle=TEXT("Actual task");
    Agent->BeginDecisionTrace(Base);
    const auto Bounded=Agent->GetDecisionTrace();
    TestEqual(TEXT("Newest five observations"),Bounded.Observations.Num(),5);
    TestTrue(TEXT("Newest observation first"),Bounded.Observations[0].Contains(TEXT("event-19")));
    TestTrue(TEXT("Actual observation position"),Bounded.Observations[0].Contains(TEXT("(19, 2, 3) cm")));
    TestEqual(TEXT("Six input subjects"),Bounded.Subjects.Num(),6);
    TestEqual(TEXT("Twelve input candidates"),Bounded.Candidates.Num(),12);
    TestTrue(TEXT("Bounded memory summaries"),Bounded.Memory.Num()<=4);
    TestTrue(TEXT("Name bounded"),Bounded.NpcName.Len()<=64);
    for (const auto& Text:Bounded.Observations) TestTrue(TEXT("Observation bounded"),Text.Len()<=160);
    TestTrue(TEXT("Companion activity uses request mode"),Bounded.CurrentActivity.Contains(TEXT("Follow")));
    TestFalse(TEXT("Unknown relative location omitted"),Bounded.Subjects[0].Contains(TEXT("cm")));
    const int32 InitialRevision=Bounded.Revision;
    Agent->TraceRequestEvent(FGuid::NewGuid(),Base.RequestId,TEXT("Failed"));
    Agent->TraceRequestEvent(Base.WorldEpoch,FGuid::NewGuid(),TEXT("Failed"));
    TestEqual(TEXT("Old world and request cannot update"),Agent->GetDecisionTrace().Revision,InitialRevision);

    // 真实 model_done 解析入口，不启动 HTTP、音频或模型
    Base.bRunningCompanion=false; Base.StreamProtocolVersion=2;
    auto AddRequest=[&](int32 Protocol)
    {
        auto Request=Base; Request.RequestId=FGuid::NewGuid(); Request.StreamProtocolVersion=Protocol;
        Settings->bEnabled=true; const bool Accepted=Gateway->Enqueue(Agent,Request); Settings->bEnabled=false;
        TestTrue(TEXT("Gateway accepted fixture"),Accepted);
        if (!Accepted) return FGuid();
        Agent->ActiveRequest=Request; Agent->bDecisionPending=true;
        auto& Reply=Agent->Replies.Add(Request.RequestId); Reply.ReplyId=FGuid::NewGuid();
        Gateway->Pending[Request.RequestId].bHasProposal=true;
        return Request.RequestId;
    };
    auto Event=[&](FGuid Id,const TCHAR* Type)
    {
        auto Json=MakeShared<FJsonObject>(); Json->SetStringField(TEXT("type"),Type);
        Json->SetStringField(TEXT("request_id"),Id.ToString(EGuidFormats::DigitsWithHyphensLower));
        Json->SetNumberField(TEXT("sentences"),0); return Json;
    };
    auto Send=[&](FGuid Id,const TSharedRef<FJsonObject>& Json)
    {
        FString Line; FJsonSerializer::Serialize(Json,TJsonWriterFactory<>::Create(&Line)); Gateway->ReadStream(Id,Line);
    };
    const FGuid First=AddRequest(2); if (!First.IsValid()) return false;
    Send(First,Event(First,TEXT("model_done")));
    TestEqual(TEXT("Missing metadata stays unknown"),Agent->GetDecisionTrace().BackendDecisionMs,-1.0);
    TestEqual(TEXT("Missing tokens stay unknown"),Agent->GetDecisionTrace().InputTokens,-1);
    TestTrue(TEXT("Missing model stays unknown"),Agent->GetDecisionTrace().ActualModelName.IsEmpty());
    TestTrue(TEXT("Missing model input counts are explicit"),Agent->GetDecisionTrace().InputSummary.Contains(TEXT("实际模型输入：未提供")));
    TestEqual(TEXT("Model completion is its own stage"),Agent->GetDecisionTrace().Stage,FName(TEXT("ModelDone")));
    TestFalse(TEXT("Model completion does not mean playback finished"),Agent->GetDecisionTrace().ReplyStage==TEXT("Finished"));
    TestEqual(TEXT("Transfer not yet done"),Agent->GetDecisionTrace().TotalElapsedMs,-1.0);

    const FGuid Second=AddRequest(2); if (!Second.IsValid()) return false;
    auto Complete=Event(Second,TEXT("model_done")); auto Metadata=MakeShared<FJsonObject>();
    Metadata->SetStringField(TEXT("model_provider"),TEXT("local")); Metadata->SetStringField(TEXT("model_name"),TEXT("Actual-Q8"));
    Metadata->SetStringField(TEXT("request_source"),TEXT("ambient")); Metadata->SetStringField(TEXT("decision_route"),TEXT("provider_dispatch"));
    Metadata->SetStringField(TEXT("fallback_reason"),TEXT("provider_timeout"));
    Metadata->SetNumberField(TEXT("queue_ms"),12.5); Metadata->SetNumberField(TEXT("decision_ms"),40.0);
    Metadata->SetNumberField(TEXT("backend_elapsed_ms"),52.5); Metadata->SetNumberField(TEXT("first_sentence_ms"),35.0);
    Metadata->SetNumberField(TEXT("input_tokens"),100); Metadata->SetNumberField(TEXT("cached_input_tokens"),64);
    Metadata->SetNumberField(TEXT("output_tokens"),10);
    Metadata->SetNumberField(TEXT("input_event_count"),3); Metadata->SetNumberField(TEXT("input_subject_count"),0);
    Metadata->SetNumberField(TEXT("input_candidate_count"),12); Complete->SetObjectField(TEXT("diagnostics"),Metadata);
    Send(Second,Complete);
    TestEqual(TEXT("Actual backend model"),Agent->GetDecisionTrace().ActualModelName,FString(TEXT("Actual-Q8")));
    TestEqual(TEXT("Actual queue timing"),Agent->GetDecisionTrace().BackendQueueMs,12.5);
    TestEqual(TEXT("Actual first sentence timing"),Agent->GetDecisionTrace().FirstSentenceMs,35.0);
    TestEqual(TEXT("Actual cache token count"),Agent->GetDecisionTrace().CachedInputTokens,64);
    TestTrue(TEXT("Game request count remains distinct"),Agent->GetDecisionTrace().InputSummary.Contains(TEXT("游戏请求记录含 20 条感知")));
    TestTrue(TEXT("Measured model counts include real zero"),Agent->GetDecisionTrace().InputSummary.Contains(TEXT("实际模型输入：事件 3、对象 0、动作候选 12")));
    const int32 CurrentRevision=Agent->GetDecisionTrace().Revision;
    Send(First,Event(First,TEXT("done")));
    TestEqual(TEXT("Late old transfer cannot replace new snapshot"),Agent->GetDecisionTrace().Revision,CurrentRevision);
    Agent->PublicState.Action=EDeliveryNpcAction::Fight; Agent->PublicState.Status=EDeliveryNpcActionStatus::Running;
    Agent->DecisionTrace.ProposedAction=TEXT("ContinueCurrent");
    Agent->TraceValidation(Base.WorldEpoch,Second,true,NAME_None);
    TestTrue(TEXT("Continue is distinct from restarting fight"),Agent->GetDecisionTrace().ValidatedAction.Contains(TEXT("ContinueCurrent")));

    const FGuid Third=AddRequest(2); if (!Third.IsValid()) return false;
    auto Invalid=Event(Third,TEXT("model_done")); auto Unsafe=MakeShared<FJsonObject>();
    Unsafe->SetStringField(TEXT("model_name"),TEXT("C:/private/model.bin")); Unsafe->SetStringField(TEXT("decision_route"),TEXT("unknown"));
    Unsafe->SetNumberField(TEXT("decision_ms"),-2); Unsafe->SetNumberField(TEXT("input_tokens"),1.5);
    Unsafe->SetNumberField(TEXT("input_event_count"),-1); Unsafe->SetNumberField(TEXT("input_subject_count"),1.5);
    Invalid->SetObjectField(TEXT("diagnostics"),Unsafe); Send(Third,Invalid);
    TestTrue(TEXT("Path metadata excluded"),Agent->GetDecisionTrace().ActualModelName.IsEmpty());
    TestTrue(TEXT("Unknown route excluded"),Agent->GetDecisionTrace().Route.IsEmpty());
    TestEqual(TEXT("Invalid timing remains unknown"),Agent->GetDecisionTrace().BackendDecisionMs,-1.0);
    TestEqual(TEXT("Fractional count remains unknown"),Agent->GetDecisionTrace().InputTokens,-1);
    TestTrue(TEXT("Invalid and absent input counts remain unknown"),Agent->GetDecisionTrace().InputSummary.Contains(TEXT("实际模型输入：事件 未提供、对象 未提供、动作候选 未提供")));
    TestTrue(TEXT("Optional invalid metadata does not reject model_done"),Gateway->Pending[Third].bModelFinished);
    Gateway->Cancel(Agent); Agent->Replies.Reset(); Agent->ActiveRequest={};
    const FGuid Legacy=AddRequest(0); if (!Legacy.IsValid()) return false;
    Send(Legacy,Event(Legacy,TEXT("done")));
    TestTrue(TEXT("Legacy done still accepted"),Gateway->Pending[Legacy].bReplyFinished);
    TestEqual(TEXT("Legacy diagnostics remain unknown"),Agent->GetDecisionTrace().BackendElapsedMs,-1.0);
    TestTrue(TEXT("Actual transfer completion measured"),Agent->GetDecisionTrace().TotalElapsedMs>=0);
    return true;
}
#endif
