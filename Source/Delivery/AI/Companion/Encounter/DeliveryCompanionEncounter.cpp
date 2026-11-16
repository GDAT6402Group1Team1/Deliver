#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Companion/Encounter/DeliveryCompanionEncounterScene.h"
#include "AI/Companion/Core/DeliveryCompanionTuning.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/World/DeliveryPickupPointComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/Companion/DeliveryCompanionMotorbike.h"
#include "World/Location/DeliveryLocationRegistry.h"
#include "Animation/AnimSequence.h"
#include "Components/SceneComponent.h"
#include "Curves/CurveVector.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/RootMotionSource.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "AIController.h"
#include "Engine/World.h"

// ── 初遇 ──
// 二狗被扔出面馆、倒地、玩家扶起结识这一段的服务器流程

/// @brief 初遇用的服务器时间；客户端还没收到 GameState 时返回 -1
double ADeliveryCompanionCharacter::GetEncounterServerTime() const
{
    if (!GetWorld()) return 0;
    const auto* State=GetWorld()->GetGameState();
    if (!HasAuthority() && (!State || !State->HasBegunPlay())) return -1;
    return State?State->GetServerWorldTimeSeconds():GetWorld()->GetTimeSeconds();
}

/// @brief 初遇开始后过了多少秒，还没开始时返回 -1
double ADeliveryCompanionCharacter::GetEncounterElapsed() const
{
    return HasEncounterStarted()?FMath::Max(0.0,GetEncounterServerTime()-Encounter.EncounterStartedAt):-1;
}

/// @brief 二狗正被扔出或正在下落时返回 true
bool ADeliveryCompanionCharacter::IsEncounterAirborne() const
{
    return Encounter.Phase==EDeliveryCompanionEncounterPhase::Ejecting || Encounter.Phase==EDeliveryCompanionEncounterPhase::Falling;
}

/// @brief 二狗倒在地上等人扶且还没有主人
bool ADeliveryCompanionCharacter::IsWaitingForHelp() const
{
    return Encounter.Phase==EDeliveryCompanionEncounterPhase::AwaitingHelp && !Companion->GetOwnerPlayerState();
}

/// @brief 等人扶时倒地动画已经播完、人也站在地面上，玩家才能开始扶
bool ADeliveryCompanionCharacter::HasFinishedEncounterFall() const
{
    const double FallSeconds=Presentation->DownAnimation?Presentation->DownAnimation->GetPlayLength():0;
    return IsWaitingForHelp() && !Super::IsKnockedDown() && GetCharacterMovement()->IsMovingOnGround() &&
        Encounter.FallAnimationStartedAt>=0 && GetEncounterServerTime()-Encounter.FallAnimationStartedAt>=FallSeconds;
}

/// @brief 正处在被扶起的阶段
bool ADeliveryCompanionCharacter::IsBeingHelpedUp() const
{
    return Encounter.Phase==EDeliveryCompanionEncounterPhase::Helping;
}

/// @brief 还没结识时一律算倒地，结识后按居民的倒地状态判断
bool ADeliveryCompanionCharacter::IsKnockedDown() const
{
    return Encounter.Phase!=EDeliveryCompanionEncounterPhase::Joined || Super::IsKnockedDown();
}

/// @brief 服务器切换初遇阶段，记下开始时间、编号加一，并立即同步
void ADeliveryCompanionCharacter::SetEncounterPhase(EDeliveryCompanionEncounterPhase Phase)
{
    if (!HasAuthority() || Encounter.Phase==Phase) return;
    Encounter.Phase=Phase;
    Encounter.PhaseStartedAt=GetEncounterServerTime();
    ++Encounter.Revision;
    OnRep_Encounter();
    ForceNetUpdate();
}

/// @brief 有人开始扶时从等人扶切到被扶起阶段
void ADeliveryCompanionCharacter::BeginHelpUp()
{
    if (!HasAuthority() || Encounter.Phase!=EDeliveryCompanionEncounterPhase::AwaitingHelp || !Companion->GetOwnerPlayerState()) return;
    bAnnouncedJoining=false;
    SetEncounterPhase(EDeliveryCompanionEncounterPhase::Helping);
}

