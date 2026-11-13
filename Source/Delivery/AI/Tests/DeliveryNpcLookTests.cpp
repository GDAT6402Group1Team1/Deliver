#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Presentation/DeliveryNpcLookComponent.h"
#include "AI/Presentation/DeliveryNpcExpressionSet.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcLookTest,"Delivery.AI.PlayerGlance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryNpcLookTest::RunTest(const FString&)
{
    const auto Settings=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Settings);
    if (!TestNotNull(TEXT("注视测试世界"),World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT { World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
    World->InitializeActorsForPlay(FURL()); World->GetWorldSettings()->NotifyBeginPlay();
    auto* Owner=World->SpawnActor<AActor>();
    auto* Body=NewObject<UCapsuleComponent>(Owner);
    Body->InitCapsuleSize(30,70); Owner->SetRootComponent(Body); Body->RegisterComponent();
    auto* Look=NewObject<UDeliveryNpcLookComponent>(Owner);
    Owner->AddInstanceComponent(Look); Look->RegisterComponent(); Look->SetComponentTickEnabled(false);
    auto* Player=World->SpawnActor<APawn>();
    auto* PlayerBody=NewObject<UCapsuleComponent>(Player);
    PlayerBody->InitCapsuleSize(24,70); PlayerBody->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    PlayerBody->SetCollisionObjectType(ECC_Pawn); PlayerBody->SetCollisionResponseToAllChannels(ECR_Overlap);
    Player->SetRootComponent(PlayerBody); PlayerBody->RegisterComponent();
    auto* Controller=World->SpawnActor<APlayerController>(); Controller->Possess(Player);
    Player->SetActorLocation(FVector(250,0,0));
    const FVector OriginalPosition=Owner->GetActorLocation();
    const auto Check=[&]{ Look->TickComponent(.2f,LEVELTICK_All,nullptr); };

    Check();
    TestEqual(TEXT("玩家经过时短暂看向玩家"),Look->GetLookTarget(),static_cast<AActor*>(Player));
    TestFalse(TEXT("只经过不记为擦身"),Look->IsAfterBrush());
    const double FirstEnd=Look->LookState.EndTime;
    Check();
    TestEqual(TEXT("玩家一直在附近不会延长注视"),Look->LookState.EndTime,FirstEnd);
    Look->LookState.EndTime=Look->ServerTime()-1;
    Check();
    TestNull(TEXT("到时后恢复，不因玩家仍站着而重新看"),Look->GetLookTarget());
    Player->SetActorLocation(FVector(700,0,0)); Check();
    Player->SetActorLocation(FVector(250,0,0)); Check();
    TestNull(TEXT("同一玩家短时间折返仍受冷却限制"),Look->GetLookTarget());

    for (const float X:{200.f,150.f,100.f,55.f}) { Player->SetActorLocation(FVector(X,0,0)); Check(); }
    TestEqual(TEXT("轻擦立即看向接触玩家"),Look->GetLookTarget(),static_cast<AActor*>(Player));
    TestTrue(TEXT("轻擦采用更明显的注视"),Look->IsAfterBrush());
    const double BrushEnd=Look->LookState.EndTime;
    Check();
    TestEqual(TEXT("连续接触只触发一次"),Look->LookState.EndTime,BrushEnd);
    TestEqual(TEXT("注视不移动身体"),Owner->GetActorLocation(),OriginalPosition);
    TestFalse(TEXT("注视不打开物理模拟"),PlayerBody->IsSimulatingPhysics());
    Look->LookState.EndTime=Look->ServerTime()-1;
    Player->SetActorLocation(FVector(100,0,0)); Check();
    Player->SetActorLocation(FVector(55,0,0)); Check();
    TestNull(TEXT("分离后立刻再次轻擦不会重复反应"),Look->GetLookTarget());

    Look->PlayerVisits.Reset(); Look->ClearLook();
    Player->SetActorLocation(FVector(-250,0,0)); Check();
    TestNull(TEXT("玩家在背后时不强行拧头"),Look->GetLookTarget());
    Player->SetActorLocation(FVector(200,100,0)); Check();
    TestEqual(TEXT("同次经过从背后绕到侧前方时仍可看一眼"),Look->GetLookTarget(),static_cast<AActor*>(Player));

    Look->PlayerVisits.Reset(); Look->ClearLook();
    Player->SetActorLocation(FVector(100,0,0)); Check();
    Player->SetActorLocation(FVector(45,0,0)); Check();
    TestFalse(TEXT("快速进入接触范围不误报轻擦"),Look->IsAfterBrush());
    Look->PlayerVisits.Reset(); Look->ClearLook();
    Player->SetActorLocation(FVector(145,0,0)); Check();
    Owner->SetActorLocation(FVector(100,0,0)); Check();
    TestFalse(TEXT("玩家静止但NPC快速靠近也按相对速度排除轻擦"),Look->IsAfterBrush());
    Owner->SetActorLocation(OriginalPosition); Check();

    auto* Wall=World->SpawnActor<AActor>();
    auto* Box=NewObject<UBoxComponent>(Wall); Wall->SetRootComponent(Box);
    Box->SetBoxExtent(FVector(10,100,120)); Box->SetCollisionProfileName(TEXT("BlockAll")); Box->RegisterComponent();
    Wall->SetActorLocation(FVector(120,0,0)); Player->SetActorLocation(FVector(250,0,0));
    Look->PlayerVisits.Reset(); Look->ClearLook(); Check();
    TestNull(TEXT("墙挡住玩家时不隔墙看人"),Look->GetLookTarget());
    Wall->Destroy(); Check();
    TestEqual(TEXT("没有遮挡时可再次关注"),Look->GetLookTarget(),static_cast<AActor*>(Player));

    // 使用真实 NPC 的感知和表现组件，轻擦不能新增冲突事实或打断正在说的话
    const FVector NpcPosition(1000,0,0);
    auto* Npc=World->SpawnActorDeferred<ADeliveryNpcCharacter>(ADeliveryNpcCharacter::StaticClass(),
        FTransform(NpcPosition),nullptr,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    if (!TestNotNull(TEXT("创建轻擦检查用的 NPC"),Npc)) return false;
    Npc->AutoPossessAI=EAutoPossessAI::Disabled;
    Npc->FinishSpawning(FTransform(NpcPosition));
    Npc->SetActorTickEnabled(false);
    Npc->Look->SetComponentTickEnabled(false);
    auto* Face=LoadObject<USkeletalMesh>(nullptr,TEXT("/Game/NPC/Residents/Appearance/SK_NpcManFace"));
    if (!TestNotNull(TEXT("正式模型包含生气表情"),Face)) return false;
    Npc->GetMesh()->SetSkeletalMeshAsset(Face);
    Npc->Presentation->Expressions=NewObject<UDeliveryNpcExpressionSet>(Npc);
    Npc->Presentation->Expressions->Poses.FindOrAdd(TEXT("Angry")).MorphWeights.Add(TEXT("EyesAngry"),1.f);
    const auto BrushNpc=[&]
    {
        for (const float X:{200.f,150.f,100.f,55.f})
        {
            Player->SetActorLocation(NpcPosition+FVector(X,0,0));
            Npc->Look->TickComponent(.2f,LEVELTICK_All,nullptr);
        }
        Npc->Presentation->TickComponent(1.f,LEVELTICK_All,nullptr);
    };
    const FGuid Epoch=FGuid::NewGuid();
    FDeliveryNpcDecisionRequest BeforeBrush,AfterBrush;
    TestTrue(TEXT("读取轻擦前的模型上下文"),Npc->Agent->BuildDecisionRequest(Epoch,BeforeBrush));
    BrushNpc();
    TestTrue(TEXT("真实 NPC 轻擦后仍看向玩家"),Npc->Look->IsAfterBrush() && Npc->Look->GetLookTarget()==Player);
    TestTrue(TEXT("读取轻擦后的模型上下文"),Npc->Agent->BuildDecisionRequest(Epoch,AfterBrush));
    TestEqual(TEXT("轻擦不增加碰撞或攻击事实"),AfterBrush.Evidence.Num(),BeforeBrush.Evidence.Num());
    TestEqual(TEXT("轻擦不使当前回复的上下文过期"),AfterBrush.ContextRevision,BeforeBrush.ContextRevision);
    TestFalse(TEXT("轻擦不直接触发生气表情"),Npc->GetMesh()->GetMorphTarget(TEXT("EyesAngry"))>.01f);
    TestFalse(TEXT("轻擦不发起模型请求"),Npc->Agent->IsDecisionPending());

    TestTrue(TEXT("准备一段进行中的对白"),Npc->Presentation->PublishSpeech(TEXT("先让我把这句说完"),TEXT("Calm"),0));
    const FGuid TalkingTurn=Npc->Presentation->GetSpeech().TurnId;
    Npc->Look->PlayerVisits.Reset(); Npc->Look->ClearLook();
    BrushNpc();
    TestTrue(TEXT("说话时轻擦仍保留原句"),Npc->Presentation->GetSpeech().bActive &&
        Npc->Presentation->GetSpeech().TurnId==TalkingTurn);
    FDeliveryNpcDecisionRequest DuringSpeech;
    TestTrue(TEXT("读取交谈中的模型上下文"),Npc->Agent->BuildDecisionRequest(Epoch,DuringSpeech));
    TestEqual(TEXT("说话时轻擦也不新增冲突事实"),DuringSpeech.ContextRevision,BeforeBrush.ContextRevision);

    // 真正的命中继续记录，不能因减少擦身反应而失去攻击感知
    Npc->Combat->ReceiveFightHit(Player,FGuid::NewGuid());
    FDeliveryNpcDecisionRequest AfterHit;
    TestTrue(TEXT("读取真实命中后的模型上下文"),Npc->Agent->BuildDecisionRequest(Epoch,AfterHit));
    TestTrue(TEXT("真正打中仍保留受击事实"),AfterHit.Evidence.ContainsByPredicate(
        [](const FDeliveryNpcEvidence& Fact){ return Fact.Kind==TEXT("LightMeleeHit"); }));
    TestTrue(TEXT("真正打中仍播放受击动作"),Npc->Combat->IsReactingToHit());
    Npc->Destroy();
    Player->Destroy();
    TestNull(TEXT("目标销毁后查询立即失效"),Look->GetLookTarget());
    Owner->Destroy();
    TestTrue(TEXT("退出后清除玩家记录"),Look->PlayerVisits.IsEmpty());
    TestNull(TEXT("退出后不保留注视目标"),Look->LookState.Target.Get());
    return true;
}
#endif
