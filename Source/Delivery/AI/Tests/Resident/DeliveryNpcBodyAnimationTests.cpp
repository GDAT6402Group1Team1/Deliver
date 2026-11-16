#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "AI/Presentation/Animation/DeliveryNpcBodyAnimInstance.h"
#include "AI/Presentation/Animation/DeliveryNpcConversationAnimInstance.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcBodyAnimationTest,"Delivery.AI.BodyAnimation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryNpcBodyAnimationTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
    const bool bWasEnabled=Settings->bEnabled; Settings->bEnabled=false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL()); World->GetWorldSettings()->NotifyBeginPlay();
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
        Settings->bEnabled=bWasEnabled;
    };
    auto* Floor=World->SpawnActor<AActor>(); auto* Box=NewObject<UBoxComponent>(Floor);
    Floor->SetRootComponent(Box); Box->SetBoxExtent(FVector(2000,2000,20));
    Box->SetCollisionProfileName(TEXT("BlockAll")); Box->RegisterComponent(); Floor->SetActorLocation(FVector(0,0,-20));
    const auto Tick=[&](float Seconds)
    {
        for (int32 Index=0;Index<FMath::CeilToInt(Seconds/.02f);++Index)
        { ++GFrameCounter; World->Tick(LEVELTICK_All,.02f); }
    };
    using EPose=UDeliveryNpcBodyAnimInstance::EPose;
    int32 CharacterIndex=0;
    for (const TCHAR* Path:{TEXT("/Game/NPC/Residents/BP_ResidentLoud.BP_ResidentLoud_C"),
        TEXT("/Game/NPC/Residents/BP_ResidentShrewd.BP_ResidentShrewd_C")})
    {
        auto* Class=LoadClass<ADeliveryNpcCharacter>(nullptr,Path);
        if (!TestNotNull(TEXT("正式居民蓝图存在"),Class)) return false;
        FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto* Npc=World->SpawnActor<ADeliveryNpcCharacter>(Class,FVector(0,CharacterIndex++*500,90),FRotator::ZeroRotator,Spawn);
        Npc->SetActorTickEnabled(false);
        // 本检查直接提供同步后的动作状态，不启动服务器决策和动作超时检查
        Npc->Agent->SetComponentTickEnabled(false);
        auto* Mesh=Npc->GetMesh();
        Mesh->VisibilityBasedAnimTickOption=EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
        Mesh->bEnableUpdateRateOptimizations=false;
        // 固定胶囊，只向动画提供实际速度字段，避免本检查混入寻路和角色移动手感
        Npc->GetCharacterMovement()->SetComponentTickEnabled(false);
        Npc->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
        Tick(.3f);
        auto* Body=Cast<UDeliveryNpcBodyAnimInstance>(Mesh->GetAnimInstance());
        if (!TestNotNull(TEXT("正式居民使用主动画蓝图"),Body)) return false;
        TestNotNull(TEXT("注视后处理仍在"),Cast<UDeliveryNpcConversationAnimInstance>(Mesh->GetPostProcessInstance()));
        TestEqual(TEXT("站立时使用待机"),Body->PoseIndex,static_cast<int32>(EPose::Idle));
        const FVector IdleFoot=Mesh->GetSocketTransform(TEXT("LeftFoot"),RTS_Component).GetLocation();
        Npc->GetCharacterMovement()->Velocity=FVector(90,0,0); Tick(.3f);
        TestEqual(TEXT("移动进入行走"),Body->PoseIndex,static_cast<int32>(EPose::Walk));
        TestTrue(TEXT("实际动画图改变了脚部姿势"),FVector::Distance(IdleFoot,
            Mesh->GetSocketTransform(TEXT("LeftFoot"),RTS_Component).GetLocation())>1.f);
        Npc->GetCharacterMovement()->Velocity=FVector(18,0,0); Tick(.04f);
        TestEqual(TEXT("低速减速时保持步态"),Body->PoseIndex,static_cast<int32>(EPose::Walk));
        Npc->GetCharacterMovement()->Velocity=FVector::ZeroVector; Tick(.3f);
        TestEqual(TEXT("停下后恢复站立"),Body->PoseIndex,static_cast<int32>(EPose::Idle));
        Npc->GetCharacterMovement()->Velocity=FVector(18,0,0); Tick(.04f);
        TestEqual(TEXT("静止后的轻微速度不反复启动步态"),Body->PoseIndex,static_cast<int32>(EPose::Idle));

        const auto LimbLength=[&](FName Parent,FName Child)
        {
            return FVector::Distance(Mesh->GetSocketTransform(Parent,RTS_Component).GetLocation(),
                Mesh->GetSocketTransform(Child,RTS_Component).GetLocation());
        };
        const double ArmLength=LimbLength(TEXT("LeftArm"),TEXT("LeftForeArm"));
        const double LegLength=LimbLength(TEXT("RightUpLeg"),TEXT("RightLeg"));
        const FVector IdleHips=Mesh->GetSocketTransform(TEXT("Hips"),RTS_Component).GetLocation();
        const auto CheckJumpShape=[&]()
        {
            TestTrue(TEXT("正式跳跃混合后手臂和腿段保持原尺寸"),ArmLength>5 && LegLength>5 &&
                FMath::IsNearlyEqual(LimbLength(TEXT("LeftArm"),TEXT("LeftForeArm")),ArmLength,1.) &&
                FMath::IsNearlyEqual(LimbLength(TEXT("RightUpLeg"),TEXT("RightLeg")),LegLength,1.));
            const FVector JumpHips=Mesh->GetSocketTransform(TEXT("Hips"),RTS_Component).GetLocation();
            TestTrue(FString::Printf(TEXT("修正骨架尺寸时不放大骨盆位移 pose=%d idle=%s jump=%s"),
                Body->PoseIndex,*IdleHips.ToCompactString(),*JumpHips.ToCompactString()),FVector::Distance(IdleHips,JumpHips)<150.);
        };
        Npc->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
        Npc->GetCharacterMovement()->Velocity=FVector(100,0,300); Tick(.25f);
        TestEqual(TEXT("真实腾空状态选择起跳片段"),Body->PoseIndex,static_cast<int32>(EPose::JumpStart));
        CheckJumpShape();
        Npc->GetCharacterMovement()->Velocity=FVector(100,0,-100); Tick(.2f);
        TestEqual(TEXT("下落切换空中循环"),Body->PoseIndex,static_cast<int32>(EPose::JumpLoop));
        CheckJumpShape();
        Npc->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
        Npc->GetCharacterMovement()->Velocity=FVector::ZeroVector; Tick(.2f);
        TestEqual(TEXT("接地切换落地片段"),Body->PoseIndex,static_cast<int32>(EPose::JumpLand));
        CheckJumpShape();
        Tick(1.4f);
        TestEqual(TEXT("落地片段完成后回原待机"),Body->PoseIndex,static_cast<int32>(EPose::Idle));

        // 测试提供服务器同步的动作状态，真正的片段选择与骨骼求值使用正式动画蓝图
        auto& State=Npc->Agent->PublicState; State.Status=EDeliveryNpcActionStatus::Running;
        State.ActionId=FGuid::NewGuid();
        State.Action=EDeliveryNpcAction::Pace; Npc->GetCharacterMovement()->Velocity=FVector(80,0,0); Tick(.2f);
        TestEqual(TEXT("踱步使用独立片段"),Body->PoseIndex,static_cast<int32>(EPose::Pace));
        auto* Table=NewObject<UDataTable>();
        Table->RowStruct=FDeliveryNpcProfileRow::StaticStruct();
        FDeliveryNpcProfileRow Row;
        Row.DisplayName=FText::FromString(TEXT("让路表现测试"));
        Row.Persona=TEXT("热心的街坊");
        const auto MoveAsideTag=FGameplayTag::RequestGameplayTag(TEXT("Npc.Behavior.Movement.MoveAside"),false);
        auto& MoveAside=Row.BehaviorPresentation.Add(MoveAsideTag);
        MoveAside.Animation=Npc->Presentation->WalkAnimation;
        MoveAside.MovementReferenceSpeed=160.f;
        Table->AddRow(TEXT("MoveAside"),Row);
        Npc->Profile.DataTable=Table; Npc->Profile.RowName=TEXT("MoveAside");
        if (!TestTrue(TEXT("应用可编辑的让路表现"),Npc->ApplyProfile())) return false;
        State.Action=EDeliveryNpcAction::MoveAside; State.ActionId=FGuid::NewGuid(); Tick(.2f);
        TestEqual(TEXT("让路选用短步混合槽"),Body->PoseIndex,static_cast<int32>(EPose::Pace));
        TestTrue(TEXT("使用让路的动画片段与参考速度"),Body->PaceAnimation==MoveAside.Animation &&
            FMath::IsNearlyEqual(Body->MovePlayRate,.5f));
        State.Status=EDeliveryNpcActionStatus::Cancelled; Tick(.2f);
        TestEqual(TEXT("取消时实际速度仍非零也回到普通走路"),Body->PoseIndex,static_cast<int32>(EPose::Walk));
        State.Status=EDeliveryNpcActionStatus::Running;
        State.Action=EDeliveryNpcAction::StrikeOnce; State.ActionId=FGuid::NewGuid(); Tick(.2f);
        TestEqual(TEXT("还没挥拳时显示快步靠近"),Body->PoseIndex,static_cast<int32>(EPose::Hurry));
        Npc->GetCharacterMovement()->Velocity=FVector::ZeroVector;
        auto* Target=World->SpawnActor<ADeliveryNpcCharacter>(Npc->GetActorLocation()+FVector(100,0,0),FRotator::ZeroRotator,Spawn);
        Target->SetActorTickEnabled(false);
        // 身体近战距离按双方胶囊计算，动画夹具把目标放在实际可挥拳的范围内
        Target->SetActorLocation(Npc->GetActorLocation()+FVector(Npc->Combat->GetStrikeApproachDistance(Target)-1.f,0,0));
        if (!TestTrue(TEXT("开始真实挥拳计时"),Npc->Combat->StartStrike(Target,State.ActionId))) return false;
        Tick(.2f);
        TestEqual(TEXT("挥拳窗口使用明确时间片段"),Body->PoseIndex,static_cast<int32>(EPose::Strike));
        TestTrue(TEXT("动画沿服务器挥拳时间推进"),Body->StrikeTime>.15f && Body->StrikeTime<.3f);
        const float BeforeReinit=Body->StrikeTime;
        Mesh->InitAnim(true); Tick(.02f); Body=Cast<UDeliveryNpcBodyAnimInstance>(Mesh->GetAnimInstance());
        if (!TestNotNull(TEXT("重建动画后仍使用主蓝图"),Body)) return false;
        TestTrue(TEXT("迟到或重建动画时接上当前挥拳进度"),Body->StrikeTime>=BeforeReinit);
        Npc->Combat->CancelStrike(); State.Status=EDeliveryNpcActionStatus::Cancelled; Tick(.2f);
        TestEqual(TEXT("取消后退出挥拳"),Body->PoseIndex,static_cast<int32>(EPose::Idle));

        using Action=EDeliveryNpcAction;
        const auto HasGetUp=[](const FDeliveryNpcDecisionRequest& Request)
        {
            return Request.Candidates.ContainsByPredicate([](const auto& Candidate)
                { return Candidate.Action==Action::GetUp; });
        };
        auto* Agent=Npc->Agent.Get();
        auto* Player=World->SpawnActor<APlayerController>();
        auto* PlayerState=World->SpawnActor<APlayerState>();
        if (!TestTrue(TEXT("创建真实玩家控制器与玩家状态"),IsValid(Player) && IsValid(PlayerState))) return false;
        PlayerState->SetOwner(Player);
        Player->PlayerState=PlayerState;
        Player->Possess(Target);
        const auto RequestGetUp=[&](FDeliveryNpcDecisionRequest& Request)
        {
            FDeliveryNpcObservation Speech;
            Speech.EventId=FGuid::NewGuid(); Speech.Kind=TEXT("PlayerSpeech"); Speech.Source=EDeliveryNpcEvidenceSource::Heard;
            Speech.Subject=Target; Speech.Addressee=Npc; Speech.Description=TEXT("起来吧。"); Speech.bNeedsReply=true;
            return Target->GetController()==Player && Agent->ObserveEvent(Speech) && Agent->BeginListening(Target,Speech.EventId) &&
                Agent->BuildDecisionRequest(FGuid::NewGuid(),Request) && Request.bPlayerRequest && Request.bPendingPlayerMessage &&
                Request.RequesterPC.Get()==Player && Request.RequesterPlayerState.Get()==PlayerState && Request.PlayerMessageId==Speech.EventId;
        };
        FDeliveryNpcDecisionRequest PostureRequest;
        if (!TestTrue(TEXT("真实玩家可以对站立的正式居民发话"),RequestGetUp(PostureRequest))) return false;
        TestFalse(TEXT("已站立不会提供起身候选"),HasGetUp(PostureRequest));
        Agent->CancelPlan(TEXT("StandingQuestionChecked"));
        FDeliveryNpcPublicState LieDown;
        LieDown.ActionId=FGuid::NewGuid(); LieDown.Action=Action::LieDown;
        LieDown.Status=EDeliveryNpcActionStatus::Running;
        if (!TestTrue(TEXT("居民执行共享的主动躺下"),Npc->ExecuteAction(LieDown))) return false;
        Npc->StopAction(LieDown.ActionId);
        Tick(.2f);
        TestEqual(TEXT("主动躺姿使用同一个正式倒地片段"),Body->PoseIndex,static_cast<int32>(EPose::Down));
        TestTrue(TEXT("主动躺姿允许主动起身"),Npc->Combat->CanStartDeliberateRecovery());
        if (!TestTrue(TEXT("主动躺姿仍可封存一般自主决策"),Agent->BuildDecisionRequest(FGuid::NewGuid(),PostureRequest))) return false;
        TestFalse(TEXT("自主快照不公布 GetUp"),HasGetUp(PostureRequest));
        if (!TestTrue(TEXT("主动躺姿接受玩家明确发话后封存新候选"),RequestGetUp(PostureRequest))) return false;
        const auto* GetUp=PostureRequest.Candidates.FindByPredicate([](const auto& Candidate)
            { return Candidate.Action==Action::GetUp && !Candidate.TargetId.IsValid(); });
        if (!TestNotNull(TEXT("正式模型快照提供无目标的 GetUp"),GetUp)) return false;
        FDeliveryNpcDecisionProposal Proposal;
        Proposal.CapabilityVersion=PostureRequest.RequestedCapabilityVersion;
        Proposal.WorldEpoch=PostureRequest.WorldEpoch; Proposal.NpcId=PostureRequest.NpcId; Proposal.RequestId=PostureRequest.RequestId;
        Proposal.ContextRevision=PostureRequest.ContextRevision; Proposal.PlanRevision=PostureRequest.PlanRevision;
        Proposal.CandidateId=GetUp->CandidateId; Proposal.Action=GetUp->Action; Proposal.EvidenceIds=GetUp->EvidenceIds;
        // 测试正式执行入口延长极短的模型时长，并让可见起身单独作为回应
        Proposal.Duration=.1f;
        Agent->ActiveRequest=PostureRequest;
        FName Reason;
        if (!TestTrue(TEXT("服务器接受同一玩家请求的无台词短 GetUp"),Agent->TryAcceptProposal(Proposal,Reason))) return false;
        FDeliveryNpcPublicState Started;
        if (!TestTrue(TEXT("正式执行入口可开始同一 GetUp"),Agent->StartAction(Started) && Npc->ExecuteAction(Started))) return false;
        TestTrue(TEXT("短提议仍由真实恢复时长控制动作"),Started.Speech.IsEmpty() && Started.ReplyToEventId==PostureRequest.PlayerMessageId &&
            Started.Duration>=UDeliveryNpcCombatComponent::RecoveryMotionSeconds);
        const double RecoveryStarted=Npc->Combat->GetRecoveryStartTime();
        TestTrue(TEXT("主动起身实际启动恢复时钟"),RecoveryStarted>=0 && Npc->Combat->IsRecovering());
        Npc->Combat->StartDeliberateRecovery();
        TestEqual(TEXT("同一姿势正在起身时保持原恢复时钟"),Npc->Combat->GetRecoveryStartTime(),RecoveryStarted);
        Tick(1.4f);
        TestTrue(TEXT("约 1.6 秒的起身过程完成前仍倒地并禁止步行"),Npc->Combat->IsDown() &&
            Npc->Combat->GetRecoveryStartTime()==RecoveryStarted && FMath::IsNearlyZero(Npc->GetCharacterMovement()->MaxWalkSpeed));
        TestEqual(TEXT("起身未完成时正式动画继续播放 GetUp"),Body->PoseIndex,static_cast<int32>(EPose::GetUp));
        Tick(.3f);
        TestTrue(TEXT("实际恢复后才回到站立和正常步速"),!Npc->IsKnockedDown() && !Npc->Combat->IsRecovering() &&
            FMath::IsNearlyEqual(Npc->GetCharacterMovement()->MaxWalkSpeed,Npc->WalkSpeed));
        TestEqual(TEXT("主动起身结束后切回待机"),Body->PoseIndex,static_cast<int32>(EPose::Idle));
        Agent->CancelPlan(TEXT("DeliberateRecoveryTestFinished"));

        Npc->Combat->ReceiveImpact(FVector::ZeroVector,Target); Tick(.2f);
        TestEqual(TEXT("受击进入倒地"),Body->PoseIndex,static_cast<int32>(EPose::Down));
        TestTrue(TEXT("倒地动画按服务器时间推进"),Body->DownTime>.1f);
        TestFalse(TEXT("真实受击不能手动启动主动起身"),Npc->Combat->StartDeliberateRecovery());
        TestFalse(TEXT("受击倒地时角色拒绝 GetUp"),Npc->CanExecute(Action::GetUp,nullptr));
        TestFalse(TEXT("受击倒地没有可执行候选时不提交新的模型请求"),RequestGetUp(PostureRequest));
        TestTrue(TEXT("不能提交的快照仍标明实际发话的玩家"),PostureRequest.bPlayerRequest &&
            PostureRequest.RequesterPC.Get()==Player && PostureRequest.RequesterPlayerState.Get()==PlayerState);
        TestFalse(TEXT("受击倒地的模型快照省略 GetUp"),HasGetUp(PostureRequest));
        Agent->CancelPlan(TEXT("CombatDownQuestionChecked"));
        Tick(Npc->Combat->RecoverySeconds+.1f);
        TestEqual(TEXT("起身阶段显示起身片段"),Body->PoseIndex,static_cast<int32>(EPose::GetUp));
        TestTrue(TEXT("起身时间有效"),Npc->Combat->GetRecoveryStartTime()>=0);
        Tick(1.7f);
        TestEqual(TEXT("恢复后回到待机"),Body->PoseIndex,static_cast<int32>(EPose::Idle));
        TestEqual(TEXT("清除倒地的旧时间"),Npc->Combat->GetDownStartTime(),-1.0);
        TestEqual(TEXT("所有阶段均没有被表现组件改回单片段模式"),Mesh->GetAnimationMode(),EAnimationMode::AnimationBlueprint);
        TestFalse(TEXT("动画不启用 NPC 全身物理"),Mesh->IsSimulatingPhysics());
        Target->Destroy(); Npc->Destroy();
    }
    return true;
}
#endif