/// @brief 失去主人后就在当前位置倒下等新主人扶，不回面馆重演初遇
void ADeliveryCompanionCharacter::ResetForNewOwner()
{
    if (!HasAuthority()) return;
    bAnnouncedJoining=false;
    Encounter.FallAnimationStartedAt=GetEncounterServerTime();
    // 失去主人后在当前实际位置等人，不能重新传送回面馆播放初遇
    SetEncounterPhase(GetCharacterMovement()->IsMovingOnGround()?EDeliveryCompanionEncounterPhase::AwaitingHelp:EDeliveryCompanionEncounterPhase::Falling);
}

/// @brief 按初遇阶段切换表现：骑车时隐藏本体，抛出阶段播放抛出动作，结识后恢复正常
void ADeliveryCompanionCharacter::OnRep_Encounter()
{
    const auto* AttachedBike=Cast<ADeliveryCompanionMotorbike>(GetAttachParentActor());
    if (Encounter.Phase==EDeliveryCompanionEncounterPhase::Joined &&
        (Companion->IsUsingErrandVehicle() || (AttachedBike && AttachedBike->CompanionSeat==GetRootComponent()->GetAttachParent())))
    {
        // 初遇与座椅分开复制，原身体维持骑乘状态，专属车继续显示骑姿替身
        RemoveEjectionMotion();
        SetActorHiddenInGame(true);
        SetActorEnableCollision(false);
        if (GetCharacterMovement()->MovementMode!=MOVE_None) GetCharacterMovement()->DisableMovement();
        UpdateEncounterPresentation();
        return;
    }
    const bool bEncounterHidden=Encounter.Phase==EDeliveryCompanionEncounterPhase::Dormant || Encounter.Phase==EDeliveryCompanionEncounterPhase::OpeningDoor;
    SetActorHiddenInGame(bEncounterHidden);
    SetActorEnableCollision(!bEncounterHidden);
    auto* Movement=GetCharacterMovement();
    if (Encounter.Phase==EDeliveryCompanionEncounterPhase::Ejecting)
    {
        ApplyEjectionMotion();
    }
    else
    {
        RemoveEjectionMotion();
        if (bEncounterHidden)
        {
            Movement->StopMovementImmediately();
            Movement->DisableMovement();
        }
        else if (Encounter.Phase==EDeliveryCompanionEncounterPhase::Falling)
        {
            Movement->SetMovementMode(MOVE_Falling);
        }
        else if (Movement->MovementMode==MOVE_None || Movement->MovementMode==MOVE_Flying)
        {
            // 正常受击后的 Falling 由 CharacterMovement 决定何时落地，不被迟到的场景引用覆盖
            Movement->SetMovementMode(MOVE_Walking);
        }
    }
    UpdateEncounterPresentation();
}

/// @brief 开门和抛出阶段显示老板，并按门的进度同步老板的动画时间
void ADeliveryCompanionCharacter::UpdateEncounterPresentation()
{
    if (!IsValid(EncounterScene) || EncounterScene->GetWorld()!=GetWorld() || !EncounterScene->HasActorBegunPlay()) return;
    const bool bShowBoss=Encounter.Phase==EDeliveryCompanionEncounterPhase::OpeningDoor ||
        Encounter.Phase==EDeliveryCompanionEncounterPhase::Ejecting;
    // 门被人挡住时老板也保持预备姿态，不能先演完挥赶、隔几秒才让二狗飞出
    double BossElapsed=FMath::Clamp(EncounterScene->DoorProgress,0.f,1.f)*EncounterScene->DoorTravelSeconds;
    if (Encounter.Phase==EDeliveryCompanionEncounterPhase::Ejecting)
    {
        BossElapsed=EncounterScene->DoorTravelSeconds+FMath::Max(0.0,GetEncounterServerTime()-Encounter.PhaseStartedAt);
    }
    EncounterScene->UpdateBossPresentation(BossElapsed,bShowBoss);
}

