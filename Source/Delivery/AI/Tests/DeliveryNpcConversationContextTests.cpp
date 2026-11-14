#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Characters/DeliveryNpcAIController.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimSequence.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "Perception/AISense_Hearing.h"
#include "Perception/AISense_Sight.h"
#include "Perception/AISense_Touch.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcConversationContextTest, "Delivery.AI.ConversationContext",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryNpcConversationContextTest::RunTest(const FString&)
{
    auto* Settings = GetMutableDefault<UDeliveryAiSettings>();
    const bool bWasEnabled = Settings->bEnabled;
    Settings->bEnabled = false;
    const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit);
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        Settings->bEnabled = bWasEnabled;
    };

    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Dajun = World->SpawnActor<ADeliveryNpcCharacter>(FVector(0, 0, 90), FRotator::ZeroRotator, Spawn);
    auto* Xiaofang = World->SpawnActor<ADeliveryNpcCharacter>(FVector(240, 0, 90), FRotator::ZeroRotator, Spawn);
    auto* Neighbor = World->SpawnActor<ADeliveryNpcCharacter>(FVector(0, 240, 90), FRotator::ZeroRotator, Spawn);
    auto* Player = World->SpawnActor<APawn>();
    auto* PlayerRoot = NewObject<USceneComponent>(Player);
    Player->SetRootComponent(PlayerRoot);
    PlayerRoot->RegisterComponent();
    Player->SetActorLocation(FVector(240, 240, 90));
    auto* Controller = World->SpawnActor<APlayerController>();
    // 独立测试世界没有 GameMode 创建 PlayerState，补齐真实玩家判定需要的数据
    Controller->PlayerState = World->SpawnActor<APlayerState>();
    Controller->PlayerState->SetIsABot(false);
    Controller->Possess(Player);
    if (!TestTrue(TEXT("测试发言者具有正常玩家身份"), Player->IsPlayerControlled())) return false;
    Dajun->DisplayName = FText::FromString(TEXT("大军"));
    Xiaofang->DisplayName = FText::FromString(TEXT("小芳"));
    Neighbor->DisplayName = FText::FromString(TEXT("邻居"));

    // 同一句玩家发言，对话对象和旁听者保存不同的接收方式
    const FGuid PlayerMessage = FGuid::NewGuid();
    const FString PlayerText = TEXT("小芳说你就会吹牛，你服不服？");
    TestTrue(TEXT("大军收到玩家直接说的话"), Dajun->HearSpeech(Player, PlayerText, PlayerMessage, 0, Dajun));
    TestTrue(TEXT("小芳听见玩家对大军说的话"), Xiaofang->HearSpeech(Player, PlayerText, PlayerMessage, 0, Dajun));
    TestTrue(TEXT("对话对象开始倾听玩家"), Dajun->Agent->IsListening());
    TestFalse(TEXT("旁听不冒充玩家正在与自己交谈"), Xiaofang->Agent->IsListening());

    FDeliveryNpcDecisionRequest DirectRequest, OverheardRequest;
    const FGuid Epoch = FGuid::NewGuid();
    if (!TestTrue(TEXT("准备直接对话的上下文"), Dajun->Agent->BuildDecisionRequest(Epoch, DirectRequest)) ||
        !TestTrue(TEXT("准备旁听者的上下文"), Xiaofang->Agent->BuildDecisionRequest(Epoch, OverheardRequest))) return false;

    const auto* Direct = DirectRequest.Evidence.FindByPredicate([&](const auto& Item) { return Item.EventId == PlayerMessage; });
    const auto* Overheard = OverheardRequest.Evidence.FindByPredicate([&](const auto& Item) { return Item.EventId == PlayerMessage; });
    if (!TestNotNull(TEXT("直接对话保留原发言"), Direct) || !TestNotNull(TEXT("旁听保留同一条原发言"), Overheard)) return false;
    TestEqual(TEXT("直接向我说话标为玩家发言"), Direct->Kind, FName(TEXT("PlayerSpeech")));
    TestTrue(TEXT("首次请求没有已采纳决策读过的事件"), DirectRequest.LastDecisionEventIds.IsEmpty());
    TestEqual(TEXT("说给别人听的话标为旁听"), Overheard->Kind, FName(TEXT("OverheardSpeech")));
    TestEqual(TEXT("原话保持不变，没有改写成已证实的事实"), Overheard->Description, PlayerText);
    TestEqual(TEXT("请求明确记录自己叫大军"), DirectRequest.SelfName, FString(TEXT("大军")));
    TestTrue(TEXT("自己在人物表中有有效编号"), DirectRequest.SelfSubjectId.IsValid());
    TestEqual(TEXT("直接对话的收话人是自己"), Direct->AddresseeId, DirectRequest.SelfSubjectId);
    const auto* NamedPlayer = DirectRequest.Subjects.FindByPredicate([&](const auto& Item)
        { return Item.SubjectId == Direct->SubjectId; });
    if (!TestNotNull(TEXT("人物表包含当前说话的玩家"), NamedPlayer)) return false;
    TestEqual(TEXT("模型能分清快递员与 NPC"), NamedPlayer->Kind, FString(TEXT("player")));

    const auto* NamedDajun = OverheardRequest.Subjects.FindByPredicate([&](const auto& Item)
        { return Item.SubjectId == Overheard->AddresseeId; });
    if (!TestNotNull(TEXT("旁听上下文能够找到真正收话的人"), NamedDajun)) return false;
    TestEqual(TEXT("旁听者知道玩家这句话说给大军听"), NamedDajun->Name, FString(TEXT("大军")));
    TestNotEqual(TEXT("旁听者不会把收话人编号当作自己"), Overheard->AddresseeId, OverheardRequest.SelfSubjectId);

    // 检查真正发送给后端的 JSON，避免 C++ 保存了信息却没有写入请求
    TSharedPtr<FJsonObject> Json;
    if (!TestTrue(TEXT("交谈上下文能够编码为 JSON"),
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(DeliveryNpc::EncodeRequest(OverheardRequest)), Json))) return false;
    TestEqual(TEXT("JSON 保留 NPC 姓名"), Json->GetStringField(TEXT("self_name")), OverheardRequest.SelfName);
    TestEqual(TEXT("JSON 保留自己的对象编号"), Json->GetStringField(TEXT("self_subject_id")),
        OverheardRequest.SelfSubjectId.ToString(EGuidFormats::DigitsWithHyphensLower));
    const TArray<TSharedPtr<FJsonValue>>* Subjects = nullptr;
    if (!TestTrue(TEXT("JSON 包含人物表"), Json->TryGetArrayField(TEXT("subjects"), Subjects))) return false;
    bool bSerializedDajun = false;
    for (const auto& Value : *Subjects)
    {
        const auto& Subject = Value->AsObject();
        if (Subject->GetStringField(TEXT("subject_id")) != Overheard->AddresseeId.ToString(EGuidFormats::DigitsWithHyphensLower)) continue;
        bSerializedDajun = true;
        TestEqual(TEXT("人物表保留大军的名字"), Subject->GetStringField(TEXT("name")), NamedDajun->Name);
        TestFalse(TEXT("人物表说明对象类型"), Subject->GetStringField(TEXT("kind")).IsEmpty());
    }
    TestTrue(TEXT("后端请求确实包含收话人的资料"), bSerializedDajun);
    const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
    if (!TestTrue(TEXT("JSON 包含听到的发言"), Json->TryGetArrayField(TEXT("evidence"), Events))) return false;
    bool bSerializedMessage = false;
    for (const auto& Value : *Events)
    {
        const auto& Event = Value->AsObject();
        if (Event->GetStringField(TEXT("event_id")) != PlayerMessage.ToString(EGuidFormats::DigitsWithHyphensLower)) continue;
        bSerializedMessage = true;
        TestEqual(TEXT("JSON 保留说话者"), Event->GetStringField(TEXT("subject_id")),
            Overheard->SubjectId.ToString(EGuidFormats::DigitsWithHyphensLower));
        TestEqual(TEXT("JSON 保留收话人"), Event->GetStringField(TEXT("addressee_id")),
            Overheard->AddresseeId.ToString(EGuidFormats::DigitsWithHyphensLower));
    }
    TestTrue(TEXT("后端请求确实包含这句原话"), bSerializedMessage);

    auto PrepareReply = [&](FDeliveryNpcDecisionProposal& Proposal)
    {
        Dajun->Agent->CancelPlan(TEXT("NextContextCheck"));
        FDeliveryNpcDecisionRequest Request;
        if (!Dajun->Agent->BuildDecisionRequest(Epoch, Request)) return false;
        Dajun->Agent->ActiveRequest = Request;
        Proposal = {};
        Proposal.WorldEpoch = Request.WorldEpoch;
        Proposal.NpcId = Request.NpcId;
        Proposal.RequestId = Request.RequestId;
        Proposal.ContextRevision = Request.ContextRevision;
        Proposal.PlanRevision = Request.PlanRevision;
        Proposal.Action = EDeliveryNpcAction::Wait;
        Proposal.Duration = 1;
        Proposal.Speech = TEXT("这话我可没说。");
        return true;
    };

    FDeliveryNpcDecisionProposal Proposal;
    if (!TestTrue(TEXT("准备等待期间收到旁听消息的请求"), PrepareReply(Proposal))) return false;
    TestTrue(TEXT("等待回复时仍能听见别人交谈"),
        Dajun->HearSpeech(Neighbor, TEXT("小芳，刚才是谁在说话？"), FGuid::NewGuid(), 1, Xiaofang));
    FName Reason;
    TestTrue(TEXT("旁听消息不会丢弃正在回答玩家的回复"), Dajun->Agent->TryAcceptProposal(Proposal, Reason));

    if (!TestTrue(TEXT("准备等待期间收到新问题的请求"), PrepareReply(Proposal))) return false;
    const FGuid NewDirectMessage = FGuid::NewGuid();
    const TArray<FGuid> EventsBeforeRejectedReply=Dajun->Agent->LastDecisionEventIds;
    TestTrue(TEXT("NPC 向我说的新一句能够被接收"),
        Dajun->HearSpeech(Xiaofang, TEXT("大军，我可没说你，别听他瞎说。"), NewDirectMessage, 1, Dajun));
    TestFalse(TEXT("新一句直接对话到达后，旧请求不能继续答上一句"), Dajun->Agent->TryAcceptProposal(Proposal, Reason));
    TestEqual(TEXT("旧回复注明需要更新对话内容"), Reason, FName(TEXT("StaleProposal")));
    TestTrue(TEXT("被拒绝的回复不能把新话标为已经看过"),
        Dajun->Agent->LastDecisionEventIds==EventsBeforeRejectedReply);
    FDeliveryNpcDecisionRequest UpdatedRequest;
    TestTrue(TEXT("可以准备包含新一句的后续请求"), Dajun->Agent->BuildDecisionRequest(Epoch, UpdatedRequest));
    TestTrue(TEXT("后续请求保留新的直接发言"), UpdatedRequest.Evidence.ContainsByPredicate([&](const auto& Item)
        { return Item.EventId == NewDirectMessage && Item.Kind == TEXT("Speech") && Item.AddresseeId == UpdatedRequest.SelfSubjectId; }));

    const FGuid LegacyMessage = FGuid::NewGuid();
    TestTrue(TEXT("原有调用省略收话人时仍可交谈"), Dajun->HearSpeech(Xiaofang, TEXT("大军，你听见没有？"), LegacyMessage, 1));
    TestTrue(TEXT("省略收话人默认说给当前 NPC"), Dajun->Agent->Evidence.ContainsByPredicate([&](const auto& Item)
        { return Item.EventId == LegacyMessage && Item.Kind == TEXT("Speech") && Item.AddresseeId == UpdatedRequest.SelfSubjectId; }));

    // 模型还没返回动作建议时听到旁人接话，也要等当前回复结束后处理
    if (!TestTrue(TEXT("准备尚未收到建议的流式请求"), PrepareReply(Proposal))) return false;
    Dajun->Agent->bDecisionPending = true;
    const int32 RequestedDepth=Dajun->Agent->ActiveRequest.ConversationDepth;
    const FGuid MessageBeforeProposal=FGuid::NewGuid();
    TestTrue(TEXT("生成动作建议期间接收到旁人的新话"),
        Dajun->HearSpeech(Neighbor, TEXT("小芳，你刚才说他不敢来？"), MessageBeforeProposal, 3, Xiaofang));
    TestNotEqual(TEXT("后来听到的话属于另一轮接话"),Dajun->GetConversationDepth(),RequestedDepth);
    TestTrue(TEXT("新话已安排下一轮请求"), Dajun->Agent->bDecisionQueued);
    if (!TestTrue(TEXT("旁听不打断正在生成的回复"),
        Dajun->Agent->ReceiveStreamDecision(Proposal.RequestId, Proposal))) return false;
    TestTrue(TEXT("收到动作建议仍保留下一轮接话"), Dajun->Agent->bDecisionQueued);
    TestEqual(TEXT("本次回复沿用请求发出时的轮数"),Dajun->Agent->GetActionConversationDepth(),RequestedDepth);
    TestFalse(TEXT("生成期间收到的话尚未被本次决策看过"),
        Dajun->Agent->LastDecisionEventIds.Contains(MessageBeforeProposal));
    TestTrue(TEXT("已采纳决策记住了此前收到的直接发言"),Dajun->Agent->LastDecisionEventIds.Contains(LegacyMessage));
    Dajun->Agent->FinishReply(Proposal.RequestId, true, 0);
    TestTrue(TEXT("本轮生成结束后仍处理生成期间的新话"), Dajun->bDecisionNeeded);
    TestFalse(TEXT("下一轮请求安排只取出一次"), Dajun->Agent->bDecisionQueued);

    Dajun->Agent->CancelPlan(TEXT("CheckNextConversationRequest"));
    FDeliveryNpcDecisionRequest NextConversationRequest;
    if (!TestTrue(TEXT("准备包含待接话内容的下一轮请求"),
        Dajun->Agent->BuildDecisionRequest(Epoch,NextConversationRequest))) return false;
    TestTrue(TEXT("取消旧动作不抹掉上次模型看过的事件"),NextConversationRequest.LastDecisionEventIds.Contains(LegacyMessage));
    TestFalse(TEXT("下一轮仍能分清模型没看过的新话"),NextConversationRequest.LastDecisionEventIds.Contains(MessageBeforeProposal));
    TSharedPtr<FJsonObject> NextConversationJson;
    if (!TestTrue(TEXT("新事件记录可以随请求编码"),FJsonSerializer::Deserialize(
        TJsonReaderFactory<>::Create(DeliveryNpc::EncodeRequest(NextConversationRequest)),NextConversationJson))) return false;
    const TArray<TSharedPtr<FJsonValue>>* LastDecisionEvents=nullptr;
    if (!TestTrue(TEXT("JSON 包含上次决策读过的事件编号"),
        NextConversationJson->TryGetArrayField(TEXT("last_decision_event_ids"),LastDecisionEvents))) return false;
    TestEqual(TEXT("编码保留全部已读取编号"),LastDecisionEvents->Num(),NextConversationRequest.LastDecisionEventIds.Num());
    TestFalse(TEXT("JSON 不会把刚收到的话误记为已读取"),LastDecisionEvents->ContainsByPredicate([&](const auto& Value)
        { return Value->AsString()==MessageBeforeProposal.ToString(EGuidFormats::DigitsWithHyphensLower); }));

    // 完整回复接口与流式回复保持相同规则
    if (!TestTrue(TEXT("准备完整回复请求"), PrepareReply(Proposal))) return false;
    Dajun->Agent->bDecisionPending = true;
    TestTrue(TEXT("完整回复等待期间接收到旁人的新话"),
        Dajun->HearSpeech(Neighbor, TEXT("小芳，你刚才没说完。"), FGuid::NewGuid(), 1, Xiaofang));
    Dajun->Agent->ReceiveDecision(Proposal.RequestId, &Proposal, NAME_None);
    TestEqual(TEXT("当前完整回复可以正常开始"), Dajun->Agent->GetPublicState().Status, EDeliveryNpcActionStatus::Accepted);
    TestTrue(TEXT("完整回复成功也保留下一轮接话"), Dajun->bDecisionNeeded);
    TestFalse(TEXT("完整回复不会重复保留旧请求安排"), Dajun->Agent->bDecisionQueued);

    // 已开始回答后再听到同伴接话，完成当前生成时仍要保留下一轮回答
    if (!TestTrue(TEXT("准备已经开始的流式回复"), PrepareReply(Proposal))) return false;
    Dajun->Agent->bDecisionPending = true;
    if (!TestTrue(TEXT("接收流式回复中的动作建议"),
        Dajun->Agent->ReceiveStreamDecision(Proposal.RequestId, Proposal))) return false;
    FDeliveryNpcPublicState RunningAction;
    if (!TestTrue(TEXT("开始本轮回复的动作"), Dajun->Agent->StartAction(RunningAction)) ||
        !TestTrue(TEXT("NPC 执行本轮回复的动作"), Dajun->ExecuteAction(RunningAction))) return false;
    const FGuid MessageDuringReply = FGuid::NewGuid();
    TestTrue(TEXT("回复已经开始后仍可接收同伴的新一句"),
        Dajun->HearSpeech(Xiaofang, TEXT("你先听我说，刚才那句话不是那个意思。"), MessageDuringReply, 2, Dajun));
    TestTrue(TEXT("动作执行期间收到新话也会安排下一次请求"), Dajun->Agent->bDecisionQueued);
    Dajun->Agent->FinishReply(Proposal.RequestId, true, 0);
    TestFalse(TEXT("本轮生成完成后解除请求占用"), Dajun->Agent->IsDecisionPending());
    TestEqual(TEXT("同伴接话没有强行取消当前动作"), Dajun->Agent->GetPublicState().ActionId, RunningAction.ActionId);
    TestTrue(TEXT("生成完成通知不会清掉下一轮回答"), Dajun->bDecisionNeeded);
    TestTrue(TEXT("下一轮回答仍能找到刚收到的原话"), Dajun->Agent->Evidence.ContainsByPredicate([&](const auto& Item)
        { return Item.EventId == MessageDuringReply && Item.Kind == TEXT("Speech"); }));

    // 连续交谈两分钟后转向玩家，邻居和刚才的答话仍应留在记忆里
    auto* Memory=Dajun->Agent.Get();
    Memory->CancelPlan(TEXT("MemoryRetentionCheck"));
    Memory->Evidence.Reset(); Memory->KnownSubjects.Reset(); Memory->RecentEventIds.Reset();
    // 提供真实出拳所需的骨架与动画，裸 C++ 角色没有这些蓝图资源
    auto* StrikeMesh=LoadObject<USkeletalMesh>(nullptr,TEXT("/Game/NPC/Residents/Appearance/SK_NpcManFace.SK_NpcManFace"));
    auto* StrikeAnimation=LoadObject<UAnimSequence>(nullptr,TEXT("/Game/NPC/Residents/Animation/A_ResidentPunch.A_ResidentPunch"));
    if (!TestNotNull(TEXT("记忆测试使用正式居民骨架"),StrikeMesh) ||
        !TestNotNull(TEXT("记忆测试使用正式出拳动画"),StrikeAnimation)) return false;
    Dajun->GetMesh()->SetSkeletalMesh(StrikeMesh);
    Dajun->Presentation->AttackAnimation=StrikeAnimation;
    Xiaofang->SetActorLocation(FVector(120,0,90));
    FDeliveryNpcObservation Sight;
    Sight.EventId=FGuid::NewGuid(); Sight.Kind=TEXT("VisibleNow"); Sight.Source=EDeliveryNpcEvidenceSource::Seen;
    Sight.Subject=Xiaofang; Sight.Description=TEXT("小芳仍在视线中，距离约 120 厘米");
    Sight.bHasObservedLocation=true; Sight.ObservedLocation=Xiaofang->GetActorLocation(); Sight.Lifetime=45;
    TestTrue(TEXT("记下邻居出现的位置"),Memory->ObserveEvent(Sight));
    const FGuid FirstSight=Sight.EventId;
    TArray<FGuid> ConversationIds;
    for (const FName Kind : {FName(TEXT("PlayerSpeech")),FName(TEXT("Speech")),FName(TEXT("OverheardSpeech")),
        FName(TEXT("SelfSpeech")),FName(TEXT("ActionReply"))})
    {
        FDeliveryNpcObservation Line;
        Line.EventId=FGuid::NewGuid(); Line.Kind=Kind;
        Line.Source=Kind==TEXT("SelfSpeech") || Kind==TEXT("ActionReply")?EDeliveryNpcEvidenceSource::ActionResult:EDeliveryNpcEvidenceSource::Heard;
        Line.Subject=Kind==TEXT("PlayerSpeech")?Player:Kind==TEXT("SelfSpeech") || Kind==TEXT("ActionReply")?Dajun:Xiaofang;
        Line.Addressee=Dajun; Line.Description=TEXT("刚才说的是谁先认怂，别装作没听见。"); Line.Lifetime=45;
        ConversationIds.Add(Line.EventId);
        TestTrue(TEXT("记录交谈中实际发生的话与回应"),Memory->ObserveEvent(Line));
    }
    FDeliveryNpcDecisionRequest RememberedRequest;
    TestTrue(TEXT("准备含邻居和交谈的请求"),Memory->BuildDecisionRequest(Epoch,RememberedRequest));
    Memory->ActiveRequest=RememberedRequest;
    const FGuid NeighborId=Memory->Evidence[0].SubjectId;
    for (int32 Refresh=0; Refresh<4; ++Refresh)
    {
        Sight.EventId=FGuid::NewGuid();
        Memory->ObserveEvent(Sight);
    }
    TestTrue(TEXT("视野刷新保留当前请求引用的原编号"),Memory->Evidence.ContainsByPredicate(
        [&](const auto& Item) { return Item.EventId==FirstSight; }));
    TestEqual(TEXT("请求期间只额外保留一条最新视野记录"),Memory->Evidence.FilterByPredicate(
        [](const auto& Item) { return Item.Kind==TEXT("VisibleNow"); }).Num(),2);
    Memory->CancelDecision();
    for (int32 Interval=0; Interval<8; ++Interval)
    {
        // 只推进这个独立测试世界的记忆时钟，不让无地面的角色连续下落两分钟
        World->TimeSeconds+=15;
        for (int32 Refresh=0; Refresh<5; ++Refresh)
        {
            Sight.EventId=FGuid::NewGuid();
            Memory->ObserveEvent(Sight);
        }
    }
    TestEqual(TEXT("同一个人的重复视野记录最终只占一条"),Memory->Evidence.FilterByPredicate(
        [](const auto& Item) { return Item.Kind==TEXT("VisibleNow"); }).Num(),1);
    TestEqual(TEXT("四十次视野刷新没有挤掉五条交谈记录"),Memory->Evidence.Num(),6);
    World->TimeSeconds+=90;
    // 这个世界没有导航，放到实际可挥拳的位置，避免把无法寻路当成记忆丢失
    Xiaofang->SetActorLocation(FVector(Dajun->Combat->GetStrikeApproachDistance(Xiaofang)-.5f,0,90));
    TestTrue(TEXT("近处目标满足真实出拳条件"),Dajun->Combat->CanApproachStrike(Xiaofang));
    TestTrue(TEXT("转头一分半后仍能准备交谈上下文"),Memory->BuildDecisionRequest(Epoch,RememberedRequest));
    for (const FGuid Id : ConversationIds)
        TestTrue(TEXT("几分钟前的原话、旁听和本人答话仍保留"),RememberedRequest.Evidence.ContainsByPredicate(
            [&](const auto& Item) { return Item.EventId==Id; }));
    const auto* OldSight=RememberedRequest.Evidence.FindByPredicate([&](const auto& Item)
        { return Item.SubjectId==NeighborId && Item.Kind==TEXT("LastSeen"); });
    if (!TestNotNull(TEXT("过时视野改为先前见过的记录"),OldSight)) return false;
    TestTrue(TEXT("旧位置明确说明当前位置未知"),OldSight->Description.Contains(TEXT("当前位置和行为未知")));
    TestEqual(TEXT("记忆没有偷读邻居移动后的实时位置"),OldSight->ObservedLocation,Sight.ObservedLocation);
    TestTrue(TEXT("模型仍知道刚才交谈的邻居姓名"),RememberedRequest.Subjects.ContainsByPredicate(
        [&](const auto& Item) { return Item.SubjectId==NeighborId && Item.Name==TEXT("小芳"); }));
    TestTrue(TEXT("仍在近处且没有遮挡时可以提供真实攻击候选"),RememberedRequest.Candidates.ContainsByPredicate(
        [&](const auto& Item) { return Item.TargetId==NeighborId && Item.Action==EDeliveryNpcAction::StrikeOnce; }));
    Xiaofang->SetActorLocation(FVector(5000,0,90));
    Memory->BuildDecisionRequest(Epoch,RememberedRequest);
    TestFalse(TEXT("记得邻居不代表能攻击已经远离的人"),RememberedRequest.Candidates.ContainsByPredicate(
        [&](const auto& Item) { return Item.TargetId==NeighborId; }));
    Xiaofang->Destroy();
    Memory->BuildDecisionRequest(Epoch,RememberedRequest);
    TestFalse(TEXT("销毁后的邻居不再作为可操作人物发送"),RememberedRequest.Subjects.ContainsByPredicate(
        [&](const auto& Item) { return Item.SubjectId==NeighborId; }));
    World->TimeSeconds+=211;
    Memory->PruneEvidence();
    TestTrue(TEXT("五分钟后结束本段短时记忆"),Memory->Evidence.IsEmpty());
    Memory->BuildDecisionRequest(Epoch,RememberedRequest);
    TestEqual(TEXT("新请求只重新记录当前身体状态"),RememberedRequest.Evidence.Num(),1);
    TestTrue(TEXT("新身体记录没有恢复已经过期的交谈"),RememberedRequest.Evidence.ContainsByPredicate(
        [](const auto& Item) { return Item.Kind==TEXT("SelfCombat"); }));
    FDeliveryNpcObservation FullWindow; FullWindow.Kind=TEXT("TestFact"); FullWindow.Lifetime=45;
    for (int32 Index=0; Index<32; ++Index) Memory->ObserveEvent(FullWindow);
    Memory->BuildDecisionRequest(Epoch,RememberedRequest);
    Memory->ActiveRequest=RememberedRequest;
    FullWindow.EventId=FGuid::NewGuid(); FullWindow.Kind=TEXT("PlayerSpeech"); FullWindow.Subject=Player;
    FullWindow.Description=TEXT("这次先听我说。");
    Memory->ObserveEvent(FullWindow);
    TestEqual(TEXT("满额请求期间仍保持三十二条上限"),Memory->Evidence.Num(),32);
    TestTrue(TEXT("全部旧记录都被引用时也不会立即删掉玩家的新话"),Memory->Evidence.ContainsByPredicate(
        [&](const auto& Item) { return Item.EventId==FullWindow.EventId; }));

    // 通过正式感知回调检查声音到期，防止它把仍在附近的人记成已经离开
    auto* Observer=World->SpawnActor<ADeliveryNpcCharacter>(FVector(0,-2000,90),FRotator::ZeroRotator,Spawn);
    auto* NpcController=Cast<ADeliveryNpcAIController>(Observer->GetController());
    if (!TestNotNull(TEXT("测试 NPC 使用正式感知控制器"),NpcController)) return false;
    UFunction* PerceptionCallback=NpcController->FindFunction(TEXT("OnTargetSeenOrHeard"));
    if (!TestNotNull(TEXT("正式感知回调可通过反射调用"),PerceptionCallback)) return false;
    auto NotifyPerception=[&](const FAIStimulus& Stimulus)
    {
        struct FCallbackParameters
        {
            AActor* Target;
            FAIStimulus Stimulus;
        } Parameters{Neighbor,Stimulus};
        NpcController->ProcessEvent(PerceptionCallback,&Parameters);
    };
    auto* ObservationMemory=Observer->Agent.Get();
    ObservationMemory->Evidence.Reset();
    Observer->bDecisionNeeded=false;
    const FVector SeenLocation(120,-2000,90);
    const FVector HeardLocation(180,-2000,90);
    const FAIStimulus Seen(*GetDefault<UAISense_Sight>(),1.f,SeenLocation,Observer->GetActorLocation());
    const FAIStimulus Heard(*GetDefault<UAISense_Hearing>(),1.f,HeardLocation,Observer->GetActorLocation());
    NotifyPerception(Seen);
    if (!TestEqual(TEXT("看见对象后保存一次观察"),ObservationMemory->Evidence.Num(),1)) return false;
    TestEqual(TEXT("视觉观察标为看见"),ObservationMemory->Evidence.Last().Source,EDeliveryNpcEvidenceSource::Seen);
    TestEqual(TEXT("观察保存感知时的位置"),ObservationMemory->Evidence.Last().ObservedLocation,SeenLocation);
    TestTrue(TEXT("距离使用感知位置，没有偷读目标当前坐标"),ObservationMemory->Evidence.Last().Description.Contains(TEXT("120 厘米")));
    TestTrue(TEXT("成功看见对象仍会安排一次决策"),Observer->bDecisionNeeded);
    Observer->bDecisionNeeded=false;
    NotifyPerception(Heard);
    if (!TestEqual(TEXT("听见声音后保留视觉和听觉两条记录"),ObservationMemory->Evidence.Num(),2)) return false;
    TestEqual(TEXT("听觉观察标为听见"),ObservationMemory->Evidence.Last().Source,EDeliveryNpcEvidenceSource::Heard);
    TestEqual(TEXT("声音保留实际听到的位置"),ObservationMemory->Evidence.Last().ObservedLocation,HeardLocation);
    TestTrue(TEXT("成功听见声音仍会安排一次决策"),Observer->bDecisionNeeded);

    Observer->bDecisionNeeded=false;
    Observer->NextDecision=World->GetTimeSeconds()+100;
    const double PreviousDecisionTime=Observer->NextDecision;
    const auto RevisionBeforeExpiry=ObservationMemory->ContextRevision;
    FAIStimulus ExpiredHearing=Heard;
    ExpiredHearing.MarkExpired();
    NotifyPerception(ExpiredHearing);
    TestEqual(TEXT("声音到期没有添加或删除观察"),ObservationMemory->Evidence.Num(),2);
    TestEqual(TEXT("声音到期没有改变模型上下文版本"),ObservationMemory->ContextRevision,RevisionBeforeExpiry);
    TestFalse(TEXT("声音到期不会生成 LostSight"),ObservationMemory->Evidence.ContainsByPredicate(
        [](const auto& Item){ return Item.Kind==TEXT("LostSight"); }));
    TestFalse(TEXT("声音到期不会安排下一轮模型请求"),Observer->bDecisionNeeded);
    TestFalse(TEXT("声音到期不会合并出下一轮请求"),ObservationMemory->bDecisionQueued);
    TestEqual(TEXT("声音到期不改变下一次决策时间"),Observer->NextDecision,PreviousDecisionTime);

    // 未接入的感知类型也不能默认当成视觉
    auto* TouchSense=GetMutableDefault<UAISense_Touch>();
    TouchSense->UpdateSenseID();
    FAIStimulus OtherSense(*TouchSense,1.f,SeenLocation,Observer->GetActorLocation());
    NotifyPerception(OtherSense);
    OtherSense.MarkNoLongerSensed();
    NotifyPerception(OtherSense);
    NotifyPerception(FAIStimulus());
    TestEqual(TEXT("其它感知与无效感知不会添加记录"),ObservationMemory->Evidence.Num(),2);
    TestFalse(TEXT("其它感知与无效感知不会安排下一轮请求"),Observer->bDecisionNeeded);

    FAIStimulus LostSight=Seen;
    LostSight.MarkNoLongerSensed();
    NotifyPerception(LostSight);
    const auto* LastSight=ObservationMemory->Evidence.FindByPredicate(
        [](const auto& Item){ return Item.Kind==TEXT("LostSight"); });
    if (!TestNotNull(TEXT("视觉失效仍记录暂时看不见"),LastSight)) return false;
    TestEqual(TEXT("视觉失效仍属于视觉观察"),LastSight->Source,EDeliveryNpcEvidenceSource::Seen);
    TestTrue(TEXT("视觉失效保留最后一次有效观察位置"),LastSight->bHasObservedLocation);
    TestEqual(TEXT("视觉失效不会改用目标当前坐标"),LastSight->ObservedLocation,SeenLocation);
    TestTrue(TEXT("暂时看不见不声称对象已经走远"),LastSight->Description.Contains(TEXT("不能据此判断对方已经走远")));
    TestTrue(TEXT("真实视觉变化仍安排决策"),Observer->bDecisionNeeded);

    LostSight.StimulusLocation=FAISystem::InvalidLocation;
    NotifyPerception(LostSight);
    TestFalse(TEXT("无效视觉位置不会被当作有效观察"),ObservationMemory->Evidence.Last().bHasObservedLocation);
    NotifyPerception(Seen);
    TestEqual(TEXT("再次看见对象恢复正常视觉观察"),ObservationMemory->Evidence.Last().Kind,FName(TEXT("Perceived")));
    TestFalse(TEXT("再次看见对象后替换旧的失去视线记录"),ObservationMemory->Evidence.ContainsByPredicate(
        [](const auto& Item){ return Item.Kind==TEXT("LostSight"); }));

    // 最后一轮话照常记住，不打断当前台词，也不额外安排模型回复
    Neighbor->SetActorLocation(Observer->GetActorLocation()+FVector(200,0,0));
    Observer->ConversationDepth=1;
    if (!TestTrue(TEXT("先开始一段尚未说完的台词"),
        Observer->Presentation->PublishSpeech(TEXT("我这句话还没说完。"),TEXT("Calm"),1))) return false;
    const FGuid SpeakingTurn=Observer->Presentation->GetSpeech().TurnId;
    for (const bool bPending:{false,true})
    {
        Observer->bDecisionNeeded=false;
        Observer->NextDecision=World->GetTimeSeconds()+100;
        ObservationMemory->bDecisionPending=bPending;
        ObservationMemory->bDecisionQueued=false;
        const double NextDecisionBeforeSpeech=Observer->NextDecision;
        const FGuid FinalMessage=FGuid::NewGuid(), ReplyToMessage=FGuid::NewGuid();
        TestTrue(TEXT("只记住最后一句也经过正式交谈入口"),
            Observer->HearSpeech(Neighbor,TEXT("你刚才那句话我可记住了。"),FinalMessage,3,Observer,ReplyToMessage,false));
        TestTrue(TEXT("最后一句保留原文、收听方式和回应编号"),ObservationMemory->Evidence.ContainsByPredicate(
            [&](const auto& Item){ return Item.EventId==FinalMessage && Item.Kind==TEXT("Speech") &&
                Item.ReplyToEventId==ReplyToMessage && Item.Description==TEXT("你刚才那句话我可记住了。") && !Item.bNeedsReply; }));
        TestEqual(TEXT("只记住话不改变原来的接话轮数"),Observer->GetConversationDepth(),1);
        TestFalse(TEXT("只记住话不唤醒下一轮决策"),Observer->bDecisionNeeded);
        TestFalse(TEXT("等待模型时只记住话也不追加请求"),ObservationMemory->bDecisionQueued);
        TestEqual(TEXT("只记住话保留当前请求状态"),ObservationMemory->bDecisionPending,bPending);
        TestEqual(TEXT("只记住话不提前下一次决策"),Observer->NextDecision,NextDecisionBeforeSpeech);
        TestTrue(TEXT("记住邻居最后一句时当前台词继续播放"),Observer->Presentation->GetSpeech().bActive &&
            Observer->Presentation->GetSpeech().TurnId==SpeakingTurn);
        const auto RevisionAfterSpeech=ObservationMemory->ContextRevision;
        TestFalse(TEXT("只记住话也拒绝同一句重复通知"),
            Observer->HearSpeech(Neighbor,TEXT("你刚才那句话我可记住了。"),FinalMessage,3,Observer,ReplyToMessage,false));
        TestEqual(TEXT("重复通知不会再次修改记忆"),ObservationMemory->ContextRevision,RevisionAfterSpeech);
    }
    Observer->bDecisionNeeded=true;
    ObservationMemory->bDecisionQueued=true;
    TestTrue(TEXT("已有后续安排时仍可记住最后一句"),
        Observer->HearSpeech(Neighbor,TEXT("行了，这事以后再说。"),FGuid::NewGuid(),3,Observer,FGuid(),false));
    TestTrue(TEXT("记住最后一句不会清掉原来的决策安排"),Observer->bDecisionNeeded && ObservationMemory->bDecisionQueued);
    ObservationMemory->bDecisionPending=false;
    ObservationMemory->bDecisionQueued=false;
    Observer->bDecisionNeeded=false;
    Player->SetActorLocation(Observer->GetActorLocation()+FVector(0,200,0));
    TestTrue(TEXT("最后一句之后玩家可以重新开口"),
        Observer->HearSpeech(Player,TEXT("先听我说，刚才是我没讲清楚。"),FGuid::NewGuid(),0));
    TestEqual(TEXT("新的玩家发言重新从零开始接话"),Observer->GetConversationDepth(),0);
    TestTrue(TEXT("新的玩家发言照常进入倾听"),ObservationMemory->IsListening());
    TestTrue(TEXT("后端关闭时新玩家发言仍安排稍后回复"),Observer->bDecisionNeeded);
    TestTrue(TEXT("玩家新发言保留没有玩家归属的环境台词"),Observer->Presentation->GetSpeech().bActive &&
        Observer->Presentation->GetSpeech().TurnId==SpeakingTurn);

    // 请求已经发出时收到最后一句，也不能把正在生成的回复判为过期
    FDeliveryNpcDecisionRequest PendingRequest;
    if (!TestTrue(TEXT("实际建立一份等待模型的请求"),ObservationMemory->BuildDecisionRequest(Epoch,PendingRequest))) return false;
    ObservationMemory->ActiveRequest=PendingRequest;
    ObservationMemory->bDecisionPending=true;
    Observer->bDecisionNeeded=false;
    const FGuid LastHeardMessage=FGuid::NewGuid();
    TestTrue(TEXT("等待模型期间记住邻居的最后一句"),
        Observer->HearSpeech(Neighbor,TEXT("我先把话说到这儿。"),LastHeardMessage,3,Observer,FGuid(),false));
    FDeliveryNpcDecisionProposal PendingProposal;
    PendingProposal.WorldEpoch=PendingRequest.WorldEpoch;
    PendingProposal.NpcId=PendingRequest.NpcId;
    PendingProposal.RequestId=PendingRequest.RequestId;
    PendingProposal.ContextRevision=PendingRequest.ContextRevision;
    PendingProposal.PlanRevision=PendingRequest.PlanRevision;
    const auto* PendingChoice=PendingRequest.Candidates.FindByPredicate([](const auto& Item)
        { return Item.Action==EDeliveryNpcAction::Wait; });
    if (!TestNotNull(TEXT("本次玩家请求提供真实等待候选"),PendingChoice)) return false;
    PendingProposal.CapabilityVersion=PendingRequest.RequestedCapabilityVersion;
    PendingProposal.CandidateId=PendingChoice->CandidateId;
    PendingProposal.Action=PendingChoice->Action;
    PendingProposal.TargetId=PendingChoice->TargetId;
    PendingProposal.EvidenceIds=PendingChoice->EvidenceIds;
    PendingProposal.Duration=1;
    PendingProposal.Speech=TEXT("行，你接着说，我听着呢。");
    TestTrue(TEXT("只记住的直接发言不会作废已发出的请求"),ObservationMemory->TryAcceptProposal(PendingProposal,Reason));
    TestFalse(TEXT("记住最后一句后没有额外唤醒请求"),Observer->bDecisionNeeded || ObservationMemory->bDecisionQueued);
    TestFalse(TEXT("当前模型没读过最后一句，不把它记成已经处理"),ObservationMemory->LastDecisionEventIds.Contains(LastHeardMessage));

    // 复现玩家刚开口、邻居上一句才播完的顺序，检查两种回复接口都先完成玩家这轮
    auto* ReplyNpc=World->SpawnActor<ADeliveryNpcCharacter>(FVector(-3000,-3000,90),FRotator::ZeroRotator,Spawn);
    auto* ReplyNeighbor=World->SpawnActor<ADeliveryNpcCharacter>(FVector(-2800,-3000,90),FRotator::ZeroRotator,Spawn);
    Player->SetActorLocation(FVector(-3000,-2800,90));
    TGuardValue<bool> StreamReplies(Settings->bStreamReplies,true);
    auto* ReplyMemory=ReplyNpc->Agent.Get();
    auto PreparePlayerReply=[&](FDeliveryNpcDecisionProposal& Reply)
    {
        ReplyMemory->ResetContext(TEXT("NextPlayerReplyCheck"));
        ReplyNpc->ObserveFact(TEXT("VisibleNow"),ReplyNeighbor,TEXT("邻居站在旁边"),EDeliveryNpcEvidenceSource::Seen);
        if (!ReplyNpc->HearSpeech(Player,TEXT("他刚才说你不敢，你怎么想？"),FGuid::NewGuid(),0,ReplyNpc)) return false;
        FDeliveryNpcDecisionRequest Request;
        if (!ReplyMemory->BuildDecisionRequest(Epoch,Request) || !Request.bPlayerRequest) return false;
        ReplyMemory->ActiveRequest=Request;
        ReplyMemory->bDecisionPending=true;
        ReplyNpc->bDecisionNeeded=false;
        const auto* Choice=Request.Candidates.FindByPredicate([](const auto& Item){return Item.Action==EDeliveryNpcAction::Wait;});
        if (!Choice) return false;
        Reply={};
        Reply.WorldEpoch=Request.WorldEpoch;Reply.NpcId=Request.NpcId;Reply.RequestId=Request.RequestId;
        Reply.ContextRevision=Request.ContextRevision;Reply.PlanRevision=Request.PlanRevision;
        Reply.CapabilityVersion=Request.RequestedCapabilityVersion;
        Reply.CandidateId=Choice->CandidateId;Reply.Action=Choice->Action;
        Reply.TargetId=Choice->TargetId;Reply.EvidenceIds=Choice->EvidenceIds;
        Reply.Duration=1;Reply.Speech=TEXT("他真这么说了？我倒要听听他怎么讲。");
        return true;
    };
    if (!TestTrue(TEXT("准备真实玩家发言对应的流式请求"),PreparePlayerReply(Proposal))) return false;
    const FGuid DeferredNeighborLine=FGuid::NewGuid();
    TestTrue(TEXT("玩家等待时邻居此前的闲聊刚好播完"),ReplyNpc->HearSpeech(
        ReplyNeighbor,TEXT("我一向早起，哪像你还得出来透气！"),DeferredNeighborLine,2,ReplyNpc));
    TestTrue(TEXT("后来听到的闲聊已安排后续处理"),ReplyMemory->bDecisionQueued);
    if (!TestTrue(TEXT("邻居的旧闲聊不会作废已经生成的玩家回复"),
        ReplyMemory->ReceiveStreamDecision(Proposal.RequestId,Proposal))) return false;
    TestFalse(TEXT("这次玩家回复没有冒充读过后来才听到的话"),ReplyMemory->LastDecisionEventIds.Contains(DeferredNeighborLine));
    ReplyMemory->FinishReply(Proposal.RequestId,true,0);
    TestTrue(TEXT("玩家这轮生成结束后仍保留邻居的接话安排"),ReplyNpc->bDecisionNeeded);
    ReplyMemory->CancelPlan(TEXT("InspectDeferredConversation"));
    FDeliveryNpcDecisionRequest DeferredRequest;
    TestTrue(TEXT("下一轮能重新准备现场信息"),ReplyMemory->BuildDecisionRequest(Epoch,DeferredRequest));
    TestTrue(TEXT("下一轮仍能读到邻居刚才说的完整原话"),DeferredRequest.Evidence.ContainsByPredicate(
        [&](const auto& Item){return Item.EventId==DeferredNeighborLine && Item.Kind==TEXT("Speech");}));
    TestFalse(TEXT("后续请求仍区分还没有处理的新话"),DeferredRequest.LastDecisionEventIds.Contains(DeferredNeighborLine));

    if (!TestTrue(TEXT("准备完整返回的玩家回复"),PreparePlayerReply(Proposal))) return false;
    const FGuid DeferredWholeReplyLine=FGuid::NewGuid();
    TestTrue(TEXT("完整返回期间同样收到邻居的旧闲聊"),ReplyNpc->HearSpeech(
        ReplyNeighbor,TEXT("我刚才还没说完呢。"),DeferredWholeReplyLine,2,ReplyNpc));
    ReplyMemory->ReceiveDecision(Proposal.RequestId,&Proposal,NAME_None);
    TestEqual(TEXT("完整返回也能先接收玩家回复"),ReplyMemory->GetPublicState().Status,EDeliveryNpcActionStatus::Accepted);
    TestTrue(TEXT("完整返回后仍会处理邻居的新一句"),ReplyNpc->bDecisionNeeded);
    TestFalse(TEXT("完整返回没有提前标记邻居的话已处理"),ReplyMemory->LastDecisionEventIds.Contains(DeferredWholeReplyLine));

    if (!TestTrue(TEXT("准备检查排队中的新玩家发言"),PreparePlayerReply(Proposal))) return false;
    FDeliveryNpcObservation NewPlayerLine;
    NewPlayerLine.Kind=TEXT("PlayerSpeech");NewPlayerLine.Subject=Player;NewPlayerLine.Addressee=ReplyNpc;
    NewPlayerLine.Source=EDeliveryNpcEvidenceSource::Heard;NewPlayerLine.bNeedsReply=true;
    NewPlayerLine.Description=TEXT("等一下，我刚才说错了，先别去找他。");
    ReplyMemory->ObserveEvent(NewPlayerLine);
    TestFalse(TEXT("玩家的新一句仍会作废上一句的回复"),ReplyMemory->TryAcceptProposal(Proposal,Reason));
    TestEqual(TEXT("新玩家发言仍沿用原来的过期重试处理"),Reason,FName(TEXT("StaleProposal")));

    if (!TestTrue(TEXT("准备检查玩家再次开口的正式入口"),PreparePlayerReply(Proposal))) return false;
    const FGuid InterruptedByPlayer=FGuid::NewGuid();
    TestTrue(TEXT("玩家可以通过正式入口打断自己的旧问题"),ReplyNpc->HearSpeech(
        Player,TEXT("先听我说，刚才是我瞎说的。"),InterruptedByPlayer,0,ReplyNpc));
    TestFalse(TEXT("旧请求在新玩家发言后不能再开始播放"),ReplyMemory->ReceiveStreamDecision(Proposal.RequestId,Proposal));
    TestEqual(TEXT("倾听仍对应玩家最新的这句话"),ReplyMemory->ListeningMessage,InterruptedByPlayer);

    // 环境台词没有玩家归属；玩家自己的已播放回复由正式请求保存归属后精确取消
    if (!TestTrue(TEXT("准备玩家已有台词的真实请求"),PreparePlayerReply(Proposal))) return false;
    if (!TestTrue(TEXT("通过正式流式入口接收玩家回复"),ReplyMemory->ReceiveStreamDecision(Proposal.RequestId,Proposal))) return false;
    FDeliveryNpcPublicState SpeakingAction;
    if (!TestTrue(TEXT("玩家回复对应动作真正开始"),ReplyMemory->StartAction(SpeakingAction)) ||
        !TestTrue(TEXT("执行玩家回复对应动作"),ReplyNpc->ExecuteAction(SpeakingAction))) return false;
    FDeliveryNpcSentence SpeakingSentence;
    SpeakingSentence.Sequence=0; SpeakingSentence.Text=Proposal.Speech;
    ReplyMemory->ReceiveSentence(Proposal.RequestId,SpeakingSentence);
    ReplyMemory->FinishReply(Proposal.RequestId,true,1);
    static_cast<UActorComponent*>(ReplyNpc->Presentation.Get())->TickComponent(.02f,LEVELTICK_All,nullptr);
    if (!TestTrue(TEXT("再次插话前自己的旧回复已经播放"),ReplyNpc->Presentation->GetSpeech().bActive)) return false;
    TestTrue(TEXT("模型结束后仍保存这名玩家的回复归属"),ReplyMemory->IsPlayerReplyActive(Controller));
    TestTrue(TEXT("同一玩家可以打断自己的已播放回复"),ReplyNpc->HearSpeech(
        Player,TEXT("先等一下，我重新说一遍。"),FGuid::NewGuid(),0,ReplyNpc));
    TestFalse(TEXT("同一玩家新发言确实停止自己的旧台词"),ReplyNpc->Presentation->GetSpeech().bActive);
    TestTrue(TEXT("取消旧台词保留原来的活动"),ReplyMemory->GetPublicState().ActionId==SpeakingAction.ActionId &&
        ReplyMemory->GetPublicState().Status==EDeliveryNpcActionStatus::Running);

    if (!TestTrue(TEXT("准备检查动作目标销毁"),PreparePlayerReply(Proposal))) return false;
    const auto* TargetChoice=ReplyMemory->ActiveRequest.Candidates.FindByPredicate([&](const auto& Item)
        {return Item.Action==EDeliveryNpcAction::LookAt && ReplyMemory->KnownSubjects.FindRef(Item.TargetId).Get()==ReplyNeighbor;});
    if (!TestNotNull(TEXT("请求包含看向邻居的真实候选"),TargetChoice)) return false;
    Proposal.Action=TargetChoice->Action;Proposal.TargetId=TargetChoice->TargetId;
    Proposal.CandidateId=TargetChoice->CandidateId;Proposal.EvidenceIds=TargetChoice->EvidenceIds;
    ReplyNeighbor->Destroy();
    TestFalse(TEXT("玩家优先不会让已经销毁的目标继续执行动作"),ReplyMemory->TryAcceptProposal(Proposal,Reason));

    // 受击仍经正式角色接口取消请求，和普通闲聊区分开
    ReplyMemory->CancelPlan(TEXT("BeforeHitCheck"));
    if (!TestTrue(TEXT("受击前仍能接收玩家发言"),ReplyNpc->HearSpeech(
        Player,TEXT("你先听我把话说完。"),FGuid::NewGuid(),0,ReplyNpc))) return false;
    FDeliveryNpcDecisionRequest BeforeHit;
    if (!TestTrue(TEXT("受击前已保存本轮请求"),ReplyMemory->BuildDecisionRequest(Epoch,BeforeHit))) return false;
    ReplyMemory->ActiveRequest=BeforeHit;ReplyMemory->bDecisionPending=true;
    Proposal.RequestId=BeforeHit.RequestId;
    ReplyNpc->ReceiveImpactFrom(FVector(100,0,0),ReplyNpc->GetActorLocation(),Player);
    TestTrue(TEXT("受击仍会让居民倒地"),ReplyNpc->IsKnockedDown());
    TestFalse(TEXT("受击已取消等待中的请求"),ReplyMemory->IsDecisionPending());
    TestFalse(TEXT("受击后旧模型回复仍不能执行"),ReplyMemory->ReceiveStreamDecision(BeforeHit.RequestId,Proposal));
    return true;
}
#endif
