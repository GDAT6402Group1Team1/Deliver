#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "Character/DeliveryCharacter.h"
#include "Combat/DeliveryCombatEvents.h"
#include "Components/SkeletalMeshComponent.h"
#include "Game/DeliveryGameState.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "Player/DeliverPlayerState.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryCompanionAssistTest,"Delivery.AI.Companion.ConfirmedHitAssistAuthority",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FDeliveryCompanionAssistTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
    const bool bEnabled=Settings->bEnabled;
    Settings->bEnabled=false;
    ON_SCOPE_EXIT { Settings->bEnabled=bEnabled; };
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    if (!TestNotNull(TEXT("创建护主规则世界"),World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit);
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
    };
    World->SetGameState(World->SpawnActor<ADeliveryGameState>());
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();
    auto* PlayerClass=LoadClass<ADeliveryCharacter>(nullptr,
        TEXT("/Game/Blueprint/Character/DeliverMan/BP_DeliveryMan.BP_DeliveryMan_C"));
    if (!TestNotNull(TEXT("正式玩家蓝图包含护主事件所需的角色配置"),PlayerClass)) return false;
    const auto* PlayerMesh=PlayerClass->GetDefaultObject<ADeliveryCharacter>()->GetMesh();
    if (!TestNotNull(TEXT("玩家蓝图具有物理资产"),PlayerMesh->GetPhysicsAsset())) return false;
    const auto MakePlayer=[&](const FVector& Location)
    {
        auto* PC=World->SpawnActor<APlayerController>();
        auto* State=World->SpawnActor<ADeliverPlayerState>();
        State->SetOwner(PC); PC->PlayerState=State;
        FActorSpawnParameters Spawn;
        Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        // 玩家会在 BeginPlay 启动布娃娃，因此从正式蓝图创建完整的骨骼和物理配置
        auto* Character=World->SpawnActor<ADeliveryCharacter>(PlayerClass,Location,FRotator::ZeroRotator,Spawn);
        PC->Possess(Character);
        Character->GetActiveRagdoll()->StopRagdoll();
        Character->SetActorLocation(Location,false,nullptr,ETeleportType::TeleportPhysics);
        return Character;
    };
    auto* Owner=MakePlayer(FVector(0,0,88));
    auto* Visitor=MakePlayer(FVector(0,400,88));
    auto* OwnerState=Owner->GetPlayerState<ADeliverPlayerState>();
    auto* Npc=World->SpawnActor<ADeliveryCompanionCharacter>(FVector(150,0,88),FRotator::ZeroRotator);
    auto* Target=World->SpawnActor<ADeliveryNpcCharacter>(FVector(300,0,88),FRotator::ZeroRotator);
    if (!TestNotNull(TEXT("创建同伴"),Npc) || !TestNotNull(TEXT("创建受击居民"),Target)) return false;
    auto* Companion=Npc->Companion.Get();
    // 已结识是本测试前置；这里只验证真实 ASC 事件路由和规则权限，不代替挥拳接触或扶起试玩
    Npc->Encounter.Phase=EDeliveryCompanionEncounterPhase::Joined;
    Companion->OwnerPlayerState=OwnerState;
    Companion->Mode=EDeliveryCompanionMode::Follow;
    Companion->Affinity=Companion->GetTuning().InitialAffinity;
    Companion->RecordRecruitMemory();
    Npc->OnRep_Encounter();
    Companion->RefreshOwnerCombatSubscription();
    if (!TestTrue(TEXT("订阅真实主人 ASC"),Companion->BoundOwnerCombat.IsValid())) return false;
    TestTrue(TEXT("二狗拳头不会击倒任何人"),Npc->Combat->bHarmlessStrikes);
    TestTrue(TEXT("二狗默认一拳倒地"),Npc->Combat->bKnockDownOnAnyPunch);
    TestTrue(TEXT("二狗允许编号去重的倒地再命中"),Npc->Combat->bReceiveHitsWhileDown);

    DeliveryCombatEvents::ReportHit(Visitor,Target,FGuid::NewGuid(),Target->GetActorLocation());
    TestNull(TEXT("旁人的交战不建立主人护主目标"),Companion->GetAssistTarget());
    const FGuid ConfirmedPunch=FGuid::NewGuid();
    DeliveryCombatEvents::ReportHit(Owner,Target,ConfirmedPunch,Target->GetActorLocation());
    if (!TestEqual(TEXT("主人实际命中事件把对方登记为目标"),Companion->GetAssistTarget(),static_cast<AActor*>(Target))) return false;
    const int32 Revision=Companion->GetAssistRevision();
    TestTrue(TEXT("当前规则可以授权 Fight"),Npc->CanExecuteGameplayRule(EDeliveryNpcAction::Fight,Target,Revision));
    TestFalse(TEXT("旧规则版本不能发拳"),Npc->CanExecuteGameplayRule(EDeliveryNpcAction::Fight,Target,Revision-1));
    TestFalse(TEXT("模型仍不能选择二狗 Fight"),Npc->CanExecuteForRequester(EDeliveryNpcAction::Fight,Target,OwnerState));
    TestFalse(TEXT("访客不能伪装护主命令"),Npc->CanExecuteForRequester(EDeliveryNpcAction::Fight,Target,Visitor->GetPlayerState()));
    TestTrue(TEXT("真实证据走专用 Agent 规则采纳入口"),Npc->Agent->RequestGameplayRuleAction(
        EDeliveryNpcAction::Fight,Target,Revision,ConfirmedPunch,5));
    const auto Accepted=Npc->Agent->GetPublicState();
    TestEqual(TEXT("来源明确标记为游戏规则"),Accepted.Origin,EDeliveryNpcActionOrigin::GameplayRule);
    TestFalse(TEXT("同代次不能每帧重启交战"),Npc->Agent->RequestGameplayRuleAction(
        EDeliveryNpcAction::Fight,Target,Revision,ConfirmedPunch,5));
    DeliveryCombatEvents::ReportHit(Owner,Target,ConfirmedPunch,Target->GetActorLocation());
    TestEqual(TEXT("重复同一拳不改护主代次"),Companion->GetAssistRevision(),Revision);
    TestEqual(TEXT("重复同一拳不换已接动作"),Npc->Agent->GetPublicState().ActionId,Accepted.ActionId);

    TestTrue(TEXT("主人有效等候命令停止护主"),Companion->ApplyCommand(EDeliveryNpcAction::Stay,OwnerState,Companion->GetCommandRevision()));
    TestNull(TEXT("等候清掉护主目标"),Companion->GetAssistTarget());
    TestFalse(TEXT("等候结束原规则动作"),Npc->Agent->HasActiveAction());
    DeliveryCombatEvents::ReportHit(Owner,Target,ConfirmedPunch,Target->GetActorLocation());
    TestNull(TEXT("取消后重放旧拳不会重新开打"),Companion->GetAssistTarget());
    const int32 BeforeOwnerHit=Companion->GetAffinity();
    const FGuid OwnerHit=FGuid::NewGuid();
    DeliveryCombatEvents::ReportHit(Owner,Npc,OwnerHit,Npc->GetActorLocation());
    TestNull(TEXT("主人打二狗时不会反击主人"),Companion->GetAssistTarget());
    TestEqual(TEXT("真实主人命中事件改变亲密度"),Companion->GetAffinity(),BeforeOwnerHit-Companion->GetTuning().OwnerHitAffinityLoss);
    const int32 AfterOwnerHit=Companion->GetAffinity();
    DeliveryCombatEvents::ReportHit(Owner,Npc,OwnerHit,Npc->GetActorLocation());
    TestEqual(TEXT("重复命中事件不再次扣亲密度"),Companion->GetAffinity(),AfterOwnerHit);
    FDeliveryNpcDecisionRequest MemoryRequest;
    Companion->ExtendDecisionRequest(MemoryRequest,OwnerState);
    TestTrue(TEXT("记忆保留服务器确认的命中编号"),MemoryRequest.CompanionMemories.ContainsByPredicate(
        [OwnerHit](const auto& Item){ return Item.EventId==OwnerHit && Item.Kind==TEXT("OwnerHit"); }));
    TestFalse(TEXT("历史记忆不补成当前行动证据"),MemoryRequest.Evidence.ContainsByPredicate(
        [OwnerHit](const auto& Item){ return Item.EventId==OwnerHit; }));
    TestFalse(TEXT("模型请求不暴露亲密度数字字段"),DeliveryNpc::EncodeRequest(MemoryRequest).Contains(TEXT("affinity")));

    auto* SameOwner=World->SpawnActor<ADeliveryCompanionCharacter>(FVector(350,0,88),FRotator::ZeroRotator);
    SameOwner->Companion->OwnerPlayerState=OwnerState;
    DeliveryCombatEvents::ReportHit(Owner,SameOwner,FGuid::NewGuid(),SameOwner->GetActorLocation());
    TestNull(TEXT("同属主同伴不能成为目标"),Companion->GetAssistTarget());

    DeliveryCombatEvents::ReportHit(Visitor,Owner,FGuid::NewGuid(),Owner->GetActorLocation());
    TestEqual(TEXT("主人被另一玩家实际命中会帮助主人"),Companion->GetAssistTarget(),static_cast<AActor*>(Visitor));
    const int32 DefenceRevision=Companion->GetAssistRevision();
    Npc->Agent->BeginListening(Owner,FGuid::NewGuid());
    Companion->UpdateAssist();
    TestEqual(TEXT("玩家说话暂停但保留当前目标"),Companion->GetAssistRevision(),DefenceRevision);
    TestTrue(TEXT("护主重试没有抢走正在倾听的玩家"),Npc->Agent->IsListening());
    Npc->Agent->CancelPlan(TEXT("TestListeningEnded"));
    TestTrue(TEXT("测试倒地前置"),Npc->Combat->SetDeliberateProne(true));
    Companion->UpdateAssist();
    TestEqual(TEXT("倒地暂停仍保留对手"),Companion->GetAssistTarget(),static_cast<AActor*>(Visitor));
    TestFalse(TEXT("倒地时规则不可执行"),Npc->CanExecuteGameplayRule(EDeliveryNpcAction::Fight,Visitor,DefenceRevision));
    TestTrue(TEXT("结束测试倒地前置"),Npc->Combat->SetDeliberateProne(false));
    TestTrue(TEXT("起身后原规则在期限内继续有效"),Npc->CanExecuteGameplayRule(EDeliveryNpcAction::Fight,Visitor,DefenceRevision));

    // 控制对象变化只复核身份，不重新绑定或丢掉 PlayerState 上的事件订阅
    auto* PC=OwnerState->GetPlayerController();
    const auto BoundASC=Companion->BoundOwnerCombat;
    PC->UnPossess();
    Companion->RefreshOwnerCombatSubscription();
    TestEqual(TEXT("换 Pawn 的空档保留同一个主人"),Companion->GetOwnerPlayerState(),static_cast<APlayerState*>(OwnerState));
    TestEqual(TEXT("换 Pawn 不重建主人 ASC 绑定"),Companion->BoundOwnerCombat.Get(),BoundASC.Get());
    PC->Possess(Owner);
    Visitor->Destroy();
    TestNull(TEXT("目标销毁立即撤销目标"),Companion->GetAssistTarget());
    DeliveryCombatEvents::ReportHit(Owner,Target,FGuid::NewGuid(),Target->GetActorLocation());
    Companion->AssistExpiresAt=World->GetTimeSeconds();
    Companion->UpdateAssist();
    TestNull(TEXT("到期旧事件不能无限续打"),Companion->GetAssistTarget());
    Companion->ReleaseOwner();
    TestTrue(TEXT("解绑后清理旧主人的私有当局记忆"),Companion->Memories.IsEmpty());
    TestFalse(TEXT("解绑主人时移除 ASC 事件订阅"),Companion->BoundOwnerCombat.IsValid());
    DeliveryCombatEvents::ReportHit(Owner,Target,FGuid::NewGuid(),Target->GetActorLocation());
    TestNull(TEXT("旧主人之后的命中不再指挥同伴"),Companion->GetAssistTarget());
    return true;
}
#endif