/// @brief 初遇绑定的任务已经开始、进行中或已完成时返回 true
bool ADeliveryCompanionCharacter::HasEncounterOrderStarted() const
{
    const auto* Manager=UDeliveryTaskManagerComponent::Get(this);
    if (!Manager || EncounterTaskId.IsNone()) return false;
    // 完成记录仍由任务管理器保留，错过开场或门口受阻后回来也能结识二狗
    TArray<UDeliveryTaskDefinition*> Tasks;
    for (const auto Status:{EDeliveryTaskStatus::AwaitingPickup,EDeliveryTaskStatus::InProgress,EDeliveryTaskStatus::Completed})
    {
        Manager->GetTasksByStatus(Status,Tasks);
        for (const auto* Task:Tasks)
            if (IsValid(Task) && Task->TaskId==EncounterTaskId) return true;
    }
    return false;
}

/// @brief Pawn 在门口触发范围内，或者是在取餐点附近的玩家
bool ADeliveryCompanionCharacter::CanWitnessEncounter(const APawn* Pawn) const
{
    if (!IsValid(Pawn) || Pawn->GetWorld()!=GetWorld() || !IsValid(EncounterScene) || EncounterScene->GetWorld()!=GetWorld()) return false;
    const FVector Trigger=EncounterScene->GetTriggerWorldLocation();
    bool bInRange=FVector::DistSquared(Pawn->GetActorLocation(),Trigger)<=FMath::Square(Companion->GetTuning().EncounterRadius);
    // 取餐范围可能比门口范围大，沿实际取餐锚点检查，避免玩家拿完面条折返却没有看见初遇
    if (!bInRange && Pawn->IsA<ADeliveryCharacter>())
    {
        const auto* Manager=UDeliveryTaskManagerComponent::Get(this);
        const auto* Registry=UDeliveryLocationRegistry::Get(this);
        if (Manager && Registry && !EncounterTaskId.IsNone())
        {
            TArray<UDeliveryTaskDefinition*> Tasks;
            for (const auto Status:{EDeliveryTaskStatus::AwaitingPickup,EDeliveryTaskStatus::InProgress,EDeliveryTaskStatus::Completed})
            {
                Manager->GetTasksByStatus(Status,Tasks);
                for (const auto* Task:Tasks)
                {
                    if (!IsValid(Task) || Task->TaskId!=EncounterTaskId || Task->PickupLocationId.IsNone()) continue;
                    for (const auto* Location:Registry->ResolveAllActors(Task->PickupLocationId))
                    {
                        if (!IsValid(Location) || Location->GetWorld()!=GetWorld() || !UDeliveryPickupPointComponent::FindOn(Location)) continue;
                        const auto* Interaction=UDeliveryInteractableComponent::FindOn(Location,EDeliveryInteractionKey::PickupE);
                        // 已取件和已完成的订单仍可见初遇，不用当前能否再次发包裹判断空间范围
                        if (Interaction && Interaction->CanInteract(Pawn)) { bInRange=true; break; }
                    }
                    if (bInRange) break;
                }
                if (bInRange) break;
            }
        }
    }
    if (!bInRange) return false;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(CompanionEncounterTrigger),false,this);
    Params.AddIgnoredActor(Pawn);
    FHitResult Hit;
    // 两种入口都必须看见门外锚点，不能因为能取餐就忽略店铺或其他遮挡
    return !GetWorld()->LineTraceSingleByChannel(Hit,Pawn->GetPawnViewLocation(),Trigger,ECC_Visibility,Params);
}

/// @brief 有玩家在门口附近，触发后的范围放宽，让玩家可以退到门边让开
bool ADeliveryCompanionCharacter::HasNearbyEncounterWitness() const
{
    const FVector Trigger=EncounterScene->GetTriggerWorldLocation();
    // 已触发后允许玩家退到门侧让开，门叶和挡路物不再反过来取消演出
    const float ResumeRadius=FMath::Max(800.f,Companion->GetTuning().EncounterRadius+300.f);
    for (auto It=GetWorld()->GetPlayerControllerIterator();It;++It)
    {
        const auto* Player=It->Get();
        const APawn* Pawn=Player?Player->GetPawn():nullptr;
        if (IsValid(Pawn) && !Pawn->IsActorBeingDestroyed() && Pawn->GetWorld()==GetWorld() &&
            FVector::DistSquared(Pawn->GetActorLocation(),Trigger)<=FMath::Square(ResumeRadius)) return true;
    }
    return false;
}

