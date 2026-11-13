#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
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
    TestTrue(TEXT("NPC 向我说的新一句能够被接收"),
        Dajun->HearSpeech(Xiaofang, TEXT("大军，我可没说你，别听他瞎说。"), NewDirectMessage, 1, Dajun));
    TestFalse(TEXT("新一句直接对话到达后，旧请求不能继续答上一句"), Dajun->Agent->TryAcceptProposal(Proposal, Reason));
    TestEqual(TEXT("旧回复注明需要更新对话内容"), Reason, FName(TEXT("StaleProposal")));
    FDeliveryNpcDecisionRequest UpdatedRequest;
    TestTrue(TEXT("可以准备包含新一句的后续请求"), Dajun->Agent->BuildDecisionRequest(Epoch, UpdatedRequest));
    TestTrue(TEXT("后续请求保留新的直接发言"), UpdatedRequest.Evidence.ContainsByPredicate([&](const auto& Item)
        { return Item.EventId == NewDirectMessage && Item.Kind == TEXT("Speech") && Item.AddresseeId == UpdatedRequest.SelfSubjectId; }));

    const FGuid LegacyMessage = FGuid::NewGuid();
    TestTrue(TEXT("原有调用省略收话人时仍可交谈"), Dajun->HearSpeech(Xiaofang, TEXT("大军，你听见没有？"), LegacyMessage, 1));
    TestTrue(TEXT("省略收话人默认说给当前 NPC"), Dajun->Agent->Evidence.ContainsByPredicate([&](const auto& Item)
        { return Item.EventId == LegacyMessage && Item.Kind == TEXT("Speech") && Item.AddresseeId == UpdatedRequest.SelfSubjectId; }));

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
    return true;
}
#endif
