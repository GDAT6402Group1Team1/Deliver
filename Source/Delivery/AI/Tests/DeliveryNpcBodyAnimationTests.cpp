#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "AI/Presentation/DeliveryNpcBodyAnimInstance.h"
#include "AI/Presentation/DeliveryNpcConversationAnimInstance.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
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

        // 测试提供服务器同步的动作状态，真正的片段选择与骨骼求值使用正式动画蓝图
        auto& State=Npc->Agent->PublicState; State.Status=EDeliveryNpcActionStatus::Running;
        State.ActionId=FGuid::NewGuid();
        State.Action=EDeliveryNpcAction::Pace; Npc->GetCharacterMovement()->Velocity=FVector(80,0,0); Tick(.2f);
        TestEqual(TEXT("踱步使用独立片段"),Body->PoseIndex,static_cast<int32>(EPose::Pace));
        State.Action=EDeliveryNpcAction::StrikeOnce; State.ActionId=FGuid::NewGuid(); Tick(.2f);
        TestEqual(TEXT("还没挥拳时显示快步靠近"),Body->PoseIndex,static_cast<int32>(EPose::Hurry));
        Npc->GetCharacterMovement()->Velocity=FVector::ZeroVector;
        auto* Target=World->SpawnActor<ADeliveryNpcCharacter>(Npc->GetActorLocation()+FVector(100,0,0),FRotator::ZeroRotator,Spawn);
        Target->SetActorTickEnabled(false);
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

        Npc->Combat->ReceiveImpact(FVector::ZeroVector,Target); Tick(.2f);
        TestEqual(TEXT("受击进入倒地"),Body->PoseIndex,static_cast<int32>(EPose::Down));
        TestTrue(TEXT("倒地动画按服务器时间推进"),Body->DownTime>.1f);
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