/// @brief 场景和抛出曲线都配好、任务已开始、有玩家在旁边时开始初遇并请求开门
bool ADeliveryCompanionCharacter::TryStartEncounter()
{
    if (!IsValid(EncounterScene) || EncounterScene->GetWorld()!=GetWorld() || !Companion->GetTuning().EjectionCurve)
    {
        if (!bReportedEncounterConfiguration)
        {
            bReportedEncounterConfiguration=true;
            UE_LOG(LogTemp,Warning,TEXT("CompanionEncounter %s requires a scene and an ejection curve"),*GetName());
        }
        return false;
    }
    if (!HasEncounterOrderStarted()) return false;
    for (auto It=GetWorld()->GetPlayerControllerIterator();It;++It)
    {
        const auto* Player=It->Get();
        const APawn* Pawn=Player?Player->GetPawn():nullptr;
        if (!CanWitnessEncounter(Pawn)) continue;
        if (!EncounterScene->RequestDoorOpen(true)) return false;
        Encounter.EncounterStartedAt=GetEncounterServerTime();
        LastNearbyEncounterWitnessAt=Encounter.EncounterStartedAt;
        Encounter.FailureReason=NAME_None;
        SetEncounterPhase(EDeliveryCompanionEncounterPhase::OpeningDoor);
        return true;
    }
    return false;
}

/// @brief 检查抛出路线：参数是否合法、路线上有没有东西挡住
ADeliveryCompanionCharacter::EEjectionPathStatus ADeliveryCompanionCharacter::GetEjectionPathStatus(
    const FDeliveryCompanionEncounterState& State) const
{
    if (!State.EjectionCurve || State.EjectionStart.ContainsNaN() || State.EjectionTarget.ContainsNaN() ||
        State.EjectionStart.Equals(State.EjectionTarget,1) || !FMath::IsFinite(State.EjectionDuration) ||
        State.EjectionDuration<=0 || State.EjectionDuration>5) return EEjectionPathStatus::Invalid;
    FRootMotionSource_MoveToForce Path;
    Path.StartLocation=State.EjectionStart;
    Path.TargetLocation=State.EjectionTarget;
    Path.PathOffsetCurve=State.EjectionCurve;
    // 使用引擎同一曲线采样与朝向变换，预检不能另算一条看似相近的抛物线
    if (!Path.GetPathOffsetInWorldSpace(0).IsNearlyZero(1) || !Path.GetPathOffsetInWorldSpace(1).IsNearlyZero(1))
        return EEjectionPathStatus::Invalid;
    FCollisionObjectQueryParams Objects;
    for (const auto Channel:{ECC_WorldStatic,ECC_WorldDynamic,ECC_Pawn,ECC_Vehicle,ECC_PhysicsBody}) Objects.AddObjectTypesToQuery(Channel);
    FCollisionQueryParams Params(SCENE_QUERY_STAT(CompanionEjectionClearance),false,this);
    const auto Capsule=FCollisionShape::MakeCapsule(GetSimpleCollisionRadius(),GetSimpleCollisionHalfHeight());
    if (GetWorld()->OverlapAnyTestByObjectType(State.EjectionStart,FQuat::Identity,Objects,Capsule,Params)) return EEjectionPathStatus::Blocked;
    FVector Previous=State.EjectionStart;
    const int32 Steps=FMath::Clamp(FMath::CeilToInt(FVector::Distance(State.EjectionStart,State.EjectionTarget)/25.f),32,128);
    for (int32 Index=1;Index<=Steps;++Index)
    {
        const float Progress=static_cast<float>(Index)/Steps;
        const FVector Position=FMath::Lerp(State.EjectionStart,State.EjectionTarget,Progress)+Path.GetPathOffsetInWorldSpace(Progress);
        if (Position.ContainsNaN()) return EEjectionPathStatus::Invalid;
        FHitResult Hit;
        if (GetWorld()->SweepSingleByObjectType(Hit,Previous,Position,FQuat::Identity,Objects,Capsule,Params)) return EEjectionPathStatus::Blocked;
        Previous=Position;
    }
    return EEjectionPathStatus::Clear;
}

