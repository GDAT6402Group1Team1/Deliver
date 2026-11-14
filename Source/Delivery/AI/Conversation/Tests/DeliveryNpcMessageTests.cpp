#include "Misc/AutomationTest.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Conversation/DeliveryConversationTurnSubsystem.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Components/SceneComponent.h"
#include "HAL/PlatformTime.h"

namespace
{
    // 与现有占用测试相同的独立世界；不启用后台、声音或导航
    struct FNpcMessageTestWorld
    {
        UWorld* World=nullptr;
        bool bWasEnabled=false;
        FNpcMessageTestWorld()
        {
            auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
            bWasEnabled=Settings->bEnabled; Settings->bEnabled=false;
            const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
                .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
            World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
            if (!World) return;
            GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
            World->InitializeActorsForPlay(FURL());
            World->GetWorldSettings()->NotifyBeginPlay();
        }
        ~FNpcMessageTestWorld()
        {
            if (World)
            {
                World->EndPlay(EEndPlayReason::Quit);
                GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
            }
            GetMutableDefault<UDeliveryAiSettings>()->bEnabled=bWasEnabled;
        }
        ADeliveryNpcCharacter* Npc(float Y)
        {
            FActorSpawnParameters Spawn;
            Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            auto* Result=World->SpawnActor<ADeliveryNpcCharacter>(FVector(200,Y,100),FRotator::ZeroRotator,Spawn);
            if (Result) Result->SetActorTickEnabled(false);
            return Result;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcMessageFlowTest,"Delivery.AI.Conversation.NpcMessageFlow",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FDeliveryNpcMessageFlowTest::RunTest(const FString&)
{
    FNpcMessageTestWorld Fixture;
    if (!TestNotNull(TEXT("World"),Fixture.World)) return false;
    auto* Turns=Fixture.World->GetSubsystem<UDeliveryConversationTurnSubsystem>();
    auto* A=Fixture.Npc(0); auto* B=Fixture.Npc(200); auto* C=Fixture.Npc(400);
    if (!TestNotNull(TEXT("Turns"),Turns) || !TestNotNull(TEXT("A"),A) ||
        !TestNotNull(TEXT("B"),B) || !TestNotNull(TEXT("C"),C)) return false;
    auto MakeReply=[](ADeliveryNpcCharacter* Npc,int32 Depth,FDeliveryNpcMessageHandle Parent={})
    {
        FDeliveryNpcReplyRegistration Reply;
        Reply.Npc=Npc; Reply.RequestId=FGuid::NewGuid(); Reply.ReplyId=FGuid::NewGuid();
        Reply.ActionId=FGuid::NewGuid(); Reply.PlanRevision=1; Reply.Depth=Depth; Reply.ParentMessage=Parent;
        Reply.bOwnsAction=true;
        return Reply;
    };
    const auto First=MakeReply(A,0);
    TestTrue(TEXT("Register approved root"),Turns->RegisterNpcReply(First));
    FDeliveryNpcMessage AB;
    TestTrue(TEXT("Prepare directed complete text"),Turns->TryPrepareNpcMessage(A,First.RequestId,First.ReplyId,B,TEXT("我准备去面馆。你要一起吗？"),{},AB));
    TestFalse(TEXT("Prepared is not delivered"),Turns->WasNpcMessageDelivered(First.ReplyId,B));
    TestTrue(TEXT("Receiver records actual text delivery"),B->Agent->ReceiveNpcMessage(AB.Handle));
    TestFalse(TEXT("Duplicate text rejected"),B->Agent->ReceiveNpcMessage(AB.Handle));
    TestTrue(TEXT("Only exact addressee has receipt"),Turns->WasNpcMessageDelivered(First.ReplyId,B));
    TestFalse(TEXT("Bystander C has no receipt"),Turns->WasNpcMessageDelivered(First.ReplyId,C));
    FDeliveryNpcDecisionRequest Request;
    TestTrue(TEXT("Build receiver decision"),B->Agent->BuildDecisionRequest(Turns->GetWorldEpoch(),Request));
    TestTrue(TEXT("Native parent retained"),Request.ParentNpcMessage==AB.Handle);
    TestFalse(TEXT("NPC relaying never acquires player request"),Request.bPlayerRequest);
    TestFalse(TEXT("NPC relaying never acquires owner PlayerState"),Request.RequesterPlayerState.IsValid());
    const auto* Evidence=Request.Evidence.FindByPredicate([&](const auto& E){ return E.EventId==AB.Handle.MessageId; });
    if (TestNotNull(TEXT("Delivered evidence"),Evidence))
    {
        TestEqual(TEXT("ToldBy classification"),Evidence->Source,EDeliveryNpcEvidenceSource::ToldBy);
        TestEqual(TEXT("Original text"),Evidence->Description,AB.Text);
        TestEqual(TEXT("NpcMessage classification"),Evidence->Kind,FName(TEXT("NpcMessage")));
    }
    const auto Second=MakeReply(B,1,AB.Handle);
    TestTrue(TEXT("Register child"),Turns->RegisterNpcReply(Second));
    TestFalse(TEXT("Child waits root sound"),Turns->CanStartNpcReplyPresentation(Second.ReplyId));
    FDeliveryNpcMessage BC;
    TestTrue(TEXT("Prepare second directed text"),Turns->TryPrepareNpcMessage(B,Second.RequestId,Second.ReplyId,C,TEXT("走，一起去。"),AB.Handle.MessageId,BC));
    TestTrue(TEXT("Deliver second text"),C->Agent->ReceiveNpcMessage(BC.Handle));
    const auto Third=MakeReply(C,2,BC.Handle);
    TestTrue(TEXT("Register grandchild"),Turns->RegisterNpcReply(Third));
    TArray<FDeliveryNpcMessageCancellation> Cancelled;
    Turns->ReportNpcReplyPresentation(Second.ReplyId,EDeliveryNpcReplyPresentation::Unavailable,Cancelled);
    TestFalse(TEXT("Unavailable parent cannot bypass speaking grandparent"),Turns->CanStartNpcReplyPresentation(Third.ReplyId));
    Turns->ReportNpcReplyPresentation(First.ReplyId,EDeliveryNpcReplyPresentation::Completed,Cancelled);
    TestTrue(TEXT("All ancestors terminal releases child audio"),Turns->CanStartNpcReplyPresentation(Third.ReplyId));
    FDeliveryNpcMessage CA;
    TestTrue(TEXT("Third delivery allowed"),Turns->TryPrepareNpcMessage(C,Third.RequestId,Third.ReplyId,A,TEXT("那我在这等你们。"),BC.Handle.MessageId,CA));
    TestEqual(TEXT("Maximum depth"),CA.Depth,3);
    TestFalse(TEXT("Final delivery only records history"),CA.bNeedsReply);
    TestTrue(TEXT("Final delivery retained"),A->Agent->ReceiveNpcMessage(CA.Handle));
    TestFalse(TEXT("No fourth model reply on final message"),Turns->RegisterNpcReply(MakeReply(A,3,CA.Handle)));

    Turns->InvalidateNpcBranchesFor(B,TEXT("PlayerInputStarted"),Cancelled);
    TestFalse(TEXT("B old incoming message no longer current"),Turns->IsNpcMessageCurrent(AB.Handle,B));
    TestTrue(TEXT("Interruption leaves actual received history"),Turns->WasNpcMessageDelivered(First.ReplyId,B));
    TestTrue(TEXT("Interruption does not cancel ancestor A"),Turns->CanStartNpcReplyPresentation(First.ReplyId));
    TestFalse(TEXT("Interruption cancels descendant C audio"),Turns->CanStartNpcReplyPresentation(Third.ReplyId));
    Turns->OnNpcMessagesInvalidated.Broadcast(Cancelled);
    TestFalse(TEXT("Old incoming cannot remain a pending reply"),B->Agent->HasPendingNpcMessage());

    // 原生输入占用三阶段均拒绝快投递，普通请求权限不被转述放大
    auto* PC=Fixture.World->SpawnActor<APlayerController>();
    auto* Pawn=Fixture.World->SpawnActor<APawn>();
    auto* Root=NewObject<USceneComponent>(Pawn); Pawn->SetRootComponent(Root); Root->RegisterComponent();
    PC->PlayerState=Fixture.World->SpawnActor<APlayerState>(); PC->Possess(Pawn);
    FDeliveryConversationTurnHandle Lease;
    TArray<FDeliveryConversationTurnLease> Replaced;
    TestEqual(TEXT("Begin actual recording lease"),Turns->TryBegin(PC,B,FGuid::NewGuid(),1,Lease,Replaced),EDeliveryConversationTurnResult::Accepted);
    const auto Fourth=MakeReply(C,0);
    TestTrue(TEXT("Register independent root"),Turns->RegisterNpcReply(Fourth));
    FDeliveryNpcMessage Blocked;
    TestFalse(TEXT("Recording blocks delivery"),Turns->TryPrepareNpcMessage(C,Fourth.RequestId,Fourth.ReplyId,B,TEXT("你好"),{},Blocked));
    Turns->FinishRecording(PC,Lease);
    TestFalse(TEXT("FinalPending blocks delivery"),Turns->TryPrepareNpcMessage(C,Fourth.RequestId,Fourth.ReplyId,B,TEXT("你好"),{},Blocked));
    Turns->CommitFinal(PC,B,Lease,Lease.Token);
    TestFalse(TEXT("AwaitingReply blocks delivery"),Turns->TryPrepareNpcMessage(C,Fourth.RequestId,Fourth.ReplyId,B,TEXT("你好"),{},Blocked));
    Turns->Cancel(PC,Lease);
    TestTrue(TEXT("Released lease allows later independent text"),Turns->TryPrepareNpcMessage(C,Fourth.RequestId,Fourth.ReplyId,B,TEXT("你好"),{},Blocked));
    auto OldWorld=Blocked.Handle; OldWorld.WorldEpoch=FGuid::NewGuid();
    TestFalse(TEXT("Different world handle rejected"),B->Agent->ReceiveNpcMessage(OldWorld));
    auto OldGeneration=Blocked.Handle; ++OldGeneration.Generation;
    TestFalse(TEXT("Different message generation rejected"),B->Agent->ReceiveNpcMessage(OldGeneration));
    TestTrue(TEXT("Original current handle remains valid"),B->Agent->ReceiveNpcMessage(Blocked.Handle));
    TestFalse(TEXT("Null addressee is never delivered"),Turns->WasNpcMessageDelivered(Fourth.ReplyId,nullptr));

    const auto Waiting=MakeReply(B,1,Blocked.Handle);
    TestTrue(TEXT("Register waiting reply"),Turns->RegisterNpcReply(Waiting));
    Turns->NpcReplies.FindChecked(Waiting.ReplyId).CreatedAt=FPlatformTime::Seconds()-61;
    Turns->NextPruneAt=FPlatformTime::Seconds()-1;
    const double DueAt=Turns->NextPruneAt;
    FDeliveryConversationTurnLease NoLease;
    Turns->GetLease(B,NoLease);
    TestEqual(TEXT("Read query cannot postpone scheduled cleanup"),Turns->NextPruneAt,DueAt);
    Turns->Tick(0);
    TestTrue(TEXT("Sixty second ancestor wait cancels child"),Turns->NpcReplies.FindChecked(Waiting.ReplyId).bInvalid);
    Turns->NpcReplies.FindChecked(Fourth.ReplyId).ChainStartedAt=FPlatformTime::Seconds()-121;
    Turns->NextPruneAt=FPlatformTime::Seconds()-1; Turns->Tick(0);
    TestFalse(TEXT("Expired chain cannot accept more input"),Turns->IsNpcMessageCurrent(Blocked.Handle,B));
    TestTrue(TEXT("Expired chain still remembers actual delivery"),Turns->WasNpcMessageDelivered(Fourth.ReplyId,B));
    Turns->NpcReplies.FindChecked(Fourth.ReplyId).TerminalAt=FPlatformTime::Seconds()-301;
    Turns->NextPruneAt=FPlatformTime::Seconds()-1; Turns->Tick(0);
    TestFalse(TEXT("Terminal receipt is eventually removed"),Turns->NpcReplies.Contains(Fourth.ReplyId));
    while (Turns->NpcReplies.Num()<Turns->MaxNpcReplyRecords)
        if (!Turns->RegisterNpcReply(MakeReply(A,0))) { AddError(TEXT("Could not fill bounded reply table")); break; }
    const int32 FullCount=Turns->NpcReplies.Num();
    TestFalse(TEXT("Full table rejects fast tracking"),Turns->RegisterNpcReply(MakeReply(A,0)));
    TestEqual(TEXT("Full table does not evict existing records"),Turns->NpcReplies.Num(),FullCount);

    TestTrue(TEXT("Empty model completion valid"),DeliveryNpcMessages::IsValidReplyText(TEXT("")));
    TestFalse(TEXT("Empty delivery invalid"),DeliveryNpcMessages::IsValidReplyText(TEXT(""),false));
    TestFalse(TEXT("Overlong text rejected, never truncated"),DeliveryNpcMessages::IsValidReplyText(FString::ChrN(121,TEXT('字'))));
    FString Supplementary;
    for (int32 Index=0;Index<120;++Index) { Supplementary.AppendChar(TCHAR(0xD83D)); Supplementary.AppendChar(TCHAR(0xDE00)); }
    TestTrue(TEXT("120 Unicode scalar values accepted"),DeliveryNpcMessages::IsValidReplyText(Supplementary));
    Supplementary.AppendChar(TCHAR(0xD83D));
    TestFalse(TEXT("Unpaired surrogate rejected"),DeliveryNpcMessages::IsValidReplyText(Supplementary));
    return true;
}
#endif
