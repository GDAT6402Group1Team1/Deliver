#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "ActorFactories/ActorFactory.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/BehaviorTree/BTTask_DeliveryExecuteNpcAction.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "AIController.h"
#include "AI/Navigation/NavigationElement.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "Builders/CubeBuilder.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcStrikeNavigationTest,"Delivery.AI.StrikeNavigation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FDeliveryNpcStrikeNavigationTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
    const bool bOldEnabled=Settings->bEnabled; Settings->bEnabled=false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(true).CreateAISystem(true);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
        Settings->bEnabled=bOldEnabled;
    };
    World->InitializeActorsForPlay(FURL()); World->GetWorldSettings()->NotifyBeginPlay();
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
    if (!TestNotNull(TEXT("独立测试世界具有导航系统"),Nav)) return false;

    auto* Floor=World->SpawnActor<AActor>(); auto* Mesh=NewObject<UStaticMeshComponent>(Floor);
    Floor->SetRootComponent(Mesh);
    Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
    Mesh->SetCollisionProfileName(TEXT("BlockAll")); Mesh->SetMobility(EComponentMobility::Static);
    Mesh->SetWorldScale3D(FVector(16,16,.4)); Floor->SetActorLocation(FVector(200,0,-20)); Mesh->RegisterComponent();
    auto* Bounds=World->SpawnActor<ANavMeshBoundsVolume>(FVector(200,0,150),FRotator::ZeroRotator);
    auto* Builder=NewObject<UCubeBuilder>(); Builder->X=1400; Builder->Y=1400; Builder->Z=500;
    UActorFactory::CreateBrushForVolumeActor(Bounds,Builder);
    Nav->OnNavigationBoundsUpdated(Bounds);
    auto* NavData=Nav->GetDefaultNavDataInstance(FNavigationSystem::Create);
    if (!TestNotNull(TEXT("生成临时导航数据"),NavData)) return false;
    // 只修改本测试世界的新导航对象，不修改项目设置或导航类的默认对象
    auto* Generation=FindFProperty<FEnumProperty>(ANavigationData::StaticClass(),TEXT("RuntimeGeneration"));
    if (!TestNotNull(TEXT("临时导航可以设置为动态生成"),Generation)) return false;
    Generation->GetUnderlyingProperty()->SetIntPropertyValue(Generation->ContainerPtrToValuePtr<void>(NavData),
        static_cast<int64>(ERuntimeGenerationType::Dynamic));
    // CreateWorld 只创建导航对象，完成世界初始化后才会监听墙体等物件的新增与移除
    FNavigationSystem::AddNavigationSystemToWorld(*World,FNavigationSystemRunMode::GameMode);
    if (!TestTrue(TEXT("测试世界已完成导航初始化"),Nav->IsWorldInitDone())) return false;
    FNavigationSystem::UpdateComponentData(*Mesh);
    ++GFrameCounter; World->Tick(LEVELTICK_All,.02f);
    Nav->Build();
    FNavLocation Start,End;
    if (!TestTrue(TEXT("起点位于生成的导航网格"),Nav->ProjectPointToNavigation(FVector(0,0,0),Start,FVector(50,50,100))) ||
        !TestTrue(TEXT("终点位于生成的导航网格"),Nav->ProjectPointToNavigation(FVector(400,0,0),End,FVector(50,50,100)))) return false;

    FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* A=World->SpawnActor<ADeliveryNpcCharacter>(FVector(0,0,90),FRotator::ZeroRotator,Spawn);
    auto* B=World->SpawnActor<ADeliveryNpcCharacter>(FVector(400,0,90),FRotator::ZeroRotator,Spawn);
    A->SetActorTickEnabled(false); B->SetActorTickEnabled(false);
    const auto DescribeNavigation=[&](const TCHAR* Stage)
    {
        const FVector StartPosition=A->GetNavAgentLocation();
        const FVector EndPosition=B->GetNavAgentLocation();
        FNavLocation ProjectedStart,ProjectedEnd;
        const bool bStartOnNav=Nav->ProjectPointToNavigation(StartPosition,ProjectedStart,FVector(50,50,100));
        const bool bEndOnNav=Nav->ProjectPointToNavigation(EndPosition,ProjectedEnd,FVector(50,50,100));
        AddInfo(FString::Printf(TEXT("NavigationDiag %s actorA=%s feetA=%s modeA=%d actorB=%s feetB=%s modeB=%d projectA=%d:%s projectB=%d:%s capsuleNavA=%d capsuleNavB=%d floorRegistered=%d floorNav=%d"),
            Stage,*A->GetActorLocation().ToString(),*StartPosition.ToString(),int32(A->GetCharacterMovement()->MovementMode),
            *B->GetActorLocation().ToString(),*EndPosition.ToString(),int32(B->GetCharacterMovement()->MovementMode),
            bStartOnNav,*ProjectedStart.Location.ToString(),bEndOnNav,*ProjectedEnd.Location.ToString(),
            A->GetCapsuleComponent()->CanEverAffectNavigation(),B->GetCapsuleComponent()->CanEverAffectNavigation(),
            Mesh->IsRegistered(),Mesh->CanEverAffectNavigation()));
        auto* Controller=Cast<AAIController>(A->GetController());
        FAIMoveRequest Request(B);
        Request.SetAllowPartialPath(false);
        Request.SetRequireNavigableEndLocation(true);
        if (Controller) Request.SetNavigationFilter(Controller->GetDefaultNavigationFilterClass());
        FPathFindingQuery Query;
        const bool bQuery=Controller && Controller->BuildPathfindingQuery(Request,Query);
        if (bQuery)
        {
            const auto Result=Nav->FindPathSync(Query);
            const int32 Points=Result.Path.IsValid()?Result.Path->GetPathPoints().Num():0;
            AddInfo(FString::Printf(TEXT("NavigationDiag %s queryStart=%s queryEnd=%s result=%d validPath=%d partial=%d points=%d first=%s last=%s"),
                Stage,*Query.StartLocation.ToString(),*Query.EndLocation.ToString(),int32(Result.Result),
                Result.Path.IsValid() && Result.Path->IsValid(),Result.Path.IsValid() && Result.Path->IsPartial(),Points,
                Points?*Result.Path->GetPathPoints()[0].Location.ToString():TEXT("none"),
                Points?*Result.Path->GetPathPoints().Last().Location.ToString():TEXT("none")));
        }
        else AddInfo(FString::Printf(TEXT("NavigationDiag %s cannot build query"),Stage));
        FCollisionQueryParams Params(SCENE_QUERY_STAT(NpcNavigationTest),false,A);
        FHitResult Between,Ground;
        World->LineTraceSingleByChannel(Between,A->GetActorLocation(),B->GetActorLocation(),ECC_Visibility,Params);
        World->LineTraceSingleByChannel(Ground,A->GetActorLocation()+FVector(0,0,100),A->GetActorLocation()-FVector(0,0,1000),ECC_Visibility,Params);
        AddInfo(FString::Printf(TEXT("NavigationDiag %s betweenBlocking=%d betweenActor=%s betweenPoint=%s groundBlocking=%d groundActor=%s groundPoint=%s"),
            Stage,Between.bBlockingHit,*GetNameSafe(Between.GetActor()),*Between.ImpactPoint.ToString(),
            Ground.bBlockingHit,*GetNameSafe(Ground.GetActor()),*Ground.ImpactPoint.ToString()));
    };
    DescribeNavigation(TEXT("BeforeWall"));
    TestTrue(TEXT("完整导航允许走向远处目标"),A->CanExecute(EDeliveryNpcAction::MoveTo,B));
    TestTrue(TEXT("完整导航允许靠近后挥拳"),A->Combat->CanApproachStrike(B));

    // 用实际墙体把导航分成两块，不能把只走到墙边的路径当成能够到达
    auto* Barrier=World->SpawnActor<AActor>();
    auto* BarrierMesh=NewObject<UStaticMeshComponent>(Barrier);
    Barrier->SetRootComponent(BarrierMesh);
    BarrierMesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
    BarrierMesh->SetCollisionProfileName(TEXT("BlockAll"));
    BarrierMesh->SetMobility(EComponentMobility::Static);
    BarrierMesh->SetWorldScale3D(FVector(.6,16,6));
    Barrier->SetActorLocation(FVector(200,0,280));
    BarrierMesh->RegisterComponent();
    FNavigationSystem::UpdateComponentData(*BarrierMesh);
    ++GFrameCounter; World->Tick(LEVELTICK_All,.02f);
    Nav->Build();
    DescribeNavigation(TEXT("WithWall"));
    auto* PathController=Cast<AAIController>(A->GetController());
    FAIMoveRequest PartialRequest(B);
    PartialRequest.SetAllowPartialPath(true);
    FPathFindingQuery PartialQuery;
    if (!TestTrue(TEXT("墙两边仍有导航数据"),PathController && PathController->BuildPathfindingQuery(PartialRequest,PartialQuery))) return false;
    const auto PartialResult=Nav->FindPathSync(PartialQuery);
    TestTrue(TEXT("允许部分路径时只能走到墙前"),PartialResult.Path.IsValid() && PartialResult.Path->IsPartial());
    TestFalse(TEXT("完整路径检查拒绝墙前结束的路径"),A->CanReachTarget(B));
    TestFalse(TEXT("隔墙且无完整路径时不提供走过去"),A->CanExecute(EDeliveryNpcAction::MoveTo,B));
    TestFalse(TEXT("隔墙时不提供追近挥拳"),A->Combat->CanApproachStrike(B));
    // 检查普通组件退出流程确实移除了墙体导航，再验证拆墙后能重新靠近目标
    const FNavigationElementHandle WallNavigation(BarrierMesh);
    TestNotNull(TEXT("拆墙前导航仍保存实际墙体"),Nav->GetDataForElement(WallNavigation));
    BarrierMesh->SetCanEverAffectNavigation(false);
    BarrierMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    BarrierMesh->UnregisterComponent();
    Barrier->Destroy();
    Nav->FlushPendingOperations();
    TestNull(TEXT("拆墙后导航已移除该墙的几何"),Nav->GetDataForElement(WallNavigation));
    ++GFrameCounter; World->Tick(LEVELTICK_All,.02f);
    Nav->Build();
    DescribeNavigation(TEXT("AfterWallRemoval"));
    TestTrue(TEXT("拆墙并更新导航后重新允许靠近"),A->CanReachTarget(B));
    A->ObserveFact(TEXT("SeenNpc"),B,TEXT("对方站在前面"),EDeliveryNpcEvidenceSource::Seen);
    Settings->bEnabled=true; const bool bQueued=A->Agent->RequestDecision(); Settings->bEnabled=false;
    if (!TestTrue(TEXT("请求这次行动"),bQueued)) return false;
    const auto Request=A->Agent->ActiveRequest;
    World->GetSubsystem<UDeliveryAiGatewaySubsystem>()->Cancel(A->Agent);
    const auto* Candidate=Request.Candidates.FindByPredicate([](const auto& Item){ return Item.Action==EDeliveryNpcAction::StrikeOnce; });
    if (!TestNotNull(TEXT("400厘米处提供靠近后挥拳"),Candidate)) return false;
    FDeliveryNpcDecisionProposal Proposal;
    Proposal.WorldEpoch=Request.WorldEpoch; Proposal.NpcId=Request.NpcId; Proposal.RequestId=Request.RequestId;
    Proposal.ContextRevision=Request.ContextRevision; Proposal.PlanRevision=Request.PlanRevision;
    Proposal.Action=Candidate->Action; Proposal.TargetId=Candidate->TargetId; Proposal.EvidenceIds=Candidate->EvidenceIds; Proposal.Duration=5;
    A->Agent->ReceiveDecision(Proposal.RequestId,&Proposal,NAME_None);
    const FGuid ActionId=A->Agent->GetPublicState().ActionId;

    // 运行正式任务节点，导航和 CharacterMovement 按正常 Tick 推进，不手动移动角色
    auto* Tree=NewObject<UBehaviorTree>(); auto* Root=NewObject<UBTComposite_Sequence>(Tree);
    Tree->RootNode=Root;
    FBTCompositeChild Child; Child.ChildTask=NewObject<UBTTask_DeliveryExecuteNpcAction>(Root); Root->Children.Add(Child);
    auto* AI=Cast<AAIController>(A->GetController());
    if (!TestTrue(TEXT("启动包含正式攻击节点的行为树"),AI && AI->RunBehaviorTree(Tree))) return false;
    const FVector Original=A->GetActorLocation();
    bool bStartedOutsideRange=false;
    double StrikeTime=-1;
    for (int32 Step=0;Step<275;++Step)
    {
        ++GFrameCounter; World->Tick(LEVELTICK_All,.02f);
        const double Started=A->Combat->GetStrikeStartTime(ActionId);
        if (Started>=0 && StrikeTime<0)
        {
            StrikeTime=Started;
            bStartedOutsideRange=FVector::Dist(A->GetActorLocation(),B->GetActorLocation())>A->Combat->StrikeRange+.1f;
        }
        if (DeliveryNpc::IsTerminal(A->Agent->GetPublicState().Status)) break;
    }
    if (AI->BrainComponent) AI->BrainComponent->StopLogic(TEXT("NavigationTestFinished"));
    TestTrue(TEXT("NPC实际沿导航移动了至少200厘米"),FVector::Dist2D(Original,A->GetActorLocation())>200);
    TestTrue(TEXT("接近后才开始出拳"),StrikeTime>=0);
    TestFalse(TEXT("不会在160厘米以外启动挥拳"),bStartedOutsideRange);
    TestTrue(TEXT("对方确实被击倒"),B->IsKnockedDown());
    TestEqual(TEXT("正式行为树报告攻击成功"),A->Agent->GetPublicState().Status,EDeliveryNpcActionStatus::Succeeded);
    TestEqual(TEXT("攻击结束恢复普通行走速度"),A->GetCharacterMovement()->MaxWalkSpeed,A->WalkSpeed);

    const auto StartMove=[&](EDeliveryNpcAction Action)
    {
        if (AI->BrainComponent) AI->BrainComponent->StopLogic(TEXT("NextMovementTest"));
        A->Agent->CancelPlan(TEXT("NextMovementTest"));
        A->ObserveFact(TEXT("SeenNpc"),B,TEXT("对方还在附近"),EDeliveryNpcEvidenceSource::Seen);
        Settings->bEnabled=true; const bool bMoveQueued=A->Agent->RequestDecision(); Settings->bEnabled=false;
        if (!bMoveQueued) return false;
        const auto MoveRequest=A->Agent->ActiveRequest;
        World->GetSubsystem<UDeliveryAiGatewaySubsystem>()->Cancel(A->Agent);
        const auto* MoveCandidate=MoveRequest.Candidates.FindByPredicate([&](const auto& Item){ return Item.Action==Action; });
        if (!MoveCandidate) return false;
        FDeliveryNpcDecisionProposal Move;
        Move.WorldEpoch=MoveRequest.WorldEpoch; Move.NpcId=MoveRequest.NpcId; Move.RequestId=MoveRequest.RequestId;
        Move.ContextRevision=MoveRequest.ContextRevision; Move.PlanRevision=MoveRequest.PlanRevision;
        Move.Action=Action; Move.TargetId=MoveCandidate->TargetId; Move.EvidenceIds=MoveCandidate->EvidenceIds; Move.Duration=5;
        A->Agent->ReceiveDecision(Move.RequestId,&Move,NAME_None);
        return A->Agent->GetPublicState().Status==EDeliveryNpcActionStatus::Accepted && AI->RunBehaviorTree(Tree);
    };
    const auto Tick=[&](float Seconds)
    {
        for (int32 Index=0;Index<FMath::CeilToInt(Seconds/.02f);++Index)
        { ++GFrameCounter; World->Tick(LEVELTICK_All,.02f); }
    };
    Tick(.1f);
    const FVector PaceOrigin=A->GetActorLocation();
    if (!TestTrue(TEXT("模型可选择附近踱步"),StartMove(EDeliveryNpcAction::Pace))) return false;
    float Furthest=0;
    for (int32 Step=0;Step<275;++Step)
    {
        Tick(.02f);
        Furthest=FMath::Max(Furthest,static_cast<float>(FVector::Dist2D(PaceOrigin,A->GetActorLocation())));
        if (DeliveryNpc::IsTerminal(A->Agent->GetPublicState().Status)) break;
    }
    TestTrue(TEXT("踱步确实离开起点而非原地成功"),Furthest>60);
    TestTrue(TEXT("踱步始终在附近"),Furthest<140);
    TestEqual(TEXT("往返踱步完成"),A->Agent->GetPublicState().Status,EDeliveryNpcActionStatus::Succeeded);
    TestTrue(TEXT("踱步结束回到本次起点"),FVector::Dist2D(PaceOrigin,A->GetActorLocation())<=12);
    TestEqual(TEXT("踱步结束恢复之前的速度"),A->GetCharacterMovement()->MaxWalkSpeed,A->WalkSpeed);

    const FVector StepOrigin=A->GetActorLocation();
    const FVector Toward=(B->GetActorLocation()-StepOrigin).GetSafeNormal2D();
    if (!TestTrue(TEXT("模型可选择向旁边让路"),StartMove(EDeliveryNpcAction::MoveAside))) return false;
    for (int32 Step=0;Step<275;++Step)
    {
        Tick(.02f);
        if (DeliveryNpc::IsTerminal(A->Agent->GetPublicState().Status)) break;
    }
    const FVector SideMove=A->GetActorLocation()-StepOrigin;
    TestEqual(TEXT("让路动作完成"),A->Agent->GetPublicState().Status,EDeliveryNpcActionStatus::Succeeded);
    TestTrue(TEXT("让路产生明确的侧向位移"),SideMove.Size2D()>140);
    TestTrue(TEXT("让路没有走向对方"),FMath::Abs(FVector::DotProduct(SideMove,Toward))<30);

    if (!TestTrue(TEXT("准备被玩家打断的踱步"),StartMove(EDeliveryNpcAction::Pace))) return false;
    Tick(.3f);
    TestEqual(TEXT("踱步期间采用慢速"),A->GetCharacterMovement()->MaxWalkSpeed,A->PaceSpeed);
    A->Agent->CancelPlan(TEXT("PlayerInterrupted"));
    TestEqual(TEXT("打断立即恢复之前速度"),A->GetCharacterMovement()->MaxWalkSpeed,A->WalkSpeed);
    const FVector CancelledAt=A->GetActorLocation(); Tick(.3f);
    TestTrue(TEXT("打断后停止沿旧路径行走"),FVector::Dist2D(CancelledAt,A->GetActorLocation())<5);
    if (AI->BrainComponent) AI->BrainComponent->StopLogic(TEXT("MovementTestFinished"));

    // 用实际碰撞体阻挡两边，完整导航网格也不能绕过落点碰撞检查
    const FVector ToTarget=(B->GetActorLocation()-A->GetActorLocation()).GetSafeNormal2D();
    const FVector Side=FVector::CrossProduct(FVector::UpVector,ToTarget).GetSafeNormal();
    TArray<AActor*> Walls;
    for (const float Sign:{-1.f,1.f})
    {
        auto* Wall=World->SpawnActor<AActor>(); auto* Box=NewObject<UBoxComponent>(Wall); Wall->SetRootComponent(Box);
        Box->SetBoxExtent(FVector(55,55,120)); Box->SetCollisionProfileName(TEXT("BlockAll")); Box->SetCanEverAffectNavigation(false);
        Wall->SetActorLocation(A->GetActorLocation()+Side*Sign*A->SideStepDistance); Box->RegisterComponent(); Walls.Add(Wall);
    }
    FVector BlockedDestination;
    TestFalse(TEXT("两侧落点被阻挡时拒绝让路"),A->FindSideStepDestination(B,BlockedDestination));
    for (auto* Wall:Walls) Wall->Destroy();
    if (!TestTrue(TEXT("准备受击中断的踱步"),StartMove(EDeliveryNpcAction::Pace))) return false;
    Tick(.2f); A->ReceiveImpactFrom(FVector(100,0,0),A->GetActorLocation(),B); Tick(.1f);
    TestTrue(TEXT("受击后NPC倒地"),A->IsKnockedDown());
    TestEqual(TEXT("迟到的动作结束也不会让倒地NPC恢复走路速度"),A->GetCharacterMovement()->MaxWalkSpeed,0.f);
    return true;
}
#endif