/// @brief 门开好后计算抛出起点、落点和时长，路线通畅就切到抛出阶段，被挡就等待
void ADeliveryCompanionCharacter::TryStartEjection()
{
    FDeliveryCompanionEncounterState Proposed=Encounter;
    Proposed.EjectionStart=EncounterScene->GetThrowStartWorldLocation();
    Proposed.EjectionTarget=EncounterScene->GetThrowLandingWorldLocation();
    Proposed.EjectionDuration=Companion->GetTuning().EjectionDuration;
    Proposed.EjectionCurve=Companion->GetTuning().EjectionCurve;
    const auto PathStatus=GetEjectionPathStatus(Proposed);
    if (PathStatus==EEjectionPathStatus::Invalid)
    {
        ResetUnstartedEncounter(TEXT("InvalidEjectionPath"));
        return;
    }
    if (PathStatus==EEjectionPathStatus::Blocked)
    {
        EncounterScene->SetEncounterObstruction(EDeliveryCompanionEncounterObstruction::EjectionPath);
        return;
    }
    Companion->StopFollowMovement();
    Agent->CancelPlan(TEXT("CompanionEncounter"));
    if (auto* AI=Cast<AAIController>(GetController())) { AI->StopMovement(); AI->ClearFocus(EAIFocusPriority::Gameplay); }
    // 唯一一次初始定位发生在角色仍隐藏时，之后的整段位移交给带碰撞的 CharacterMovement
    if (!SetActorLocationAndRotation(Proposed.EjectionStart,EncounterScene->GetThrowStartTransform().Rotator(),false,nullptr,ETeleportType::TeleportPhysics))
    {
        ResetUnstartedEncounter(TEXT("EjectionStartUnavailable"));
        return;
    }
    EncounterScene->SetEncounterObstruction(EDeliveryCompanionEncounterObstruction::None);
    Encounter=Proposed;
    Encounter.FallAnimationStartedAt=GetEncounterServerTime();
    bReportedLandingTimeout=false;
    SetEncounterPhase(EDeliveryCompanionEncounterPhase::Ejecting);
    if (!EjectionMotionId) BeginEncounterFall(TEXT("MotionStartFailed"));
}

