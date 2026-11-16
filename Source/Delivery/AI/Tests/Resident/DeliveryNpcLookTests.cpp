#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Presentation/Face/DeliveryNpcLookComponent.h"
#include "AI/Presentation/Face/DeliveryNpcExpressionSet.h"
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
#include "GameFramework/CharacterMovementComponent.h"
#include "UObject/UnrealType.h"

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

    Look->PlayerVisits.Reset(); Look->ClearLook(); Look->NextAttentionAt=0;
    Player->SetActorLocation(FVector(-250,0,0)); Check();
    TestNull(TEXT("玩家在背后时不强行拧头"),Look->GetLookTarget());
    Player->SetActorLocation(FVector(200,100,0)); Check();
    TestEqual(TEXT("同次经过从背后绕到侧前方时仍可看一眼"),Look->GetLookTarget(),static_cast<AActor*>(Player));

    Look->PlayerVisits.Reset(); Look->ClearLook(); Look->NextAttentionAt=0;
    Player->SetActorLocation(FVector(100,0,0)); Check();
    Player->SetActorLocation(FVector(45,0,0)); Check();
    TestFalse(TEXT("快速进入接触范围不误报轻擦"),Look->IsAfterBrush());
    Look->PlayerVisits.Reset(); Look->ClearLook(); Look->NextAttentionAt=0;
    Player->SetActorLocation(FVector(145,0,0)); Check();
    Owner->SetActorLocation(FVector(100,0,0)); Check();
    TestFalse(TEXT("玩家静止但NPC快速靠近也按相对速度排除轻擦"),Look->IsAfterBrush());
    Owner->SetActorLocation(OriginalPosition); Check();

    auto* Wall=World->SpawnActor<AActor>();
    auto* Box=NewObject<UBoxComponent>(Wall); Wall->SetRootComponent(Box);
    Box->SetBoxExtent(FVector(10,100,120)); Box->SetCollisionProfileName(TEXT("BlockAll")); Box->RegisterComponent();
    Wall->SetActorLocation(FVector(120,0,0)); Player->SetActorLocation(FVector(250,0,0));
    Look->PlayerVisits.Reset(); Look->ClearLook(); Look->NextAttentionAt=0; Check();
    TestNull(TEXT("墙挡住玩家时不隔墙看人"),Look->GetLookTarget());
    Wall->Destroy(); Check();
    TestEqual(TEXT("没有遮挡时可再次关注"),Look->GetLookTarget(),static_cast<AActor*>(Player));

    // 多个玩家轮流经过也不能让 NPC 不停换目标；这次没看的人不会排队补看
    auto* OtherPlayer=World->SpawnActor<APawn>();
    auto* OtherBody=NewObject<UCapsuleComponent>(OtherPlayer);
    OtherPlayer->SetRootComponent(OtherBody); OtherBody->RegisterComponent();
    OtherPlayer->SetActorLocation(FVector(200,100,0));
    Look->StartLook(OtherPlayer,false,Look->ServerTime());
    TestEqual(TEXT("第二个过路人不立即抢走注视"),Look->GetLookTarget(),static_cast<AActor*>(Player));
    Look->LookState.EndTime=Look->ServerTime()-1;
    Look->ClearLook();
    Look->StartLook(OtherPlayer,false,Look->ServerTime());
    TestNull(TEXT("上一眼结束后仍保留整体间隔"),Look->GetLookTarget());
    Look->NextAttentionAt=0;
    Look->bNoticePassersby=false;
    Look->PlayerVisits.Reset(); Check();
    TestNull(TEXT("人物可以不关注普通过路人"),Look->GetLookTarget());
    Look->bNoticePassersby=true;
    OtherPlayer->Destroy();

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

    TestFalse(TEXT("对白期间鸣笛不抢表情"),Npc->Look->TryStartle(Player));
    TestTrue(TEXT("跳过受惊后原句继续播放"),Npc->Presentation->GetSpeech().bActive &&
        Npc->Presentation->GetSpeech().TurnId==TalkingTurn);
    Npc->Presentation->CancelSpeech();
    Npc->Look->ClearLook();
    Npc->Look->NextStartleAt=0;
    Npc->Presentation->Expressions->Poses.FindOrAdd(TEXT("Surprised")).MorphWeights.Add(TEXT("FaceSurprised"),1.f);
    Player->SetActorLocation(NpcPosition+FVector(200,0,0));
    FDeliveryNpcDecisionRequest BeforeStartle,AfterStartle;
    TestTrue(TEXT("读取受惊前的上下文"),Npc->Agent->BuildDecisionRequest(Epoch,BeforeStartle));
    TestTrue(TEXT("空闲时近处声源可以引起受惊"),Npc->Look->TryStartle(Player));
    TestTrue(TEXT("受惊标记供各客户端查询"),Npc->Look->IsStartled());
    Npc->Presentation->TickExpression(1.f);
    TestTrue(TEXT("复用正式模型的受惊表情"),Npc->GetMesh()->GetMorphTarget(TEXT("FaceSurprised"))>.5f);
    TestTrue(TEXT("读取受惊后的上下文"),Npc->Agent->BuildDecisionRequest(Epoch,AfterStartle));
    TestEqual(TEXT("小反应不新增事实或作废模型请求"),AfterStartle.ContextRevision,BeforeStartle.ContextRevision);
    TestFalse(TEXT("小反应本身不安排模型请求"),Npc->Agent->IsDecisionPending());
    TestFalse(TEXT("连续鸣笛不重复延长反应"),Npc->Look->TryStartle(Player));
    Npc->Look->LookState.EndTime=Npc->Look->ServerTime()-1;
    TestFalse(TEXT("迟到客户端不会播放已经结束的受惊"),Npc->Look->IsStartled());
    Npc->Look->ClearLook();
    TestFalse(TEXT("表情结束后仍有鸣笛冷却"),Npc->Look->TryStartle(Player));
    Npc->Look->NextStartleAt=0;
    Player->SetActorLocation(NpcPosition+FVector(300,0,0));
    TestFalse(TEXT("较远的鸣笛不引起小反应"),Npc->Look->TryStartle(Player));
    Player->SetActorLocation(NpcPosition+FVector(200,0,0));
    Npc->Look->bStartleAtHorn=false;
    TestFalse(TEXT("人物配置可以关闭鸣笛反应"),Npc->Look->TryStartle(Player));
    Npc->Look->bStartleAtHorn=true;
    auto* StartleWall=World->SpawnActor<AActor>();
    auto* StartleBox=NewObject<UBoxComponent>(StartleWall); StartleWall->SetRootComponent(StartleBox);
    StartleBox->SetBoxExtent(FVector(10,100,120)); StartleBox->SetCollisionProfileName(TEXT("BlockAll")); StartleBox->RegisterComponent();
    StartleWall->SetActorLocation(NpcPosition+FVector(100,0,0));
    TestFalse(TEXT("不会隔墙盯住声源"),Npc->Look->TryStartle(Player));
    StartleWall->Destroy();

    // 这里只提供同步状态来检查表现优先级，不用它代替游戏中的动作验收
    auto* StateProperty=FindFProperty<FStructProperty>(UDeliveryNpcAgentComponent::StaticClass(),TEXT("PublicState"));
    if (!TestNotNull(TEXT("动作复制字段存在"),StateProperty)) return false;
    auto* Action=StateProperty->ContainerPtrToValuePtr<FDeliveryNpcPublicState>(Npc->Agent);
    const FDeliveryNpcPublicState SavedAction=*Action;
    const FGuid BusyId=FGuid::NewGuid();
    const FVector MovingVelocity(100,0,0);
    Npc->GetCharacterMovement()->Velocity=MovingVelocity;
    for (const auto Status:{EDeliveryNpcActionStatus::Accepted,EDeliveryNpcActionStatus::Running})
        for (const auto Kind:{EDeliveryNpcAction::LookAt,EDeliveryNpcAction::MoveTo,EDeliveryNpcAction::Fight,
            EDeliveryNpcAction::Flee,EDeliveryNpcAction::Follow,EDeliveryNpcAction::TakePackage,EDeliveryNpcAction::DeliverPackage})
        {
            Action->ActionId=BusyId; Action->Status=Status; Action->Action=Kind;
            TestFalse(TEXT("已接受或正在执行的动作优先于受惊"),Npc->Look->TryStartle(Player));
            Npc->Look->StartLook(Player,true,Npc->Look->ServerTime());
            TestNull(TEXT("忙碌时轻擦也不抢走注视"),Npc->Look->GetLookTarget());
            TestEqual(TEXT("原动作编号保持不变"),Action->ActionId,BusyId);
            TestEqual(TEXT("没有暂停或改写移动速度"),Npc->GetCharacterMovement()->Velocity,MovingVelocity);
        }
    *Action=SavedAction;
    Npc->GetCharacterMovement()->Velocity=FVector::ZeroVector;
    TestTrue(TEXT("重新空闲后新事件可引起受惊"),Npc->Look->TryStartle(Player));
    Action->ActionId=BusyId; Action->Status=EDeliveryNpcActionStatus::Running; Action->Action=EDeliveryNpcAction::Fight;
    Npc->Look->TickComponent(.2f,LEVELTICK_All,nullptr);
    *Action=SavedAction;
    TestFalse(TEXT("动作接管后不会补播旧受惊"),Npc->Look->IsStartled());
    Npc->Look->PlayerVisits.Reset();
    Npc->Look->NextAttentionAt=0;
    Npc->Look->TickComponent(.2f,LEVELTICK_All,nullptr);
    TestNull(TEXT("忙碌期间经过的人不会在结束后补算新事件"),Npc->Look->GetLookTarget());
    Npc->Look->TickComponent(.2f,LEVELTICK_All,nullptr);
    TestNull(TEXT("重建采样后玩家站在原地仍不重复关注"),Npc->Look->GetLookTarget());
    Npc->Look->NextStartleAt=0;
    TestTrue(TEXT("开始检查目标失效"),Npc->Look->TryStartle(Player));
    Player->SetActorHiddenInGame(true);
    TestFalse(TEXT("目标隐藏后不继续注视"),Npc->Look->IsStartled());
    Player->SetActorHiddenInGame(false);
    Npc->Look->ClearLook();

    // 真正的命中继续记录，不能因减少擦身反应而失去攻击感知
    Npc->Combat->ReceiveFightHit(Player,FGuid::NewGuid());
    FDeliveryNpcDecisionRequest AfterHit;
    TestTrue(TEXT("读取真实命中后的模型上下文"),Npc->Agent->BuildDecisionRequest(Epoch,AfterHit));
    TestTrue(TEXT("真正打中仍保留受击事实"),AfterHit.Evidence.ContainsByPredicate(
        [](const FDeliveryNpcEvidence& Fact){ return Fact.Kind==TEXT("LightMeleeHit"); }));
    TestTrue(TEXT("真正打中仍播放受击动作"),Npc->Combat->IsReactingToHit());
    Npc->Look->HitExpression=TEXT("Angry");
    Npc->Presentation->TickExpression(1.f);
    TestTrue(TEXT("受击后按人物配置显示表情"),Npc->GetMesh()->GetMorphTarget(TEXT("EyesAngry"))>.5f);
    Npc->Look->HitExpression=NAME_None;
    Npc->Presentation->TickExpression(1.f);
    TestFalse(TEXT("关闭额外受击表情后不残留愤怒"),Npc->GetMesh()->GetMorphTarget(TEXT("EyesAngry"))>.01f);
    TestTrue(TEXT("关闭表情不取消真实受击动作"),Npc->Combat->IsReactingToHit());
    Npc->Destroy();
    Player->Destroy();
    TestNull(TEXT("目标销毁后查询立即失效"),Look->GetLookTarget());
    Owner->Destroy();
    TestTrue(TEXT("退出后清除玩家记录"),Look->PlayerVisits.IsEmpty());
    TestNull(TEXT("退出后不保留注视目标"),Look->LookState.Target.Get());
    return true;
}
#endif
