#include "Misc/AutomationTest.h"
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/ScopeExit.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/WorldSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcConversationOcclusionTest, "Delivery.AI.ConversationOcclusion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryNpcConversationOcclusionTest::RunTest(const FString&)
{
    auto* Settings = GetMutableDefault<UDeliveryAiSettings>();
    const bool bWasEnabled = Settings->bEnabled;
    const bool bWasStreaming = Settings->bStreamReplies;
    Settings->bEnabled = false;
    Settings->bStreamReplies = true;
    ON_SCOPE_EXIT
    {
        Settings->bEnabled = bWasEnabled;
        Settings->bStreamReplies = bWasStreaming;
    };

    const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
    if (!TestNotNull(TEXT("创建带物理查询的交谈测试世界"), World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit);
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
    };

    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Speaker = World->SpawnActor<ADeliveryNpcCharacter>(FVector(0, 0, 90), FRotator::ZeroRotator, Spawn);
    auto* Neighbor = World->SpawnActor<ADeliveryNpcCharacter>(FVector(240, 0, 90), FRotator::ZeroRotator, Spawn);
    if (!TestNotNull(TEXT("创建说话的居民"), Speaker) || !TestNotNull(TEXT("创建接话的居民"), Neighbor)) return false;
    Speaker->DisplayName = FText::FromString(TEXT("大壮"));
    Neighbor->DisplayName = FText::FromString(TEXT("翠花"));
    for (auto* Npc : {Speaker, Neighbor})
    {
        // 角色保持位置，Agent 和表现组件仍由测试世界正常更新
        Npc->SetActorTickEnabled(false);
        Npc->GetCharacterMovement()->DisableMovement();
        if (!Npc->GetController()) Npc->SpawnDefaultController();
    }
    if (!TestNotNull(TEXT("说话者拥有 AI 控制器"), Speaker->GetController())) return false;

    auto* PassingPawn = World->SpawnActor<APawn>();
    if (!TestNotNull(TEXT("创建从两人中间经过的角色"), PassingPawn)) return false;
    auto* PassingCapsule = NewObject<UCapsuleComponent>(PassingPawn);
    PassingPawn->SetRootComponent(PassingCapsule);
    PassingCapsule->InitCapsuleSize(35, 100);
    PassingCapsule->SetCollisionProfileName(TEXT("BlockAll"));
    PassingCapsule->SetCollisionObjectType(ECC_Pawn);
    PassingCapsule->RegisterComponent();
    PassingPawn->SetActorLocation(FVector(120, 400, 90));

    auto* Agent = Speaker->Agent.Get();
    auto* Presentation = Speaker->Presentation.Get();
    const FGuid Epoch = FGuid::NewGuid();
    TSet<FString> DisplayedSentences;
    const auto Tick = [&](float Seconds)
    {
        for (int32 Index = 0; Index < FMath::CeilToInt(Seconds / .02f); ++Index)
        {
            ++GFrameCounter;
            World->Tick(LEVELTICK_All, .02f);
            const auto Speech = Presentation->GetSpeech();
            if (Speech.bActive) DisplayedSentences.Add(Speech.Text);
        }
    };
    const auto BeginReply = [&](EDeliveryNpcAction Action, AActor* Target, FDeliveryNpcDecisionProposal& Proposal)
    {
        Agent->CancelPlan(TEXT("NextConversationOcclusionCase"));
        if (Target)
            Speaker->ObserveFact(TEXT("SeenNpc"), Target, TEXT("对方正在附近"), EDeliveryNpcEvidenceSource::Seen);
        FDeliveryNpcDecisionRequest Request;
        if (!Agent->BuildDecisionRequest(Epoch, Request)) return false;
        const auto* Candidate = Request.Candidates.FindByPredicate([&](const auto& Item)
        {
            return Item.Action == Action && (Target
                ? Agent->KnownSubjects.FindRef(Item.TargetId).Get() == Target : !Item.TargetId.IsValid());
        });
        if (!Candidate) return false;
        // 直接提供经过协议检查的模型结果，测试期间始终禁止发送 HTTP 请求
        Request.RequestId = FGuid::NewGuid();
        Agent->ActiveRequest = Request;
        Agent->bDecisionPending = true;
        Proposal = {};
        Proposal.WorldEpoch = Request.WorldEpoch;
        Proposal.NpcId = Request.NpcId;
        Proposal.RequestId = Request.RequestId;
        Proposal.ContextRevision = Request.ContextRevision;
        Proposal.PlanRevision = Request.PlanRevision;
        Proposal.Action = Candidate->Action;
        Proposal.TargetId = Candidate->TargetId;
        Proposal.EvidenceIds = Candidate->EvidenceIds;
        Proposal.Duration = 5;
        Proposal.Speech = TEXT("翠花，听见没？我有话说。");
        Proposal.Emotion = TEXT("Calm");
        Proposal.bHasSpeechTarget = Target != nullptr;
        Proposal.SpeechTargetId = Candidate->TargetId;
        if (!Agent->ReceiveStreamDecision(Request.RequestId, Proposal)) return false;
        FDeliveryNpcPublicState RunningAction;
        return Agent->StartAction(RunningAction) && Speaker->ExecuteAction(RunningAction);
    };

    FDeliveryNpcDecisionProposal Reply;
    if (!TestTrue(TEXT("先开始一次看向邻居的流式回复"), BeginReply(EDeliveryNpcAction::LookAt, Neighbor, Reply))) return false;
    PassingPawn->SetActorLocation(FVector(120, 0, 90));
    FHitResult RawHit;
    FCollisionQueryParams RawParams(SCENE_QUERY_STAT(DeliveryNpcConversationOcclusionTest), false, Speaker);
    const FVector Offset(0, 0, 40);
    const bool bRawBlocked = World->LineTraceSingleByChannel(RawHit, Speaker->GetActorLocation() + Offset,
        Neighbor->GetActorLocation() + Offset, ECC_Visibility, RawParams);
    if (!TestTrue(TEXT("原始 Visibility 查询确实先撞到经过的人"), bRawBlocked && RawHit.GetActor() == PassingPawn)) return false;
    TestTrue(TEXT("交谈视线允许普通角色从中间经过"), Agent->CanLookAt(Neighbor));

    // 模拟模型先选好动作，首句台词稍后才到达
    Tick(.8f);
    TestEqual(TEXT("第三人经过期间 LookAt 仍在执行"), Agent->GetPublicState().Status, EDeliveryNpcActionStatus::Running);
    TestTrue(TEXT("第三人没有清掉待播的回复"), Presentation->HasPendingSpeech());
    FDeliveryNpcSentence First;
    First.Sequence = 0;
    First.Text = TEXT("翠花，听见没？");
    First.Emotion = TEXT("Calm");
    FDeliveryNpcSentence Second = First;
    Second.Sequence = 1;
    Second.Text = TEXT("我有话说。");
    // 本测试按无音频世界的播放时间推进；PCM 编码另由 SentencePlayback 覆盖
    Agent->ReceiveSentence(Reply.RequestId, First);
    Agent->ReceiveSentence(Reply.RequestId, Second);
    Agent->FinishReply(Reply.RequestId, true, 2);
    Tick(3.3f);
    TestTrue(TEXT("迟到的第一句经过正常播放流程"), DisplayedSentences.Contains(First.Text));
    TestTrue(TEXT("第二句也完整进入播放流程"), DisplayedSentences.Contains(Second.Text));
    TestFalse(TEXT("整轮台词播完后不再等待"), Presentation->HasPendingSpeech());
    const FString CompleteText = First.Text + Second.Text;
    const auto* Spoken = Agent->Evidence.FindByPredicate([&](const auto& Item)
    {
        return Item.Kind == TEXT("SelfSpeech") && Item.Description == CompleteText;
    });
    if (!TestNotNull(TEXT("说话者记住的是完整两句台词"), Spoken)) return false;
    const auto* Heard = Neighbor->Agent->Evidence.FindByPredicate([&](const auto& Item)
    {
        return Item.EventId == Spoken->EventId && Item.Kind == TEXT("Speech") && Item.Description == CompleteText;
    });
    if (!TestNotNull(TEXT("隔着第三人仍把整轮台词交给邻居接话"), Heard)) return false;
    TestEqual(TEXT("邻居记录自己为收话人"), Heard->AddresseeId, Neighbor->Agent->GetNpcId());
    TestEqual(TEXT("邻居进入下一轮交谈"), Neighbor->GetConversationDepth(), 1);
    FDeliveryNpcDecisionRequest Followup;
    if (!TestTrue(TEXT("邻居可以准备后续模型上下文"), Neighbor->Agent->BuildDecisionRequest(Epoch, Followup))) return false;
    TestTrue(TEXT("后续上下文包含刚听完的整轮台词"), Followup.Evidence.ContainsByPredicate([&](const auto& Item)
    {
        return Item.EventId == Spoken->EventId && Item.Kind == TEXT("Speech") && Item.Description == CompleteText;
    }));
    TestTrue(TEXT("上下文中的说话者仍是大壮"), Followup.Subjects.ContainsByPredicate([&](const auto& Item)
    {
        return Item.SubjectId == Heard->SubjectId && Item.Name == TEXT("大壮");
    }));
    TestTrue(TEXT("本轮 LookAt 可以正常结束"),
        Agent->TryFinishAction(Agent->GetPublicState().ActionId, EDeliveryNpcActionStatus::Succeeded, NAME_None));

    // 墙放在经过的人后面，防止忽略第一个碰撞后错误地放过后方墙体
    auto* Wall = World->SpawnActor<AActor>();
    auto* WallBox = NewObject<UBoxComponent>(Wall);
    Wall->SetRootComponent(WallBox);
    WallBox->SetBoxExtent(FVector(10, 100, 150));
    WallBox->SetCollisionProfileName(TEXT("BlockAll"));
    WallBox->RegisterComponent();
    Wall->SetActorLocation(FVector(175, 0, 90));
    TestFalse(TEXT("第三人后方的墙仍阻挡交谈视线"), Agent->CanLookAt(Neighbor));
    TestFalse(TEXT("墙后的邻居不会收到新的台词"),
        Neighbor->HearSpeech(Speaker, TEXT("这句话隔着墙"), FGuid::NewGuid(), 1, Neighbor));
    WallBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    if (!TestTrue(TEXT("移开墙后能开始下一轮"), BeginReply(EDeliveryNpcAction::LookAt, Neighbor, Reply))) return false;
    WallBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    Tick(.2f);
    TestEqual(TEXT("真正的墙体阻挡会结束 LookAt"), Agent->GetPublicState().Status, EDeliveryNpcActionStatus::Cancelled);
    TestEqual(TEXT("墙体阻挡保留目标不可见原因"), Agent->GetPublicState().Reason, FName(TEXT("TargetUnavailable")));
    TestFalse(TEXT("墙体中断后停止待播回复"), Presentation->HasPendingSpeech());
    WallBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Wall->Destroy();

    if (!TestTrue(TEXT("准备目标即将移除的回复"), BeginReply(EDeliveryNpcAction::LookAt, Neighbor, Reply))) return false;
    Neighbor->Destroy();
    Tick(.2f);
    TestEqual(TEXT("目标移除后明确取消动作"), Agent->GetPublicState().Status, EDeliveryNpcActionStatus::Cancelled);
    TestEqual(TEXT("目标移除记录目标失效"), Agent->GetPublicState().Reason, FName(TEXT("TargetUnavailable")));
    TestFalse(TEXT("目标移除后不保留旧回复"), Presentation->HasPendingSpeech());

    if (!TestTrue(TEXT("准备独立的新回复来检查受击"), BeginReply(EDeliveryNpcAction::Wait, nullptr, Reply))) return false;
    Speaker->ReceiveImpactFrom(FVector(200, 0, 0), Speaker->GetActorLocation(), PassingPawn);
    TestEqual(TEXT("受击明确取消正在执行的动作"), Agent->GetPublicState().Status, EDeliveryNpcActionStatus::Cancelled);
    TestEqual(TEXT("受击保留 Hit 原因"), Agent->GetPublicState().Reason, FName(TEXT("Hit")));
    TestFalse(TEXT("受击仍会停止全部待播台词"), Presentation->HasPendingSpeech());
    Agent->ReceiveSentence(Reply.RequestId, First);
    Agent->FinishReply(Reply.RequestId, true, 1);
    TestFalse(TEXT("受击后晚到的句子不能恢复旧回复"), Presentation->HasPendingSpeech());
    TestFalse(TEXT("整个测试始终没有启用模型请求"), Settings->bEnabled);
    return true;
}
#endif
