#include "Misc/AutomationTest.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Conversation/DeliveryConversationTurnSubsystem.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "Speech/DeliverySpeechInputComponent.h"
#include "UI/DeliverySpeechStatusWidget.h"
#include "Components/SceneComponent.h"
#include "Components/TextBlock.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformTime.h"
#include "UObject/UnrealType.h"

namespace
{
    struct FConversationTestWorld
    {
        UWorld* World=nullptr;
        bool bPreviousAiEnabled=false;

        FConversationTestWorld()
        {
            auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
            bPreviousAiEnabled=Settings->bEnabled;
            Settings->bEnabled=false;
            const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
                .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
            World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
            if (!World) return;
            GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
            World->InitializeActorsForPlay(FURL());
            World->GetWorldSettings()->NotifyBeginPlay();
        }
        ~FConversationTestWorld()
        {
            if (World)
            {
                World->EndPlay(EEndPlayReason::Quit);
                GEngine->DestroyWorldContext(World);
                World->DestroyWorld(false);
            }
            GetMutableDefault<UDeliveryAiSettings>()->bEnabled=bPreviousAiEnabled;
        }
        APawn* MakePawn()
        {
            auto* Pawn=World->SpawnActor<APawn>();
            auto* Root=NewObject<USceneComponent>(Pawn);
            Pawn->SetRootComponent(Root); Root->RegisterComponent();
            Pawn->SetActorLocation(FVector(0,0,90));
            return Pawn;
        }
        APlayerController* MakePlayer()
        {
            auto* Player=World->SpawnActor<APlayerController>();
            // Pawn 通过 PlayerState 区分玩家与 NPC，独立世界没有 GameMode 代为创建
            Player->PlayerState=World->SpawnActor<APlayerState>();
            Player->PlayerState->SetIsABot(false);
            Player->Possess(MakePawn());
            return Player;
        }
        ADeliveryNpcCharacter* MakeNpc(float Y=0)
        {
            FActorSpawnParameters Spawn;
            Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            auto* Npc=World->SpawnActor<ADeliveryNpcCharacter>(FVector(220,Y,90),FRotator::ZeroRotator,Spawn);
            Npc->SetActorTickEnabled(false);
            return Npc;
        }
        ADeliveryCompanionCharacter* MakeCompanion(APlayerController* Owner,float Y=0)
        {
            FActorSpawnParameters Spawn;
            Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            auto* Npc=World->SpawnActor<ADeliveryCompanionCharacter>(FVector(220,Y,90),FRotator::ZeroRotator,Spawn);
            Npc->SetActorTickEnabled(false);
            Npc->Companion->SetComponentTickEnabled(false);
            // 夹具建立复制后的已结识状态，不把准备归属和阶段当作实际初遇或扶起验收
            auto* Property=FindFProperty<FObjectPropertyBase>(UDeliveryCompanionComponent::StaticClass(),TEXT("OwnerPlayerState"));
            auto* EncounterProperty=FindFProperty<FStructProperty>(ADeliveryCompanionCharacter::StaticClass(),TEXT("Encounter"));
            if (!Property || !EncounterProperty) return nullptr;
            Property->SetObjectPropertyValue_InContainer(Npc->Companion,Owner->PlayerState);
            EncounterProperty->ContainerPtrToValuePtr<FDeliveryCompanionEncounterState>(Npc)->Phase=EDeliveryCompanionEncounterPhase::Joined;
            return Npc;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryConversationLeaseWorldTest,"Delivery.Speech.Conversation.WorldLeases",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FDeliveryConversationLeaseWorldTest::RunTest(const FString&)
{
    FConversationTestWorld Fixture;
    if (!TestNotNull(TEXT("建立真实测试世界"),Fixture.World)) return false;
    auto* Turns=Fixture.World->GetSubsystem<UDeliveryConversationTurnSubsystem>();
    if (!TestNotNull(TEXT("服务器世界自动创建轮次协调"),Turns)) return false;
    auto* First=Fixture.MakePlayer();
    auto* Second=Fixture.MakePlayer();
    auto* Npc=Fixture.MakeNpc();
    auto* OtherNpc=Fixture.MakeNpc(220);
    using EResult=EDeliveryConversationTurnResult;
    FDeliveryConversationTurnHandle FirstHandle,DuplicateHandle,OtherHandle;
    TArray<FDeliveryConversationTurnLease> Replaced;
    const FGuid ClientToken=FGuid::NewGuid();
    TestEqual(TEXT("第一位玩家获得录音占用"),Turns->TryBegin(First,Npc,ClientToken,1,FirstHandle,Replaced),EResult::Accepted);
    TestTrue(TEXT("正式编号由服务器生成"),FirstHandle.Token!=ClientToken);
    FDeliveryConversationTurnLease Lease;
    Turns->GetLease(Npc,Lease);
    const double RecordingDeadline=Lease.Deadline;
    TestEqual(TEXT("相同 Begin 重发幂等"),Turns->TryBegin(First,Npc,ClientToken,1,DuplicateHandle,Replaced),EResult::AlreadyActive);
    TestTrue(TEXT("重发保留原 Handle"),DuplicateHandle==FirstHandle);
    Turns->GetLease(Npc,Lease);
    TestEqual(TEXT("重发不延长录音期限"),Lease.Deadline,RecordingDeadline);
    TestEqual(TEXT("另一玩家收到 Busy"),Turns->TryBegin(Second,Npc,FGuid::NewGuid(),1,OtherHandle,Replaced),EResult::Busy);
    TestEqual(TEXT("松键只转换一次"),Turns->FinishRecording(First,FirstHandle),EResult::Accepted);
    Turns->GetLease(Npc,Lease);
    const double FinalDeadline=Lease.Deadline;
    TestEqual(TEXT("重复松键是幂等读取"),Turns->FinishRecording(First,FirstHandle),EResult::AlreadyActive);
    Turns->GetLease(Npc,Lease);
    TestEqual(TEXT("重复松键不延长文字期限"),Lease.Deadline,FinalDeadline);
    TestFalse(TEXT("不能提交客户端指定的正式编号"),Turns->CommitFinal(First,Npc,FirstHandle,ClientToken));
    TestTrue(TEXT("服务器编号提交一次"),Turns->CommitFinal(First,Npc,FirstHandle,FirstHandle.Token));
    TestFalse(TEXT("重复 final 不再提交"),Turns->CommitFinal(First,Npc,FirstHandle,FirstHandle.Token));
    TestFalse(TEXT("旧回复编号不能释放当前占用"),Turns->ReleaseReply(Npc,FGuid::NewGuid()));

    TestEqual(TEXT("同玩家可向另一个 NPC 开始输入"),Turns->TryBegin(First,OtherNpc,FGuid::NewGuid(),2,OtherHandle,Replaced),EResult::Accepted);
    TestTrue(TEXT("旧 NPC 正在播放的回复没有被清理"),Replaced.IsEmpty() && Turns->GetLease(Npc,Lease));
    TestEqual(TEXT("更旧 Begin 不能重新获得录音许可"),Turns->TryBegin(First,Npc,ClientToken,1,DuplicateHandle,Replaced),EResult::Stale);
    const auto OldRecording=OtherHandle;
    TestEqual(TEXT("同玩家下一轮替换旧录音"),Turns->TryBegin(First,OtherNpc,FGuid::NewGuid(),3,OtherHandle,Replaced),EResult::Accepted);
    TestEqual(TEXT("只返回被替换的旧录音"),Replaced.Num(),1);
    TestFalse(TEXT("旧 Cancel 不影响新轮"),Turns->Cancel(First,OldRecording));
    TestTrue(TEXT("旧 NPC 真实播放完成可精确释放"),Turns->ReleaseReply(Npc,FirstHandle.Token));
    TestTrue(TEXT("另一个 NPC 新录音继续持有"),Turns->GetLease(OtherNpc,Lease) && Lease.Handle==OtherHandle);

    int32 ExpiredCount=0;
    Turns->OnLeaseExpired.AddLambda([&](const FDeliveryConversationTurnLease&,EDeliveryConversationLeaseEnd){ ++ExpiredCount; });
    First->Possess(Fixture.MakePawn());
    TestFalse(TEXT("换 Pawn 立即使旧录音失效"),Turns->GetLease(OtherNpc,Lease));
    TestEqual(TEXT("换 Pawn 只通知一次失效"),ExpiredCount,1);
    TestEqual(TEXT("旧序号不会因释放而复活"),Turns->TryBegin(First,OtherNpc,FGuid::NewGuid(),3,OtherHandle,Replaced),EResult::Stale);
    TestEqual(TEXT("新 Pawn 可以发送更高序号"),Turns->TryBeginText(First,OtherNpc,FGuid::NewGuid(),4,OtherHandle,Replaced),EResult::Accepted);
    Turns->Leases.FindChecked(TWeakObjectPtr<ADeliveryNpcCharacter>(OtherNpc)).Deadline=FPlatformTime::Seconds()-1;
    TestEqual(TEXT("文字到期后不能提交"),Turns->ValidateFinal(First,OtherNpc,OtherHandle),EResult::Stale);
    TestEqual(TEXT("超时也只通知一次"),ExpiredCount,2);
    Turns->OnLeaseExpired.Clear();

    FDeliveryConversationTurnHandle ExpiringHandle,ReentrantHandle;
    Turns->TryBeginText(First,OtherNpc,FGuid::NewGuid(),5,ExpiringHandle,Replaced);
    bool bReentered=false;
    Turns->OnLeaseExpired.AddLambda([&](const FDeliveryConversationTurnLease& Expired,EDeliveryConversationLeaseEnd)
    {
        if (!(Expired.Handle==ExpiringHandle)) return;
        FDeliveryConversationTurnLease ReadBack;
        bReentered=!Turns->GetLease(OtherNpc,ReadBack);
        Turns->TryBegin(First,OtherNpc,FGuid::NewGuid(),6,ReentrantHandle,Replaced);
    });
    Turns->Leases.FindChecked(TWeakObjectPtr<ADeliveryNpcCharacter>(OtherNpc)).Deadline=FPlatformTime::Seconds()-1;
    TestTrue(TEXT("失效回调可重入查询并创建下一轮"),Turns->GetLease(OtherNpc,Lease) && bReentered && Lease.Handle==ReentrantHandle);
    TestFalse(TEXT("重入后的旧取消不能删除新轮"),Turns->Cancel(First,ExpiringHandle));
    Turns->OnLeaseExpired.Clear();

    FConversationTestWorld OtherWorld;
    auto* OtherTurns=OtherWorld.World->GetSubsystem<UDeliveryConversationTurnSubsystem>();
    TestTrue(TEXT("每个 World 有独立 epoch"),OtherTurns && OtherTurns->GetWorldEpoch()!=Turns->GetWorldEpoch());
    if (OtherTurns)
    {
        TestEqual(TEXT("拒绝跨 World 的玩家和 NPC"),OtherTurns->TryBegin(First,Npc,FGuid::NewGuid(),5,DuplicateHandle,Replaced),EResult::Invalid);
        TestFalse(TEXT("跨 World 旧 Handle 不释放其他占用"),OtherTurns->Cancel(First,OtherHandle));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryConversationVoiceBoundaryTest,"Delivery.Speech.Conversation.FinalBoundaries",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FDeliveryConversationVoiceBoundaryTest::RunTest(const FString&)
{
    FConversationTestWorld Fixture;
    if (!TestNotNull(TEXT("建立真实 RPC 边界测试世界"),Fixture.World)) return false;
    auto* PC=Fixture.MakePlayer();
    if (!TestTrue(TEXT("发言者具有真实玩家身份"),PC->GetPawn()->IsPlayerControlled())) return false;
    auto* Npc=Fixture.MakeCompanion(PC);
    if (!TestNotNull(TEXT("建立归属当前玩家的连麦同伴"),Npc)) return false;
    auto* Conversation=NewObject<UDeliveryConversationComponent>(PC);
    PC->AddInstanceComponent(Conversation); Conversation->RegisterComponent();
    auto* Turns=Fixture.World->GetSubsystem<UDeliveryConversationTurnSubsystem>();
    if (!TestNotNull(TEXT("可访问真实租约子系统"),Turns)) return false;
    int32 VoiceErrors=0;
    int32 Submitted=0;
    Conversation->OnVoiceInputError.AddLambda([&](FName){ ++VoiceErrors; });
    Conversation->OnSubmitted.AddLambda([&](ADeliveryNpcCharacter*,FGuid,const FString&){ ++Submitted; });
    auto SeedLocal=[&](uint64 Sequence)
    {
        Conversation->ResetLocalVoice();
        Conversation->LocalVoiceToken={FGuid::NewGuid(),1};
        Conversation->LocalVoiceSequence=Sequence;
        Conversation->LastLocalInputSequence=Sequence;
        Conversation->LocalVoiceTarget=Npc;
        Conversation->LocalVoiceContext.Capture(PC);
        Conversation->LocalVoiceDeadline=FPlatformTime::Seconds()+15;
        return Conversation->LocalVoiceToken.Id;
    };

    const auto ClientToken=SeedLocal(1);
    Conversation->ServerBeginVoice_Implementation(Npc,ClientToken,1,0,nullptr);
    const auto Handle=Conversation->ServerVoiceHandle;
    if (!TestTrue(TEXT("按下通过服务器取得 Handle"),Handle.IsValid())) return false;
    Conversation->ServerBeginVoice_Implementation(Npc,ClientToken,1,0,nullptr);
    TestTrue(TEXT("Begin 重发不新建租约"),Conversation->ServerVoiceHandle==Handle);
    Conversation->ServerFinishVoice_Implementation(ClientToken,1);
    Conversation->ServerFinishVoice_Implementation(ClientToken,1);
    Conversation->LastSubmissionId=Handle.Token;
    Conversation->ServerSubmitVoice_Implementation(Npc,TEXT("你好，听见了吗"),Handle);
    FDeliveryConversationTurnLease Lease;
    TestTrue(TEXT("提交后持有回复租约"),Turns->GetLease(Npc,Lease) && Lease.Phase==EDeliveryConversationTurnPhase::AwaitingReply);
    TestTrue(TEXT("多句回复期间保留服务器语音来源"),Conversation->IsVoiceTurn(Handle) && Conversation->IsVoiceReplyMessage(Handle.Token));
    Conversation->ServerSubmitVoice_Implementation(Npc,TEXT("重复的旧 final"),Handle);
    TestTrue(TEXT("直接重复 Deliver 也不会撤销回复"),Conversation->DeliverFinalText(Npc,TEXT("重复的旧 final"),Handle));
    Conversation->ServerCancelVoice_Implementation(ClientToken,1);
    TestTrue(TEXT("松键后的旧 Cancel 不取消已提交的回复"),Turns->GetLease(Npc,Lease) && Lease.Handle==Handle);
    FDeliveryNpcDecisionRequest Request;
    TestTrue(TEXT("成功取得包含发言的真实决策输入"),Npc->Agent->BuildDecisionRequest(FGuid::NewGuid(),Request));
    TestEqual(TEXT("重复 final 只记录一条玩家发言"),Request.Evidence.FilterByPredicate([&](const auto& Item)
        { return Item.Kind==TEXT("PlayerSpeech") && Item.EventId==Handle.Token; }).Num(),1);

    const auto SecondToken=SeedLocal(2);
    Conversation->ServerBeginVoice_Implementation(Npc,SecondToken,2,0,nullptr);
    const auto SecondHandle=Conversation->ServerVoiceHandle;
    TestTrue(TEXT("新轮采用新的服务器编号"),SecondHandle.IsValid() && SecondHandle.Token!=Handle.Token);
    TestFalse(TEXT("新轮替换后旧语音来源被释放"),Conversation->IsVoiceReplyMessage(Handle.Token));
    Conversation->ServerCancelVoice_Implementation(ClientToken,1);
    TestTrue(TEXT("旧 Cancel 不结束新录音"),Turns->GetLease(Npc,Lease) && Lease.Handle==SecondHandle);
    Conversation->ServerFinishVoice_Implementation(SecondToken,2);
    Conversation->NextSendAt=0;
    Conversation->LastSubmissionId=SecondHandle.Token;
    Conversation->ServerSubmitVoice_Implementation(Npc,TEXT("   "),SecondHandle);
    TestFalse(TEXT("空 final 释放本轮占用"),Turns->GetLease(Npc,Lease));
    TestFalse(TEXT("空 final 不残留语音来源"),Conversation->IsVoiceReplyMessage(SecondHandle.Token));
    TestEqual(TEXT("空 final 明确反馈为空"),Conversation->LastResult,FName(TEXT("Empty")));

    const auto ThirdToken=SeedLocal(3);
    Conversation->bVoiceTextReady=true;
    Conversation->FinalVoiceText=TEXT("识别完成但许可尚未返回");
    Conversation->ClientVoiceRejected_Implementation(ThirdToken,3,TEXT("Busy"));
    TestEqual(TEXT("没有活跃 ASR token 也显示服务器拒绝"),Conversation->GetVoiceInputError(),FName(TEXT("Busy")));
    TestEqual(TEXT("拒绝反馈只发一次"),VoiceErrors,1);
    const auto FourthToken=SeedLocal(4);
    Conversation->ClientVoiceRejected_Implementation(ThirdToken,3,TEXT("Expired"));
    TestTrue(TEXT("旧拒绝保留下一轮本机编号"),Conversation->LocalVoiceToken.Id==FourthToken);
    TestEqual(TEXT("旧拒绝不再通知 UI"),VoiceErrors,1);
    Conversation->NextLocalSendAt=FPlatformTime::Seconds()+30;
    TestFalse(TEXT("冷却期间打字发送不被接收"),Conversation->Submit(Npc,TEXT("下一份草稿")));
    TestTrue(TEXT("拒绝本地发送不取消当前录音"),Conversation->LocalVoiceToken.Id==FourthToken);
    TestEqual(TEXT("拒绝发送不触发清草稿事件"),Submitted,0);
    Conversation->LocalVoiceDeadline=FPlatformTime::Seconds()-1;
    Conversation->TickComponent(0,LEVELTICK_All,nullptr);
    TestFalse(TEXT("没有 grant 的输入也会终止等待"),Conversation->LocalVoiceToken.IsValid());
    TestEqual(TEXT("无 grant 等待明确超时"),Conversation->GetVoiceInputError(),FName(TEXT("Expired")));

    const auto DelayedClient=SeedLocal(5);
    Conversation->ServerBeginVoice_Implementation(Npc,DelayedClient,5,0,nullptr);
    const auto DelayedHandle=Conversation->ServerVoiceHandle;
    Conversation->ServerFinishVoice_Implementation(DelayedClient,5);
    // 服务器已建租约并收到结束，延迟向本机交付 grant，复现 final 先到的消息顺序
    Conversation->LocalVoiceHandle={};
    Conversation->NextSendAt=FPlatformTime::Seconds()+.5;
    Conversation->HandleVoiceText(Conversation->LocalVoiceToken,Npc,TEXT("等我说完"));
    TestTrue(TEXT("final 等待 grant 时保留下一句闸门"),Conversation->HasPendingVoiceSubmission());
    TestEqual(TEXT("grant 到达前没有发送 final"),Submitted,0);
    const auto ReentrantSubmit=Conversation->OnSubmitted.AddLambda([&](ADeliveryNpcCharacter*,FGuid,const FString&)
        { Conversation->SubmitReadyVoiceText(); });
    Conversation->ClientVoiceGranted_Implementation(DelayedClient,5,DelayedHandle);
    TestTrue(TEXT("final 已提交但冷却尚未结束仍保持闸门"),Conversation->HasPendingVoiceSubmission() &&
        Conversation->bVoiceSubmissionSent && Conversation->DeferredVoiceHandle==DelayedHandle);
    Conversation->ClientVoiceGranted_Implementation(DelayedClient,5,DelayedHandle);
    TestEqual(TEXT("重复 grant 和提交广播重入均不重复发 final"),Submitted,1);
    Conversation->ClientResult_Implementation(TEXT("Heard"),FGuid::NewGuid());
    TestTrue(TEXT("其他发言的回执不能解除当前等待"),Conversation->HasPendingVoiceSubmission());
    Conversation->NextSendAt=0;
    Conversation->FlushDeferredVoiceSubmission();
    TestFalse(TEXT("服务器真实接收回执解除闸门"),Conversation->HasPendingVoiceSubmission());
    TestFalse(TEXT("同步接收回执清除已发标记"),Conversation->bVoiceSubmissionSent);
    TestEqual(TEXT("限频结束取得真实 Heard 结果"),Conversation->LastResult,FName(TEXT("Heard")));
    FDeliveryNpcDecisionRequest DelayedRequest;
    Npc->Agent->BuildDecisionRequest(DelayedHandle.WorldEpoch,DelayedRequest);
    TestEqual(TEXT("等候许可和限频的原话只进入一次真实上下文"),DelayedRequest.Evidence.FilterByPredicate([&](const auto& Item)
        { return Item.Kind==TEXT("PlayerSpeech") && Item.EventId==DelayedHandle.Token && Item.Description==TEXT("等我说完"); }).Num(),1);
    Conversation->OnSubmitted.Remove(ReentrantSubmit);

    const auto TimedOutClient=SeedLocal(6);
    Conversation->ServerBeginVoice_Implementation(Npc,TimedOutClient,6,0,nullptr);
    Conversation->ServerFinishVoice_Implementation(TimedOutClient,6);
    Conversation->NextSendAt=FPlatformTime::Seconds()+.5;
    Conversation->HandleVoiceText(Conversation->LocalVoiceToken,Npc,TEXT("回执超时的短句"));
    TestTrue(TEXT("本轮 final 仍在等服务器限频"),Conversation->HasPendingVoiceSubmission() && Conversation->DeferredVoiceHandle.IsValid());
    Conversation->LocalVoiceDeadline=FPlatformTime::Seconds()-1;
    Conversation->TickComponent(0,LEVELTICK_All,nullptr);
    TestTrue(TEXT("回执超时同时清除本机闸门和服务器延迟槽"),!Conversation->HasPendingVoiceSubmission() &&
        !Conversation->bVoiceSubmissionSent && !Conversation->DeferredVoiceHandle.IsValid() &&
        !Fixture.World->GetTimerManager().IsTimerActive(Conversation->VoiceSubmitTimer));

    const auto LateClient=SeedLocal(7);
    Conversation->ServerBeginVoice_Implementation(Npc,LateClient,7,0,nullptr);
    const auto LateHandle=Conversation->ServerVoiceHandle;
    Conversation->ServerFinishVoice_Implementation(LateClient,7);
    Conversation->LocalVoiceHandle={};
    Conversation->HandleVoiceText(Conversation->LocalVoiceToken,Npc,TEXT("本机已超时的原句"));
    const int32 BeforeLateGrant=Submitted;
    TestEqual(TEXT("迟到许可测试的服务器期限仍有效"),Turns->ValidateFinal(PC,Npc,LateHandle),EDeliveryConversationTurnResult::Accepted);
    Conversation->LocalVoiceDeadline=FPlatformTime::Seconds()-1;
    Conversation->ClientVoiceGranted_Implementation(LateClient,7,LateHandle);
    TestTrue(TEXT("晚到许可不能抢在Tick前提交已过本机期限的final"),Submitted==BeforeLateGrant &&
        !Conversation->HasPendingVoiceSubmission() && !Turns->GetLease(Npc,Lease));
    TestEqual(TEXT("迟到许可明确报告本机期限已到"),Conversation->GetVoiceInputError(),FName(TEXT("Expired")));
    Conversation->OnVoiceInputError.Clear(); Conversation->OnSubmitted.Clear();
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryConversationVoiceKeyTest,"Delivery.Speech.Conversation.KeyOwnership",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FDeliveryConversationVoiceKeyTest::RunTest(const FString&)
{
    FConversationTestWorld Fixture;
    if (!TestNotNull(TEXT("建立键盘轮次测试世界"),Fixture.World)) return false;
    auto* PC=Fixture.MakePlayer();
    auto* Conversation=NewObject<UDeliveryConversationComponent>(PC);
    PC->AddInstanceComponent(Conversation); Conversation->RegisterComponent();
    auto* Speech=NewObject<UDeliverySpeechInputComponent>(PC);
    PC->AddInstanceComponent(Speech); Speech->RegisterComponent();
    // 只推进真实组件内的纯状态，不打开麦克风、不创建音频线程
    const auto Generation=Speech->TurnState.BeginPreparation();
    if (!TestTrue(TEXT("纯状态准备完成"),Speech->TurnState.MarkReady(Generation))) return false;
    FDeliverySpeechTurnToken Current;
    if (!TestTrue(TEXT("建立模拟按钮录音轮次"),Speech->TurnState.BeginCapture(FPlatformTime::Seconds(),Current))) return false;
    Speech->bTurnStarted=true;
    int32 Cancelled=0;
    bool bStartDuringCancel=false;
    FDeliverySpeechTurnToken Reentrant;
    Speech->OnTurnCancelled.AddLambda([&](const FDeliverySpeechTurnToken&)
    {
        ++Cancelled;
        if (bStartDuringCancel)
        {
            Speech->TurnState.BeginCapture(FPlatformTime::Seconds(),Reentrant);
            Speech->bTurnStarted=true;
            Conversation->KeyVoiceToken=Reentrant;
        }
    });
    const FDeliverySpeechTurnToken OldKey{FGuid::NewGuid(),Generation};
    Conversation->KeyVoiceToken=OldKey;
    Conversation->FinishVoiceFromKey();
    TestTrue(TEXT("旧键松开不结束按钮启动的新轮次"),Speech->IsHolding() && Speech->GetActiveToken()==Current);
    Conversation->KeyVoiceToken=OldKey;
    Conversation->CancelVoiceKeyHold();
    TestTrue(TEXT("旧键取消也保留按钮新轮次"),Speech->IsHolding() && Speech->GetActiveToken()==Current && Cancelled==0);
    Conversation->KeyVoiceToken=Current;
    Conversation->CancelVoiceKeyHold();
    TestTrue(TEXT("V 映射取消也不会结束自动录入"),Speech->IsHolding() && Cancelled==0);
    Conversation->FinishVoiceFromKey();
    TestTrue(TEXT("V 松开不会结束自动录入"),Speech->IsHolding() && Cancelled==0);
    Conversation->CancelVoiceFromKey();
    TestTrue(TEXT("明确取消当前输入后回到准备状态"),Speech->IsVoiceReady() && Cancelled==1);
    Conversation->CancelVoiceKeyHold();
    TestEqual(TEXT("重复取消没有第二份通知"),Cancelled,1);

    Speech->TurnState.BeginCapture(FPlatformTime::Seconds(),Current);
    Speech->bTurnStarted=true;
    Conversation->KeyVoiceToken=Current;
    bStartDuringCancel=true;
    Conversation->CancelVoiceFromKey();
    bStartDuringCancel=false;
    TestTrue(TEXT("取消回调开启的新轮次不被旧处理覆盖"),Reentrant.IsValid() &&
        Speech->GetActiveToken()==Reentrant && Conversation->KeyVoiceToken==Reentrant && Cancelled==2);
    Conversation->KeyVoiceToken={};
    Conversation->CancelVoiceFromKey();
    TestTrue(TEXT("明确取消仍能结束当前输入"),Speech->IsVoiceReady() && Cancelled==3);
    Speech->OnTurnCancelled.Clear();

    // 使用真实 Widget 刷新状态，验证识别阶段不会变成一份可见输入流程
    PC->Player=NewObject<ULocalPlayer>(GEngine);
    auto* Status=CreateWidget<UDeliverySpeechStatusWidget>(PC);
    if (!TestNotNull(TEXT("创建本机连麦状态界面"),Status)) return false;
    Status->VoiceLabel=NewObject<UTextBlock>(Status);
    Status->VoicePreview=NewObject<UTextBlock>(Status);
    Status->VoiceDisableLabel=NewObject<UTextBlock>(Status);
    Status->NativeConstruct();
    // 通过组件关闭，清掉前面取消测试留下的状态文案并通知真实界面
    Speech->DisableVoice();
    Status->RefreshStatus();
    TestTrue(TEXT("未打开文字面板也能发现连麦入口"),Status->GetVisibility()!=ESlateVisibility::Collapsed &&
        Status->VoiceLabel->GetText().ToString()==TEXT("V 与二狗连麦") && Status->VoiceDisableLabel->GetText().ToString()==TEXT("连麦"));
    const auto UiGeneration=Speech->TurnState.BeginPreparation();
    Speech->TurnState.MarkReady(UiGeneration);
    Speech->TurnState.BeginCapture(FPlatformTime::Seconds(),Current);
    Speech->PartialText=TEXT("只留在内部的识别草稿");
    Status->VoicePreview->SetText(FText::FromString(Speech->PartialText));
    Status->RefreshStatus();
    TestTrue(TEXT("说话时只显示连麦状态"),Status->VoiceLabel->GetText().ToString().Contains(TEXT("连麦中")));
    TestTrue(TEXT("旧预览内容被清除并隐藏"),Status->VoicePreview->GetText().IsEmpty() &&
        Status->VoicePreview->GetVisibility()==ESlateVisibility::Collapsed);
    Speech->TurnState.FinishCapture(Current,FPlatformTime::Seconds());
    Status->RefreshStatus();
    TestTrue(TEXT("收尾不显示识别和按住发送流程"),Status->VoiceLabel->GetText().ToString().Contains(TEXT("连麦中")) &&
        !Status->VoiceLabel->GetText().ToString().Contains(TEXT("识别")));
    Status->NativeDestruct();
    Speech->CancelHold();
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryConversationVoiceTargetTest,"Delivery.Speech.Conversation.VoiceTarget",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryConversationVoiceTargetTest::RunTest(const FString&)
{
    FConversationTestWorld Fixture;
    if (!TestNotNull(TEXT("建立连麦目标测试世界"),Fixture.World)) return false;
    auto* PC=Fixture.MakePlayer();
    auto* Other=Fixture.MakePlayer();
    auto* Conversation=NewObject<UDeliveryConversationComponent>(PC);
    PC->AddInstanceComponent(Conversation); Conversation->RegisterComponent();
    auto* Nearby=Fixture.MakeNpc();
    Conversation->SetConversationTarget(Nearby);
    TestNull(TEXT("没有同伴时不使用文字面板中的居民"),Conversation->ResolveVoiceCompanion());
    auto* Companion=Fixture.MakeCompanion(PC,100);
    auto* ForeignCompanion=Fixture.MakeCompanion(Other,200);
    if (!TestNotNull(TEXT("建立主人二狗"),Companion) || !TestNotNull(TEXT("建立其他玩家二狗"),ForeignCompanion)) return false;
    TestTrue(TEXT("默认目标按 PlayerState 归属选择"),Conversation->ResolveVoiceCompanion()==Companion);
    Companion->SetActorLocation(FVector(20000,0,90));
    TestFalse(TEXT("远处同伴不能被当作近距离文字交谈"),Conversation->CanTalkTo(Companion));
    TestTrue(TEXT("跑腿远处仍可通过队友连麦校验"),Conversation->CanVoiceTalkTo(Companion));
    TestFalse(TEXT("其他玩家的同伴不能冒充自己的连麦对象"),Conversation->CanVoiceTalkTo(ForeignCompanion));
    TestFalse(TEXT("普通居民不能通过连麦权限"),Conversation->CanVoiceTalkTo(Nearby));
    ForeignCompanion->SetActorLocation(FVector(30000,30000,90));
    Other->GetPawn()->SetActorLocation(FVector(10000,10000,90));
    auto* FurtherNeighbor=Fixture.MakeNpc(300);
    auto* CompanionNeighbor=Fixture.MakeNpc(500);
    CompanionNeighbor->SetActorLocation(FVector(20200,0,90));
    Conversation->VoiceOverhearLimit=1;
    uint64 Sequence=0;
    auto BeginVoice=[&](ADeliveryNpcCharacter* Target)
    {
        Conversation->ResetLocalVoice();
        Conversation->LocalVoiceToken={FGuid::NewGuid(),1};
        Conversation->LocalVoiceSequence=++Sequence;
        Conversation->LastLocalInputSequence=Sequence;
        Conversation->LocalVoiceTarget=Target;
        Conversation->LocalVoiceContext.Capture(PC);
        Conversation->LocalVoiceDeadline=FPlatformTime::Seconds()+15;
        Conversation->ServerBeginVoice_Implementation(Target,Conversation->LocalVoiceToken.Id,Sequence,0,nullptr);
        return Conversation->ServerVoiceHandle;
    };
    TestFalse(TEXT("普通居民的远程 Begin 被服务器拒绝"),BeginVoice(Nearby).IsValid());
    TestFalse(TEXT("非本玩家二狗的 Begin 被服务器拒绝"),BeginVoice(ForeignCompanion).IsValid());
    const auto Remote=BeginVoice(Companion);
    if (!TestTrue(TEXT("远程主人取得真实语音租约"),Remote.IsValid())) return false;
    const FGuid RemoteClient=Conversation->ServerVoiceClientToken;
    Conversation->ServerFinishVoice_Implementation(RemoteClient,Sequence);
    Conversation->LastSubmissionId=Remote.Token;
    // 允许真实请求入队，但不推进世界或 Gateway Tick，也不启动外部服务
    GetMutableDefault<UDeliveryAiSettings>()->bEnabled=true;
    Conversation->ServerSubmitVoice_Implementation(Companion,TEXT("二狗，先听我说"),Remote);
    TestTrue(TEXT("连麦不会生成玩家转身意图"),Conversation->LastLocalFacingSequence==0 && Conversation->LastServerFacingSequence==0);
    TestTrue(TEXT("只有直连二狗发起模型请求"),Companion->Agent->IsDecisionPending() &&
        !Nearby->Agent->IsDecisionPending() && !FurtherNeighbor->Agent->IsDecisionPending() && !CompanionNeighbor->Agent->IsDecisionPending());
    GetMutableDefault<UDeliveryAiSettings>()->bEnabled=false;
    FDeliveryNpcDecisionRequest Direct,Heard,Further,Far;
    Companion->Agent->BuildDecisionRequest(Remote.WorldEpoch,Direct);
    Nearby->Agent->BuildDecisionRequest(Remote.WorldEpoch,Heard);
    FurtherNeighbor->Agent->BuildDecisionRequest(Remote.WorldEpoch,Further);
    CompanionNeighbor->Agent->BuildDecisionRequest(Remote.WorldEpoch,Far);
    TestTrue(TEXT("远程 PlayerSpeech 仍绑定真实主人"),Direct.RequesterPC==PC && Direct.RequesterPlayerState==PC->PlayerState &&
        Direct.PlayerMessageId==Remote.Token && Direct.Evidence.ContainsByPredicate([&](const auto& Fact)
        { return Fact.EventId==Remote.Token && Fact.Kind==TEXT("PlayerSpeech"); }));
    TestTrue(TEXT("最近旁人只保存无需回复的听闻事实"),Heard.Evidence.ContainsByPredicate([&](const auto& Fact)
        { return Fact.EventId==Remote.Token && Fact.Kind==TEXT("OverheardSpeech") && !Fact.bNeedsReply; }));
    TestFalse(TEXT("旁听人数受限"),Further.Evidence.ContainsByPredicate([&](const auto& Fact){ return Fact.EventId==Remote.Token; }));
    TestFalse(TEXT("二狗身边的远处居民听不到玩家原话"),Far.Evidence.ContainsByPredicate([&](const auto& Fact){ return Fact.EventId==Remote.Token; }));
    TestTrue(TEXT("语音来源关联实际二狗"),Conversation->IsVoiceReplyMessage(Remote.Token,Companion) &&
        !Conversation->IsVoiceReplyMessage(Remote.Token,ForeignCompanion));
    const auto Short=BeginVoice(Companion);
    Conversation->ServerCancelVoice_Implementation(RemoteClient,Remote.InputSequence);
    TestTrue(TEXT("旧取消不能释放新的连麦轮次"),Conversation->ServerVoiceHandle==Short);
    Conversation->ServerFinishVoice_Implementation(Conversation->ServerVoiceClientToken,Short.InputSequence);
    Conversation->LastSubmissionId=Short.Token;
    Conversation->NextSendAt=FPlatformTime::Seconds()+.5;
    Conversation->ServerSubmitVoice_Implementation(Companion,TEXT("停"),Short);
    TestTrue(TEXT("快速单字保留在有界延迟槽中"),Conversation->DeferredVoiceHandle==Short && Conversation->DeferredVoiceText==TEXT("停"));
    Conversation->ServerSubmitVoice_Implementation(Companion,TEXT("重复包不能覆盖"),Short);
    TestEqual(TEXT("同轮重发不覆盖待提交原话"),Conversation->DeferredVoiceText,FString(TEXT("停")));
    Conversation->NextSendAt=0;
    Conversation->FlushDeferredVoiceSubmission();
    TestFalse(TEXT("提交后清空延迟槽"),Conversation->DeferredVoiceHandle.IsValid());
    FDeliveryNpcDecisionRequest ShortRequest;
    Companion->Agent->BuildDecisionRequest(Short.WorldEpoch,ShortRequest);
    TestTrue(TEXT("等候限频后完整交付单字短句"),ShortRequest.Evidence.ContainsByPredicate([&](const auto& Fact)
        { return Fact.EventId==Short.Token && Fact.Description==TEXT("停"); }));
    Companion->Agent->InterruptPlayerReply(PC);
    TestFalse(TEXT("真实回复取消释放来源记录"),Conversation->IsVoiceReplyMessage(Short.Token));
    const auto CancelledTurn=BeginVoice(Companion);
    const auto CancelledClient=Conversation->ServerVoiceClientToken;
    Conversation->ServerFinishVoice_Implementation(CancelledClient,CancelledTurn.InputSequence);
    Conversation->NextSendAt=FPlatformTime::Seconds()+.5;
    Conversation->ServerSubmitVoice_Implementation(Companion,TEXT("等一下"),CancelledTurn);
    Conversation->ServerCancelVoice_Implementation(CancelledClient,CancelledTurn.InputSequence);
    TestTrue(TEXT("取消清除延迟文字和定时器"),!Conversation->DeferredVoiceHandle.IsValid() && Conversation->DeferredVoiceText.IsEmpty() &&
        !Fixture.World->GetTimerManager().IsTimerActive(Conversation->VoiceSubmitTimer));
    Conversation->LocalVoiceToken={FGuid::NewGuid(),1};
    Conversation->LocalVoiceTarget=Companion;
    const auto Token=Conversation->LocalVoiceToken;
    Conversation->ClearConversationTarget(Nearby);
    TestTrue(TEXT("关闭文字面板不清除独立语音轮次"),Conversation->LocalVoiceToken==Token);
    Conversation->SetConversationTarget(ForeignCompanion);
    TestTrue(TEXT("改变文字对象不改变连麦目标"),Conversation->ResolveVoiceCompanion()==Companion && Conversation->LocalVoiceToken==Token);
    Conversation->ResetLocalVoice();
    PC->UnPossess();
    TestFalse(TEXT("失去控制对象后不接受新语音"),Conversation->CanVoiceTalkTo(Companion));
    PC->Possess(Fixture.MakePawn());
    auto* Property=FindFProperty<FObjectPropertyBase>(UDeliveryCompanionComponent::StaticClass(),TEXT("OwnerPlayerState"));
    const auto ChangedOwnerTurn=BeginVoice(Companion);
    const auto ChangedClient=Conversation->ServerVoiceClientToken;
    Conversation->ServerFinishVoice_Implementation(ChangedClient,ChangedOwnerTurn.InputSequence);
    Conversation->LastSubmissionId=ChangedOwnerTurn.Token;
    Conversation->NextSendAt=FPlatformTime::Seconds()+.5;
    Conversation->ServerSubmitVoice_Implementation(Companion,TEXT("归属变化前尚未送达的话"),ChangedOwnerTurn);
    Property->SetObjectPropertyValue_InContainer(Companion->Companion,Other->PlayerState);
    TestFalse(TEXT("归属变更立即使旧目标失效"),Conversation->CanVoiceTalkTo(Companion));
    Conversation->NextSendAt=0;
    Conversation->FlushDeferredVoiceSubmission();
    TestTrue(TEXT("归属变化后拒绝延迟的旧录音且不改成普通对话"),!Conversation->ServerVoiceHandle.IsValid() &&
        Conversation->LastResult==TEXT("Unavailable") && !Conversation->IsVoiceReplyMessage(ChangedOwnerTurn.Token));
    TestNull(TEXT("归属缓存失效后不会返回眼前其他人物"),Conversation->ResolveVoiceCompanion());
    Property->SetObjectPropertyValue_InContainer(Companion->Companion,PC->PlayerState);
    TestTrue(TEXT("重新查询可找到有效归属"),Conversation->ResolveVoiceCompanion()==Companion);
    Companion->Destroy();
    TestNull(TEXT("目标销毁不留下缓存"),Conversation->ResolveVoiceCompanion());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryConversationVehicleVoiceTest,"Delivery.Speech.Conversation.VehicleContinuity",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryConversationVehicleVoiceTest::RunTest(const FString&)
{
    FConversationTestWorld Fixture;
    if (!TestNotNull(TEXT("建立上下车语音测试世界"),Fixture.World)) return false;
    auto* PC=Fixture.MakePlayer();
    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Character=Fixture.World->SpawnActor<ADeliveryCharacter>(FVector(0,0,100),FRotator::ZeroRotator,Spawn);
    auto* Bike=Fixture.World->SpawnActor<ADeliveryMotorbike>(FVector(200,0,100),FRotator::ZeroRotator,Spawn);
    if (!TestNotNull(TEXT("建立原角色"),Character) || !TestNotNull(TEXT("建立真实车辆"),Bike)) return false;
    Character->SetActorTickEnabled(false); Bike->SetActorTickEnabled(false);
    PC->Possess(Character);
    auto* Npc=Fixture.MakeCompanion(PC,400);
    auto* Conversation=NewObject<UDeliveryConversationComponent>(PC);
    PC->AddInstanceComponent(Conversation); Conversation->RegisterComponent();
    auto* Turns=Fixture.World->GetSubsystem<UDeliveryConversationTurnSubsystem>();
    auto* DriverProperty=FindFProperty<FObjectPropertyBase>(ADeliveryMotorbike::StaticClass(),TEXT("Driver"));
    if (!TestNotNull(TEXT("取得车辆驾驶员复制字段"),DriverProperty) || !Npc || !Turns) return false;
    // 只准备服务器已完成交接后的关系，使用真实 Possess，不冒充 F 上下车操作验收
    auto Enter=[&]()
    {
        DriverProperty->SetObjectPropertyValue_InContainer(Bike,Character);
        Character->AttachToActor(Bike,FAttachmentTransformRules::KeepWorldTransform);
        PC->Possess(Bike);
    };
    auto Exit=[&]()
    {
        DriverProperty->SetObjectPropertyValue_InContainer(Bike,nullptr);
        Character->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
        PC->Possess(Character);
    };
    uint64 Sequence=0;
    int32 Submitted=0;
    Conversation->OnSubmitted.AddLambda([&](ADeliveryNpcCharacter*,FGuid,const FString&){ ++Submitted; });
    auto Begin=[&]()
    {
        Conversation->ResetLocalVoice();
        Conversation->LocalVoiceToken={FGuid::NewGuid(),1};
        Conversation->LocalVoiceSequence=++Sequence;
        Conversation->LastLocalInputSequence=Sequence;
        Conversation->LocalVoiceTarget=Npc;
        Conversation->LocalVoiceContext.Capture(PC);
        Conversation->LocalVoiceDeadline=FPlatformTime::Seconds()+15;
        Conversation->ServerBeginVoice_Implementation(Npc,Conversation->LocalVoiceToken.Id,Sequence,0,nullptr);
        return Conversation->ServerVoiceHandle;
    };
    FDeliveryConversationTurnLease Lease;
    const auto Walking=Begin();
    if (!TestTrue(TEXT("步行取得真实语音租约"),Walking.IsValid() && Turns->GetLease(Npc,Lease))) return false;
    const double RecordingDeadline=Lease.Deadline;
    Enter();
    TestTrue(TEXT("上车后保存同一原角色与录音编号"),Turns->GetLease(Npc,Lease) && Lease.Handle==Walking &&
        Lease.Deadline==RecordingDeadline && Conversation->LocalVoiceContext.Check(PC)==EDeliverySpeechPlayerContextState::Valid);
    TestTrue(TEXT("驾驶状态仍允许与主人二狗连麦"),Conversation->CanVoiceTalkTo(Npc));
    Conversation->HandleVoiceReleased(Conversation->LocalVoiceToken);
    Conversation->NextSendAt=0;
    Conversation->HandleVoiceText(Conversation->LocalVoiceToken,Npc,TEXT("这句话从步行说到车上"));
    TestTrue(TEXT("跨上车的整句话进入回复阶段"),Turns->GetLease(Npc,Lease) && Lease.Handle==Walking &&
        Lease.Phase==EDeliveryConversationTurnPhase::AwaitingReply && Conversation->IsVoiceReplyMessage(Walking.Token,Npc));
    const double ReplyDeadline=Lease.Deadline;
    Exit();
    TestTrue(TEXT("等待回复时下车保留原回复与期限"),Turns->GetLease(Npc,Lease) && Lease.Handle==Walking &&
        Lease.Deadline==ReplyDeadline && Conversation->IsVoiceReplyMessage(Walking.Token,Npc));

    Enter();
    const auto Driving=Begin();
    Conversation->HandleVoiceReleased(Conversation->LocalVoiceToken);
    Turns->GetLease(Npc,Lease);
    const double FinalDeadline=Lease.Deadline;
    const int32 BeforePending=Submitted;
    PC->UnPossess();
    TestEqual(TEXT("短暂控制器空窗只表示等待"),Turns->GetVoiceContextState(PC,Driving),EDeliverySpeechPlayerContextState::Pending);
    TestFalse(TEXT("身份未完整时不能把消息作为合法远程对白"),Conversation->IsVoiceReplyMessage(Driving.Token,Npc));
    Conversation->HandleVoiceText(Conversation->LocalVoiceToken,Npc,TEXT("下车时的句尾仍保留"));
    TestTrue(TEXT("final 在空窗期间保留且尚未提交"),Conversation->HasPendingVoiceSubmission() &&
        !Conversation->bVoiceSubmissionSent && Submitted==BeforePending && Turns->GetLease(Npc,Lease) && Lease.Deadline==FinalDeadline);
    Exit();
    Conversation->NextSendAt=FPlatformTime::Seconds()+.5;
    Conversation->SubmitReadyVoiceText();
    TestTrue(TEXT("回调中身份恢复后立即清过渡计时，不让旧计时取消待回执final"),
        Conversation->LocalVoiceContextPendingSince<0 && Conversation->DeferredVoiceHandle==Driving);
    Conversation->NextSendAt=0;
    Conversation->FlushDeferredVoiceSubmission();
    TestTrue(TEXT("同原角色控制恢复后提交原句一次"),Submitted==BeforePending+1 &&
        !Conversation->HasPendingVoiceSubmission() && Conversation->IsVoiceReplyMessage(Driving.Token,Npc));

    const auto InFlight=Begin();
    const auto InFlightClient=Conversation->ServerVoiceClientToken;
    PC->UnPossess();
    Conversation->ServerFinishVoice_Implementation(InFlightClient,InFlight.InputSequence);
    TestTrue(TEXT("已建立轮次在空窗收到结束仍启动五秒期限"),Turns->GetLease(Npc,Lease) &&
        Lease.Phase==EDeliveryConversationTurnPhase::FinalPending);
    const double InFlightDeadline=Lease.Deadline;
    Conversation->ServerFinishVoice_Implementation(InFlightClient,InFlight.InputSequence);
    TestTrue(TEXT("重复结束不会延长等待期限"),Turns->GetLease(Npc,Lease) && Lease.Deadline==InFlightDeadline);
    TestEqual(TEXT("空窗禁止直接校验最终文本"),Turns->ValidateFinal(PC,Npc,InFlight),EDeliveryConversationTurnResult::Stale);
    Conversation->ServerSubmitVoice_Implementation(Npc,TEXT("已在网络中的原句"),InFlight);
    TestTrue(TEXT("已到服务器的原句进入唯一有界待发槽"),Conversation->DeferredVoiceHandle==InFlight &&
        Turns->GetLease(Npc,Lease) && Lease.Deadline==InFlightDeadline);
    PC->Possess(Character);
    Conversation->NextSendAt=0;
    Conversation->FlushDeferredVoiceSubmission();
    TestTrue(TEXT("身份恢复后原句只进入一次正式发言上下文"),Conversation->IsVoiceReplyMessage(InFlight.Token,Npc));
    FDeliveryNpcDecisionRequest Request;
    Npc->Agent->BuildDecisionRequest(InFlight.WorldEpoch,Request);
    TestEqual(TEXT("网络中原句不重复"),Request.Evidence.FilterByPredicate([&](const auto& Fact)
        { return Fact.EventId==InFlight.Token && Fact.Description==TEXT("已在网络中的原句"); }).Num(),1);

    Begin();
    Conversation->HandleVoiceReleased(Conversation->LocalVoiceToken);
    PC->UnPossess();
    Conversation->HandleVoiceText(Conversation->LocalVoiceToken,Npc,TEXT("两秒后才恢复的本机原句"));
    TestTrue(TEXT("第一次遇到复制空窗立即记录起点"),Conversation->LocalVoiceContextPendingSince>=0);
    Conversation->LocalVoiceContextPendingSince=FPlatformTime::Seconds()-FDeliverySpeechPlayerContext::TransitionGraceSeconds-.01;
    PC->Possess(Character);
    const int32 BeforeLateRestore=Submitted;
    Conversation->SubmitReadyVoiceText();
    TestTrue(TEXT("本机长帧后恢复Valid也不能绕过已到的两秒期限"),Submitted==BeforeLateRestore &&
        !Conversation->HasPendingVoiceSubmission() && !Turns->GetLease(Npc,Lease));

    const auto Expiring=Begin();
    PC->UnPossess();
    TestTrue(TEXT("第一次缺少控制对象保留有限等待"),Turns->GetLease(Npc,Lease));
    Turns->Leases.FindChecked(TWeakObjectPtr<ADeliveryNpcCharacter>(Npc)).VoiceContextPendingSince=
        FPlatformTime::Seconds()-FDeliverySpeechPlayerContext::TransitionGraceSeconds-.01;
    PC->Possess(Character);
    TestFalse(TEXT("空窗超过两秒后才恢复控制也释放旧占用"),Turns->GetLease(Npc,Lease));
    TestFalse(TEXT("超时后恢复控制也不会复活旧语音"),Conversation->IsVoiceReplyMessage(Expiring.Token,Npc));

    const auto Replaced=Begin();
    PC->Possess(Fixture.MakePawn());
    TestFalse(TEXT("同PC换成另一人物立即释放原轮次"),Turns->GetLease(Npc,Lease));
    TestFalse(TEXT("换人不继承旧语音来源"),Conversation->IsVoiceReplyMessage(Replaced.Token,Npc));
    PC->Possess(Character);
    const auto Foreign=Begin();
    Enter();
    auto* OtherCharacter=Fixture.World->SpawnActor<ADeliveryCharacter>(FVector(800,0,100),FRotator::ZeroRotator,Spawn);
    DriverProperty->SetObjectPropertyValue_InContainer(Bike,OtherCharacter);
    TestFalse(TEXT("车辆明确属于其他驾驶员时不使用宽限"),Turns->GetLease(Npc,Lease));
    TestFalse(TEXT("其他驾驶员车辆不认作原玩家连麦"),Conversation->IsVoiceReplyMessage(Foreign.Token,Npc));
    DriverProperty->SetObjectPropertyValue_InContainer(Bike,Character);
    Exit();
    Enter();
    const auto Destroyed=Begin();
    Character->Destroy();
    TestFalse(TEXT("车仍受控但原角色销毁时旧回复失效"),Turns->GetLease(Npc,Lease));
    TestFalse(TEXT("原角色弱引用失效不能回退到仅比较车辆Pawn"),Conversation->IsVoiceReplyMessage(Destroyed.Token,Npc));
    Conversation->OnSubmitted.Clear();
    return true;
}
#endif