/// @brief 给角色移动加上抛出的根运动；客户端要等本机门叶也开好才开始
void ADeliveryCompanionCharacter::ApplyEjectionMotion()
{
    // 门与角色分属两个复制 Actor，先等本机门叶到达服务器确认的开门姿势
    if (!HasAuthority() && (!IsValid(EncounterScene) || !EncounterScene->IsDoorPresentationOpen())) return;
    auto* Movement=GetCharacterMovement();
    const auto Existing=Movement->GetRootMotionSourceByID(EjectionMotionId);
    if (!HasAuthority() && AppliedMotionRevision==Encounter.Revision && !Existing.IsValid() && !bClientMotionWasDisabled)
    {
        // 正常完成的源可能已由 CMC 清理，不能因为估计的服务器时间略慢而重播尾段
        FinishedMotionRevision=Encounter.Revision;
    }
    if (AppliedMotionRevision==Encounter.Revision && Existing.IsValid())
    {
        if (Existing->Status.HasFlag(ERootMotionSourceStatusFlags::Finished)) FinishedMotionRevision=Encounter.Revision;
        else if (!Existing->Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval))
        {
            // 阶段先到而旧移动模式后到时，保留本次源并恢复它所需的模式
            if (Movement->MovementMode!=MOVE_Flying) Movement->SetMovementMode(MOVE_Flying);
            bClientMotionWasDisabled=false;
            return;
        }
    }
    RemoveEjectionMotion();
    const double Now=GetEncounterServerTime();
    if (FinishedMotionRevision==Encounter.Revision || Now<Encounter.PhaseStartedAt || Now<0 ||
        !Encounter.EjectionCurve || !FMath::IsFinite(Encounter.EjectionDuration) || Encounter.EjectionDuration<=0) return;
    const float Elapsed=static_cast<float>(Now-Encounter.PhaseStartedAt);
    if (Elapsed>=Encounter.EjectionDuration) { FinishedMotionRevision=Encounter.Revision; return; }
    bSavedOrientToMovement=Movement->bOrientRotationToMovement;
    bSavedControllerRotation=Movement->bUseControllerDesiredRotation;
    bEjectionRotationSaved=true;
    Movement->bOrientRotationToMovement=false;
    Movement->bUseControllerDesiredRotation=false;
    Movement->StopMovementImmediately();
    Movement->SetMovementMode(MOVE_Flying);
    auto Source=MakeShared<FRootMotionSource_MoveToForce>();
    Source->InstanceName=FName(*FString::Printf(TEXT("CompanionEjection_%d"),Encounter.Revision));
    Source->Priority=1000;
    Source->AccumulateMode=ERootMotionAccumulateMode::Override;
    Source->bInLocalSpace=false;
    Source->Settings.SetFlag(ERootMotionSourceSettingsFlags::UseSensitiveLiftoffCheck);
    Source->StartLocation=Encounter.EjectionStart;
    Source->TargetLocation=Encounter.EjectionTarget;
    Source->Duration=Encounter.EjectionDuration;
    Source->bRestrictSpeedToExpected=true;
    Source->PathOffsetCurve=Encounter.EjectionCurve;
    Source->FinishVelocityParams.Mode=ERootMotionFinishVelocityMode::SetVelocity;
    Source->FinishVelocityParams.SetVelocity=FVector::ZeroVector;
    // 模拟代理也创建相同参数的源，CMC 的复制只匹配已有源，不负责凭空创建它
    if (!HasAuthority()) Source->SetTime(Elapsed);
    EjectionMotionId=Movement->ApplyRootMotionSource(Source);
    if (EjectionMotionId)
    {
        AppliedMotionRevision=Encounter.Revision;
        bClientMotionWasDisabled=false;
    }
}

/// @brief 客户端在抛出阶段移动模式变成 None 时记下来，之后重新加上抛出动作
void ADeliveryCompanionCharacter::OnMovementModeChanged(EMovementMode PreviousMovementMode,uint8 PreviousCustomMode)
{
    Super::OnMovementModeChanged(PreviousMovementMode,PreviousCustomMode);
    if (!HasAuthority() && Encounter.Phase==EDeliveryCompanionEncounterPhase::Ejecting &&
        GetCharacterMovement()->MovementMode==MOVE_None) bClientMotionWasDisabled=true;
}

/// @brief 去掉抛出根运动，恢复原来的转向设置
void ADeliveryCompanionCharacter::RemoveEjectionMotion()
{
    auto* Movement=GetCharacterMovement();
    if (EjectionMotionId) Movement->RemoveRootMotionSourceByID(EjectionMotionId);
    EjectionMotionId=0;
    if (bEjectionRotationSaved)
    {
        Movement->bOrientRotationToMovement=bSavedOrientToMovement;
        Movement->bUseControllerDesiredRotation=bSavedControllerRotation;
        bEjectionRotationSaved=false;
    }
}

/// @brief 抛出结束或被打断时停下移动，切到下落阶段
void ADeliveryCompanionCharacter::BeginEncounterFall(FName Reason)
{
    if (!HasAuthority()) return;
    Encounter.FailureReason=Reason;
    RemoveEjectionMotion();
    GetCharacterMovement()->StopMovementImmediately();
    SetEncounterPhase(EDeliveryCompanionEncounterPhase::Falling);
    if (!Reason.IsNone()) UE_LOG(LogTemp,Warning,TEXT("CompanionEncounter %s ejection ended: %s"),*GetName(),*Reason.ToString());
}

/// @brief 初遇没开始成功时关门、清掉阻挡状态，回到等待开始
void ADeliveryCompanionCharacter::ResetUnstartedEncounter(FName Reason)
{
    Encounter.FailureReason=Reason;
    Encounter.EncounterStartedAt=-1;
    if (IsValid(EncounterScene))
    {
        EncounterScene->SetEncounterObstruction(EDeliveryCompanionEncounterObstruction::None);
        EncounterScene->RequestDoorOpen(false);
    }
    SetEncounterPhase(EDeliveryCompanionEncounterPhase::Dormant);
    NextEncounterCheck=GetWorld()->GetTimeSeconds()+3;
    UE_LOG(LogTemp,Log,TEXT("CompanionEncounter %s reset before ejection: %s"),*GetName(),*Reason.ToString());
}

/// @brief 服务器按初遇阶段推进：等待开始、开门、抛出、下落、等人扶和被扶起
void ADeliveryCompanionCharacter::UpdateEncounter()
{
    const double Now=GetEncounterServerTime();
    if (Encounter.Phase==EDeliveryCompanionEncounterPhase::Dormant)
    {
        if (GetWorld()->GetTimeSeconds()>=NextEncounterCheck)
        { NextEncounterCheck=GetWorld()->GetTimeSeconds()+.25; TryStartEncounter(); }
    }
    else if (Encounter.Phase==EDeliveryCompanionEncounterPhase::OpeningDoor)
    {
        if (!IsValid(EncounterScene) || EncounterScene->GetWorld()!=GetWorld()) ResetUnstartedEncounter(TEXT("SceneLost"));
        else if (GetWorld()->GetTimeSeconds()>=NextEncounterCheck)
        {
            // 被玩家挡住时按需重查整条路径，真正飞行期间仍由 CMC 每帧处理碰撞
            NextEncounterCheck=GetWorld()->GetTimeSeconds()+.2;
            const bool bNearby=HasNearbyEncounterWitness();
            if (bNearby) LastNearbyEncounterWitnessAt=Now;
            if (Now-LastNearbyEncounterWitnessAt>=Companion->GetTuning().DoorWaitSeconds)
                ResetUnstartedEncounter(TEXT("WitnessLeft"));
            else if (!EncounterScene->IsDoorOpen())
                EncounterScene->SetEncounterObstruction(EncounterScene->IsDoorOpeningBlocked()?
                    EDeliveryCompanionEncounterObstruction::Doorway:EDeliveryCompanionEncounterObstruction::None);
            else if (bNearby) TryStartEjection();
            else EncounterScene->SetEncounterObstruction(EDeliveryCompanionEncounterObstruction::None);
        }
    }
    else if (Encounter.Phase==EDeliveryCompanionEncounterPhase::Ejecting)
    {
        const auto Motion=GetCharacterMovement()->GetRootMotionSourceByID(EjectionMotionId);
        if (!Motion.IsValid() || Motion->Status.HasFlag(ERootMotionSourceStatusFlags::Finished))
        {
            const bool bArrived=FVector::Dist(GetActorLocation(),Encounter.EjectionTarget)<=Companion->GetTuning().EjectionArrivalTolerance;
            BeginEncounterFall(bArrived?FName(NAME_None):FName(TEXT("EjectionBlocked")));
        }
    }
    else if (Encounter.Phase==EDeliveryCompanionEncounterPhase::Falling)
    {
        const auto* Movement=GetCharacterMovement();
        // 某些移动状态直接恢复 Walking，使用 CMC 当前实际支撑面补上漏掉的落地事件
        if (Movement->IsMovingOnGround() && Movement->CurrentFloor.IsWalkableFloor() &&
            Movement->IsWalkable(Movement->CurrentFloor.HitResult)) ConfirmEncounterLanding();
        else if (Now-Encounter.PhaseStartedAt>10 && !bReportedLandingTimeout)
        {
            bReportedLandingTimeout=true;
            if (Encounter.FailureReason.IsNone()) Encounter.FailureReason=TEXT("LandingNotConfirmed");
            ForceNetUpdate();
            UE_LOG(LogTemp,Warning,TEXT("CompanionEncounter %s still has no confirmed landing"),*GetName());
        }
    }
    else if (Encounter.Phase==EDeliveryCompanionEncounterPhase::Helping && !Super::IsKnockedDown() &&
        GetCharacterMovement()->IsMovingOnGround() && Now-Encounter.PhaseStartedAt>=Companion->GetTuning().HelpRecoverySeconds)
    {
        SetEncounterPhase(EDeliveryCompanionEncounterPhase::Joined);
    }
    if (Encounter.Phase==EDeliveryCompanionEncounterPhase::Joined && bKeepDoorOpenForRescue &&
        GetWorld()->GetTimeSeconds()>=NextEncounterCheck)
    {
        NextEncounterCheck=GetWorld()->GetTimeSeconds()+.2;
        const auto* OwnerState=Companion->GetOwnerPlayerState();
        const auto* PC=OwnerState?OwnerState->GetPlayerController():nullptr;
        const APawn* Rescuer=PC?PC->GetPawn():nullptr;
        if (!IsValid(EncounterScene)) bKeepDoorOpenForRescue=false;
        else if (IsValid(Rescuer) && EncounterScene->IsPastDoorway(Rescuer->GetActorLocation()) &&
            EncounterScene->IsPastDoorway(GetActorLocation()) && EncounterScene->RequestDoorOpen(false))
        {
            bKeepDoorOpenForRescue=false;
        }
    }
    if (Encounter.Phase==EDeliveryCompanionEncounterPhase::Joined && Companion->GetOwnerPlayerState() && !bAnnouncedJoining)
    {
        const auto* PC=Companion->GetOwnerPlayerState()->GetPlayerController();
        APawn* Rescuer=PC?PC->GetPawn():nullptr;
        if (IsValid(Rescuer) && Rescuer->GetWorld()==GetWorld())
        {
            FDeliveryNpcObservation Joined;
            Joined.EventId=FGuid::NewGuid(); Joined.Kind=TEXT("CompanionJoined");
            Joined.Subject=Rescuer; Joined.Source=EDeliveryNpcEvidenceSource::ActionResult; Joined.Lifetime=60;
            Joined.Description=TEXT("刚才这名玩家亲手扶我起来，我现在已站起来并认他为主人，准备继续陪他送快递。没有亲历的旧订单不能说一起完成，也不能虚构已经送达。");
            if (Agent->ObserveEvent(Joined)) { bAnnouncedJoining=true; WakeDecision(); }
        }
    }
}

/// @brief 下落阶段落到地面时调用 ConfirmEncounterLanding
void ADeliveryCompanionCharacter::Landed(const FHitResult& Hit)
{
    Super::Landed(Hit);
    if (!HasAuthority() || Encounter.Phase!=EDeliveryCompanionEncounterPhase::Falling || !GetCharacterMovement()->IsWalkable(Hit)) return;
    // Landed 发生在 CMC 切回 Walking 之前，阶段先复制，下一帧移动状态才允许扶起
    ConfirmEncounterLanding();
}

/// @brief 切到等人扶；二狗被截在店里时门先开着，否则关门
void ADeliveryCompanionCharacter::ConfirmEncounterLanding()
{
    // 先确认真实支撑面。被截停在店内时保留通行，等两人都离开再关门
    bKeepDoorOpenForRescue=IsValid(EncounterScene) && EncounterScene->IsDoorOpen() &&
        !EncounterScene->IsPastDoorway(GetActorLocation());
    SetEncounterPhase(EDeliveryCompanionEncounterPhase::AwaitingHelp);
    if (IsValid(EncounterScene) && !bKeepDoorOpenForRescue) EncounterScene->RequestDoorOpen(false);
}

/// @brief 在空中被撞时改为下落，其余情况交给居民的受击流程
void ADeliveryCompanionCharacter::ReceiveImpactFrom(const FVector& Impulse,const FVector& Point,AActor* Source)
{
    if (!HasAuthority() || Impulse.ContainsNaN() || IsHidden()) return;
    // 战斗组件会拒绝倒地后的重复冲击，也不能让被拒绝的冲击反复清零空中速度
    if (Super::IsKnockedDown()) return;
    if (IsEncounterAirborne())
    {
        BeginEncounterFall(TEXT("InterruptedByImpact"));
        Encounter.FallAnimationStartedAt=GetEncounterServerTime();
    }
    Super::ReceiveImpactFrom(Impulse,Point,Source);
    if (Super::IsKnockedDown()) Companion->RecordOwnerVehicleHit(Source);
}
