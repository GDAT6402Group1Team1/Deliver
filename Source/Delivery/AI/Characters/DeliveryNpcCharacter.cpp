#include "AI/Characters/DeliveryNpcCharacter.h"
#include "Components/SkeletalMeshComponent.h"
#include "AI/Characters/DeliveryNpcProfile.h"
#include "AI/Characters/DeliveryNpcAIController.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Presentation/Face/DeliveryNpcLookComponent.h"
#include "AI/Actions/DeliveryVehicleHoldComponent.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Conversation/DeliveryNpcConversationTrace.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Combat/Types/DeliveryCollisionChannels.h"
#include "Vehicle/Motorbike/DeliveryMotorbike.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "HAL/IConsoleManager.h"
#include "Engine/World.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystem/Abilities/GA_NpcStrike.h"
#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"

namespace
{
// 控制台变量：设为 1 时，导航不可达会写出原因
TAutoConsoleVariable<int32> CVarNpcNavigationDiagnostics(TEXT("Delivery.Npc.NavigationDiagnostics"), 0,
                                                         TEXT("记录 NPC 行动候选的导航起点、终点和路径结果"), ECVF_Default);
}

// 创建 GAS、决策、战斗、表现、注视和交谈组件，由 ADeliveryNpcAIController 控制
ADeliveryNpcCharacter::ADeliveryNpcCharacter()
{
    bReplicates = true;
    SetReplicateMovement(true);
    PrimaryActorTick.bCanEverTick = true;
    AIControllerClass = ADeliveryNpcAIController::StaticClass();
    AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;
    AbilitySystem = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystem"));
    AbilitySystem->SetIsReplicated(true);
    AbilitySystem->SetReplicationMode(EGameplayEffectReplicationMode::Minimal);
    StrikeAbilityClass = UGA_NpcStrike::StaticClass();
    Agent = CreateDefaultSubobject<UDeliveryNpcAgentComponent>(TEXT("Agent"));
    Combat = CreateDefaultSubobject<UDeliveryNpcCombatComponent>(TEXT("Combat"));
    Presentation = CreateDefaultSubobject<UDeliveryNpcPresentationComponent>(TEXT("Presentation"));
    Look = CreateDefaultSubobject<UDeliveryNpcLookComponent>(TEXT("Look"));
    ConversationInteractable = CreateDefaultSubobject<UDeliveryInteractableComponent>(TEXT("Conversation"));
    ConversationInteractable->PromptText = FText::FromString(TEXT("交谈"));
    ConversationInteractable->InteractRadius = 600;
    GetCapsuleComponent()->InitCapsuleSize(34, 88);
    GetCapsuleComponent()->SetCollisionObjectType(ECC_Pawn);
    GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
    // Pawn 胶囊负责贴地；专用查询体只供车撞和出拳发现，不能用 Ignore 地面的通道行走
    ImpactSensor = CreateDefaultSubobject<UCapsuleComponent>(TEXT("ImpactSensor"));
    ImpactSensor->SetupAttachment(GetRootComponent());
    ImpactSensor->InitCapsuleSize(34, 88);
    ImpactSensor->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    ImpactSensor->SetCollisionObjectType(DeliveryCollision::Pedestrian);
    ImpactSensor->SetCollisionResponseToAllChannels(ECR_Overlap);
    ImpactSensor->SetGenerateOverlapEvents(false);
    GetMesh()->SetRelativeLocationAndRotation(FVector(0, 0, -88), FRotator(0, -90, 0));
    GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    GetCharacterMovement()->MaxWalkSpeed = 180;
    // 由 AI 的关注对象和导航方向提供朝向，停步时 LookAt 也能真正转身
    GetCharacterMovement()->bOrientRotationToMovement = false;
    GetCharacterMovement()->bUseControllerDesiredRotation = true;
    GetCharacterMovement()->RotationRate = FRotator(0, 240, 0);
    bUseControllerRotationYaw = false;
    DisplayName = FText::FromString(TEXT("居民"));
}

// 服务器授予出拳技能，读取人物配置，设置交谈提示和碰撞，订阅决策结束
void ADeliveryNpcCharacter::BeginPlay()
{
    Super::BeginPlay();
    AbilitySystem->InitAbilityActorInfo(this, this);
    if (HasAuthority() && StrikeAbilityClass)
        StrikeAbilityHandle = AbilitySystem->GiveAbility(FGameplayAbilitySpec(StrikeAbilityClass, 1, INDEX_NONE, this));

    // 读取人物配置表中的姓名、性格、声音和动作表现
    ApplyProfile();
    ConversationInteractable->InteractRadius = 600;
    GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;

    // 碰撞设置
    GetCapsuleComponent()->SetCollisionObjectType(ECC_Pawn);
    // 居民靠近和拽车不压缩玩家镜头，场景墙壁仍按原 Camera 通道防穿透
    GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
    ImpactSensor->SetCapsuleSize(GetCapsuleComponent()->GetUnscaledCapsuleRadius(),
                                 GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight());

    // 头顶的 [T] 交谈提示，交互检查和打开交谈交给本类的 CanTalk、OpenConversation
    ConversationInteractable->bPromptAtTarget = true;
    ConversationInteractable->bUseConversationIcon = false;
    ConversationInteractable->PromptText = FText::Format(NSLOCTEXT("Delivery", "NpcTalkPrompt", "[T] 和{0}说话"), DisplayName);
    ConversationInteractable->PromptOffset = FVector(0, 0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - 20);
    ConversationInteractable->CanUse.BindUObject(this, &ADeliveryNpcCharacter::CanTalk);
    ConversationInteractable->Use.BindUObject(this, &ADeliveryNpcCharacter::OpenConversation);

    // 决策结束后按结果安排重试，开局 2 秒后才发第一次请求
    Agent->OnDecisionFinished.AddUniqueDynamic(this, &ADeliveryNpcCharacter::OnDecisionFinished);
    NextDecision = GetWorld()->GetTimeSeconds() + 2;
}

// 从 DT_NpcProfiles 读取一行：姓名、性格、TTS 音色、表情和动作表现
bool ADeliveryNpcCharacter::ApplyProfile()
{
    if (!Profile.DataTable || Profile.RowName.IsNone() ||
        Profile.DataTable->GetRowStruct() != FDeliveryNpcProfileRow::StaticStruct())
        return false;
    const auto* Row = Profile.DataTable->FindRow<FDeliveryNpcProfileRow>(Profile.RowName, TEXT("NpcProfile"), false);
    if (!Row || Row->DisplayName.IsEmpty() || Row->Persona.IsEmpty() || Row->Persona.Len() > 1024)
        return false;
    const FString VoiceName = Row->TtsVoice.ToString().ToLower();
    if (!Row->TtsVoice.IsNone() && VoiceName != TEXT("male") && VoiceName != TEXT("female"))
    {
        UE_LOG(LogTemp, Warning,
               TEXT("NPC profile %s has unsupported TTS voice %s; supported: male/female. Existing settings kept."),
               *Profile.RowName.ToString(), *Row->TtsVoice.ToString());
        return false;
    }
    DisplayName = Row->DisplayName;
    Agent->Persona = Row->Persona;
    Presentation->VoiceSet = Row->VoiceSet;
    if (!Row->TtsVoice.IsNone())
        Presentation->TtsVoice = FName(*VoiceName);
    BehaviorPresentation = Row->BehaviorPresentation;
    return true;
}

// 查找某个行为标签对应的动画，骨架不兼容或参数无效时返回空
const FDeliveryNpcBehaviorPresentation* ADeliveryNpcCharacter::FindBehaviorPresentation(FGameplayTag Behavior) const
{
    const auto* Variant = Behavior.IsValid() ? BehaviorPresentation.Find(Behavior) : nullptr;
    if (!Variant || !IsValid(Variant->Animation) || !FMath::IsFinite(Variant->PlayRate) || Variant->PlayRate < .25f ||
        Variant->PlayRate > 3.f || !FMath::IsFinite(Variant->MovementReferenceSpeed) || Variant->MovementReferenceSpeed < 0 ||
        Variant->MovementReferenceSpeed > 500.f)
        return nullptr;
    const auto* MeshComponent = GetMesh();
    const auto* MeshAsset = MeshComponent ? MeshComponent->GetSkeletalMeshAsset() : nullptr;
    const auto* Skeleton = MeshAsset ? MeshAsset->GetSkeleton() : nullptr;
    const auto* Animation = Variant->Animation.Get();
    if (!Skeleton || Animation->GetSkeleton() != Skeleton || Animation->HasRootMotion() ||
        Animation->GetAdditiveAnimType() != AAT_None || !FMath::IsFinite(Animation->GetPlayLength()) ||
        Animation->GetPlayLength() <= 0)
        return nullptr;
    return Variant;
}

// 交互组件询问能否显示和使用交谈提示，条件交给玩家的交谈组件判断
bool ADeliveryNpcCharacter::CanTalk(APawn* Interactor, bool bCheckAvailability)
{
    if (!Interactor)
        return false;
    const auto* PC = Cast<APlayerController>(Interactor->GetController());
    const auto* Conversation = PC ? PC->FindComponentByClass<UDeliveryConversationComponent>() : nullptr;
    if (!Conversation || !Conversation->CanTalkTo(this))
        return false;
    // 本机提示与 T 键采用同一个对象，服务器不使用远端玩家的镜头重选目标
    return !PC->IsLocalController() || Conversation->GetAimedNpc() == this;
}

// 交互组件触发交谈时调用，服务器通知玩家打开交谈面板
bool ADeliveryNpcCharacter::OpenConversation(APawn* Interactor)
{
    if (!HasAuthority() || !CanTalk(Interactor, true) || !Interactor->GetController())
        return false;
    if (auto* Conversation = Interactor->GetController()->FindComponentByClass<UDeliveryConversationComponent>())
    {
        Conversation->ClientOpenConversation(this);
        return true;
    }
    return false;
}

// 服务器：把一条见闻写成 Agent 事件，有效期 30 秒
void ADeliveryNpcCharacter::ObserveFact(FName Kind, AActor* Subject, const FString& Text, EDeliveryNpcEvidenceSource Source)
{
    if (!HasAuthority())
        return;
    FDeliveryNpcObservation Event;
    Event.Kind = Kind;
    Event.Subject = Subject;
    Event.Description = Text;
    Event.Source = Source;
    Event.Lifetime = 45;
    Event.bHasObservedLocation = Subject && Source != EDeliveryNpcEvidenceSource::ToldBy;
    if (Event.bHasObservedLocation)
        Event.ObservedLocation = Subject->GetActorLocation();
    if (Agent->ObserveEvent(Event))
    {
        // 新的现场事件可以开启下一次争执，自己的动作结束不重置接话轮数
        if (Source != EDeliveryNpcEvidenceSource::ActionResult)
            ConversationDepth = 0;
        WakeDecision();
    }
}

// 现场听见的话，转交 HearSpeechInternal
bool ADeliveryNpcCharacter::HearSpeech(AActor* Speaker, const FString& Text, FGuid TurnId, int32 Depth, AActor* Addressee,
                                       FGuid ReplyToEventId, bool bRequestReply)
{
    return HearSpeechInternal(Speaker, Text, TurnId, Depth, Addressee, ReplyToEventId, bRequestReply, false);
}

// 交谈流程第 8 步，服务器：过滤距离和遮挡，把这句话写成事件，按说话人决定何时请求模型
bool ADeliveryNpcCharacter::HearSpeechInternal(AActor* Speaker, const FString& Text, FGuid TurnId, int32 Depth, AActor* Addressee,
                                               FGuid ReplyToEventId, bool bRequestReply, bool bCompanionVoice)
{
    if (!HasAuthority() || !IsValid(Speaker) || Speaker->GetWorld() != GetWorld() || Depth > 3 ||
        (Addressee && (!IsValid(Addressee) || Addressee->GetWorld() != GetWorld())) || Text.IsEmpty() || Text.Len() > 256 ||
        (!bCompanionVoice && FVector::DistSquared(Speaker->GetActorLocation(), GetActorLocation()) > FMath::Square(900.f)))
        return false;

    // 没有指定对象时视为对自己说；连麦不检查遮挡
    if (!Addressee)
        Addressee = this;
    if (!bCompanionVoice && !DeliveryNpcConversationTrace::HasClearSpeechPath(this, Speaker))
        return false;

    // 写成事件：对别人说的记为旁听，玩家对我说的记为 PlayerSpeech，其他 NPC 对我说的记为 Speech
    const auto* SpeakingPawn = Cast<APawn>(Speaker);
    FDeliveryNpcObservation Event;
    Event.EventId = TurnId;
    Event.ReplyToEventId = ReplyToEventId;
    const bool bPlayerSpeaking = SpeakingPawn && SpeakingPawn->IsPlayerControlled();
    Event.Kind = Addressee != this ? TEXT("OverheardSpeech") : bPlayerSpeaking ? TEXT("PlayerSpeech") : TEXT("Speech");
    Event.Subject = Speaker;
    Event.Addressee = Addressee;
    Event.Source = EDeliveryNpcEvidenceSource::Heard;
    Event.Description = Text;
    Event.Lifetime = 45;
    Event.bNeedsReply = bRequestReply;
    if (!Agent->ObserveEvent(Event))
        return false;

    // 只需记住的话到此为止
    if (!bRequestReply)
        return true;
    // 旁人先记住玩家问了什么，等听到回答后再决定是否插话
    if (bPlayerSpeaking && Addressee != this)
        return true;

    // 玩家对我说：播放“听见了”的短反应，立即请求模型
    if (Event.Kind == TEXT("PlayerSpeech"))
    {
        DecisionRetryCount = 0;
        bDecisionRetryScheduled = false;
        if (!Agent->BeginListening(Speaker, TurnId))
            return false;
        ConversationDepth = 0;
        if (!Agent->RequestDecision())
            MarkDecisionNeeded();
        return true;
    }

    // 其他 NPC 对我说：记下接话轮数，让下一次请求尽快发出
    ConversationDepth = Depth;
    WakeDecision();
    // 正在回复时收到同伴的新话，保存后安排下一次回答
    if (Agent->IsDecisionPending())
        Agent->RequestDecision();
    return true;
}

// 判断某个动作当前能否执行，生成候选和校验建议时都会调用
bool ADeliveryNpcCharacter::CanExecute(EDeliveryNpcAction Action, AActor* Target) const
{
    if (Action == EDeliveryNpcAction::GetUp)
        return !Target && Combat->CanStartDeliberateRecovery();

    // 倒地时都不行；受击后仰期间只允许交战、逃跑、等待和注视
    if (IsKnockedDown() ||
        (Combat->IsReactingToHit() && Action != EDeliveryNpcAction::Fight && Action != EDeliveryNpcAction::Flee &&
         Action != EDeliveryNpcAction::Wait && Action != EDeliveryNpcAction::LookAt))
        return false;

    // 正在拽车时只能松手、等待或注视
    if (Action == EDeliveryNpcAction::ReleaseVehicle)
        return GetHeldVehicle() != nullptr;
    if (GetHeldVehicle())
        return Action == EDeliveryNpcAction::Wait || Action == EDeliveryNpcAction::LookAt;

    // 移动类动作要能找到可走到的落点，攻击类动作交给战斗组件判断
    if (Action == EDeliveryNpcAction::Pace || Action == EDeliveryNpcAction::MoveAside)
    {
        FVector Destination;
        return Action == EDeliveryNpcAction::Pace ? FindPaceDestination(Destination)
                                                  : FindSideStepDestination(Target, Destination);
    }
    if (Action == EDeliveryNpcAction::StrikeOnce)
        return Cast<AAIController>(GetController()) && Combat->CanApproachStrike(Target);
    if (Action == EDeliveryNpcAction::Fight)
    {
        const bool bChangingFight =
            Agent->CanUpdateRunningAction() && Agent->GetPublicState().Action == EDeliveryNpcAction::Fight;
        return Cast<AAIController>(GetController()) &&
               (bChangingFight ? Combat->CanChooseFightTarget(Target) : Combat->CanBeginFight(Target));
    }
    if (Action == EDeliveryNpcAction::Flee)
    {
        FVector Destination;
        return FindFleeDestination(Target, FMath::Clamp(FleeTravelLimit, 200.f, 1200.f), Destination);
    }
    if (Action == EDeliveryNpcAction::HoldVehicle)
    {
        const auto* Bike = Cast<ADeliveryMotorbike>(Target);
        return Bike && Bike->ResidentHold && Bike->ResidentHold->CanHold(this);
    }
    if (Action == EDeliveryNpcAction::MoveTo)
        return IsValid(Target) && Target != this && !HasReachedMoveTarget(Target) && CanReachTarget(Target);
    return Action == EDeliveryNpcAction::Wait || Action == EDeliveryNpcAction::LookAt || Action == EDeliveryNpcAction::LieDown;
}

// 普通居民不区分请求者，同伴子类在这里检查主人权限
bool ADeliveryNpcCharacter::CanExecuteForRequester(EDeliveryNpcAction Action, AActor* Target, APlayerState* Requester) const
{
    return CanExecute(Action, Target);
}

// 用导航的到达判断检查是否已走到目标旁边
bool ADeliveryNpcCharacter::HasReachedMoveTarget(AActor* Target) const
{
    const auto* AI = Cast<AAIController>(GetController());
    const auto* PathFollowing = AI ? AI->GetPathFollowingComponent() : nullptr;
    if (!IsValid(Target) || !PathFollowing)
        return false;
    FAIMoveRequest Request(Target);
    Request.SetAcceptanceRadius(MoveAcceptanceRadius);
    Request.SetReachTestIncludesAgentRadius(true);
    Request.SetReachTestIncludesGoalRadius(true);
    return PathFollowing->HasReached(Request);
}

// 检查导航能否完整走到目标，查询本身不移动角色
bool ADeliveryNpcCharacter::CanReachTarget(AActor* Target) const
{
    const auto* AI = Cast<AAIController>(GetController());
    auto* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!IsValid(Target) || Target == this || Target->GetWorld() != GetWorld())
        return false;
    const bool bDiagnose = CVarNpcNavigationDiagnostics.GetValueOnGameThread() != 0;
    if (!AI || !Nav)
    {
        if (bDiagnose)
            UE_LOG(LogTemp, Display,
                   TEXT("NpcNavigation npc=%s target=%s stage=Unavailable controller=%d navigation=%d world=%s"), *GetName(),
                   *GetNameSafe(Target), AI != nullptr, Nav != nullptr, *GetNameSafe(GetWorld()));
        return false;
    }
    // 与实际 MoveTo 使用相同的角色位置、目标偏移和导航过滤器
    FAIMoveRequest Request(Target);
    Request.SetNavigationFilter(AI->GetDefaultNavigationFilterClass());
    Request.SetAllowPartialPath(false);
    Request.SetRequireNavigableEndLocation(true);
    FPathFindingQuery Query;
    if (!AI->BuildPathfindingQuery(Request, Query))
    {
        if (bDiagnose)
            UE_LOG(LogTemp, Display, TEXT("NpcNavigation npc=%s target=%s stage=BuildQueryFailed start=%s goal=%s"), *GetName(),
                   *GetNameSafe(Target), *GetNavAgentLocation().ToString(), *Target->GetActorLocation().ToString());
        return false;
    }
    const auto Result = Nav->FindPathSync(Query);
    if (bDiagnose)
        UE_LOG(
            LogTemp, Display,
            TEXT(
                "NpcNavigation npc=%s target=%s stage=FindPath navData=%s start=%s end=%s result=%d valid=%d partial=%d points=%d"),
            *GetName(), *GetNameSafe(Target), *GetNameSafe(Query.NavData.Get()), *Query.StartLocation.ToString(),
            *Query.EndLocation.ToString(), int32(Result.Result), Result.Path.IsValid() && Result.Path->IsValid(),
            Result.Path.IsValid() && Result.Path->IsPartial(), Result.Path.IsValid() ? Result.Path->GetPathPoints().Num() : 0);
    return Result.IsSuccessful() && Result.Path.IsValid() && Result.Path->IsValid() && !Result.Path->IsPartial();
}

// 在导航网格上找一个可走到、胶囊能站下、路程不超过上限的落点
bool ADeliveryNpcCharacter::FindNearbyWalkDestination(const FVector& Desired, float MaxTravel, FVector& OutLocation) const
{
    auto* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!Nav || !Cast<AAIController>(GetController()) || !GetCharacterMovement()->IsMovingOnGround())
        return false;
    const FVector Start = GetNavAgentLocation();
    const auto& Properties = GetNavAgentPropertiesRef();
    const auto* NavData = Nav->GetNavDataForProps(Properties, Start);
    FNavLocation Projected;
    if (!NavData || !Nav->ProjectPointToNavigation(Desired, Projected, FVector(30, 30, 80), NavData) ||
        FVector::Dist2D(Start, Projected.Location) < 35 || FMath::Abs(Start.Z - Projected.Location.Z) > 60)
        return false;
    const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    const auto Shape = FCollisionShape::MakeCapsule(GetCapsuleComponent()->GetScaledCapsuleRadius(), HalfHeight);
    const FVector EndCenter = Projected.Location + FVector(0, 0, HalfHeight + 2);
    FCollisionQueryParams Params(SCENE_QUERY_STAT(NpcNearbyWalk), false, this);
    if (GetWorld()->OverlapBlockingTestByChannel(EndCenter, FQuat::Identity, ECC_Pawn, Shape, Params))
        return false;
    FHitResult Hit;
    // 查询胶囊抬高两厘米，避免把当前脚下的接触地面当成前方障碍
    if (GetWorld()->SweepSingleByChannel(Hit, GetActorLocation() + FVector(0, 0, 2), EndCenter, FQuat::Identity, ECC_Pawn, Shape,
                                         Params))
        return false;
    FPathFindingQuery Query(this, *NavData, Start, Projected.Location);
    Query.SetAllowPartialPaths(false);
    const auto Path = Nav->FindPathSync(Properties, Query);
    if (!Path.IsSuccessful() || !Path.Path.IsValid() || Path.Path->IsPartial() || Path.Path->GetLength() > MaxTravel)
        return false;
    OutLocation = Projected.Location;
    return true;
}

// 为踱步找一个附近落点
bool ADeliveryNpcCharacter::FindPaceDestination(FVector& OutLocation) const
{
    if (!HasAuthority() || IsKnockedDown() || GetHeldVehicle())
        return false;
    const float Distance = FMath::Clamp(PaceDistance, 40.f, 140.f);
    const float Budget = FMath::Min(Distance * 1.5f, GetPaceTravelBudget());
    if (Budget < 40.f)
        return false;
    const float Step = FMath::Min(Distance, Budget);
    for (const FVector& Direction :
         {GetActorRightVector(), -GetActorRightVector(), GetActorForwardVector(), -GetActorForwardVector()})
        if (FindNearbyWalkDestination(GetNavAgentLocation() + Direction * Step, Budget, OutLocation))
            return true;
    return false;
}

// 为让开找一个对方侧面的落点
bool ADeliveryNpcCharacter::FindSideStepDestination(AActor* Target, FVector& OutLocation) const
{
    const auto* Person = Cast<APawn>(Target);
    if (!HasAuthority() || IsKnockedDown() || GetHeldVehicle() || !IsValid(Person) || Person == this ||
        Person->GetWorld() != GetWorld() ||
        FVector::DistSquared(GetActorLocation(), Person->GetActorLocation()) > FMath::Square(600.f))
        return false;
    FVector Toward = (Person->GetActorLocation() - GetActorLocation()).GetSafeNormal2D();
    if (Toward.IsNearlyZero())
        Toward = GetActorForwardVector();
    const FVector Side = FVector::CrossProduct(FVector::UpVector, Toward).GetSafeNormal();
    const float Distance = FMath::Clamp(SideStepDistance, 80.f, 250.f);
    for (const float Sign : {1.f, -1.f})
        if (FindNearbyWalkDestination(GetNavAgentLocation() + Side * Sign * Distance, Distance * 1.5f, OutLocation))
            return true;
    return false;
}

// 为逃跑找一个远离威胁的落点，要比现在离得更远
bool ADeliveryNpcCharacter::FindFleeDestination(AActor* Threat, float RemainingTravel, FVector& OutLocation) const
{
    const auto* Person = Cast<APawn>(Threat);
    if (!HasAuthority() || IsKnockedDown() || GetHeldVehicle() || !IsValid(Person) || Person == this ||
        Person->GetWorld() != GetWorld() || !FMath::IsFinite(RemainingTravel) || RemainingTravel < 60)
        return false;
    const FVector Start = GetNavAgentLocation();
    const FVector ThreatPosition = Person->GetActorLocation();
    const float CurrentDistance = FVector::Dist2D(Start, ThreatPosition);
    if (CurrentDistance >= FMath::Clamp(FleeSafeDistance, 200.f, 900.f))
        return false;
    FVector Away = (Start - ThreatPosition).GetSafeNormal2D();
    if (Away.IsNearlyZero())
        Away = -GetActorForwardVector();
    const float Step = FMath::Min(FMath::Clamp(FleeStepDistance, 80.f, 350.f), RemainingTravel / 1.5f);
    // 正后方被挡住时尝试两侧，落点必须比当前位置离对方更远
    for (const float Angle : {0.f, 40.f, -40.f, 75.f, -75.f})
    {
        const FVector Desired = Start + Away.RotateAngleAxis(Angle, FVector::UpVector) * Step;
        FVector Destination;
        if (FindNearbyWalkDestination(Desired, FMath::Min(RemainingTravel, Step * 1.5f), Destination) &&
            FVector::Dist2D(Destination, ThreatPosition) >= CurrentDistance + 35)
        {
            OutLocation = Destination;
            return true;
        }
    }
    return false;
}

// 空闲、聊天或等回复时可以挪步调整站位
bool ADeliveryNpcCharacter::CanAdjustStandingPosition() const
{
    if (!HasAuthority() || IsKnockedDown() || GetHeldVehicle() || Combat->IsStriking() || Combat->IsReactingToHit() ||
        !GetCharacterMovement()->IsMovingOnGround() || GetCharacterMovement()->MaxWalkSpeed <= 0)
        return false;
    const auto& State = Agent->GetPublicState();
    return !Agent->HasActiveAction() || State.Action == EDeliveryNpcAction::Wait || State.Action == EDeliveryNpcAction::LookAt;
}

// 与邻居太近时，找一个拉开距离的站位
bool ADeliveryNpcCharacter::FindStandingSpace(AActor* Neighbor, float BodyGap, FVector& OutLocation) const
{
    if (!CanAdjustStandingPosition() || !IsValid(Neighbor) || Neighbor == this || Neighbor->GetWorld() != GetWorld())
        return false;
    const FVector Start = GetNavAgentLocation();
    const FVector Other = Neighbor->GetActorLocation();
    const float Wanted = GetSimpleCollisionRadius() + Neighbor->GetSimpleCollisionRadius() + FMath::Clamp(BodyGap, 40.f, 200.f);
    const float Distance = FVector::Dist2D(Start, Other);
    if (Distance >= Wanted)
        return false;
    FVector Away = (Start - Other).GetSafeNormal2D();
    if (Away.IsNearlyZero())
        Away = -GetActorForwardVector();
    // 先向外挪，后方受阻再找斜侧空位；沿用导航与胶囊检查，不直接改变位置
    for (const float Angle : {0.f, 35.f, -35.f, 70.f, -70.f, 90.f, -90.f})
    {
        const float Along = Distance * FMath::Cos(FMath::DegreesToRadians(Angle));
        const float Step = FMath::Clamp(
            FMath::Sqrt(FMath::Max(0.f, Wanted * Wanted - Distance * Distance + Along * Along)) - Along + 12.f, 45.f, 280.f);
        FVector Destination;
        if (FindNearbyWalkDestination(Start + Away.RotateAngleAxis(Angle, FVector::UpVector) * Step, Step * 1.5f, Destination) &&
            FVector::Dist2D(Destination, Other) >= Wanted - 5.f)
        {
            OutLocation = Destination;
            return true;
        }
    }
    return false;
}

// 交谈流程第 16 步，服务器：行为树开始动作时调用，按动作类型启动移动、战斗或对白
bool ADeliveryNpcCharacter::ExecuteAction(const FDeliveryNpcPublicState& State)
{
    const bool bAllowed = State.Origin == EDeliveryNpcActionOrigin::GameplayRule
                              ? CanExecuteGameplayRule(State.Action, State.Target, State.RuleRevision)
                              : CanExecuteForRequester(State.Action, State.Target, State.Requester);
    if (!HasAuthority() || ExecutingAction.IsValid() || !bAllowed)
        return false;

    // 移动类和攻击类动作切换行走速度，结束时在 StopAction 中恢复
    ExecutingAction = State.ActionId;
    if (State.Action == EDeliveryNpcAction::Pace || State.Action == EDeliveryNpcAction::MoveAside ||
        State.Action == EDeliveryNpcAction::MoveTo || State.Action == EDeliveryNpcAction::StrikeOnce ||
        State.Action == EDeliveryNpcAction::Fight || State.Action == EDeliveryNpcAction::Flee)
    {
        PreviousActionWalkSpeed = GetCharacterMovement()->MaxWalkSpeed;
        GetCharacterMovement()->MaxWalkSpeed =
            State.Action == EDeliveryNpcAction::Pace ? PaceSpeed
            : (State.Action == EDeliveryNpcAction::StrikeOnce || State.Action == EDeliveryNpcAction::Fight ||
               State.Action == EDeliveryNpcAction::Flee)
                ? AngryApproachSpeed
                : WalkSpeed;
    }

    // 按动作类型启动对应功能
    bool bStarted = true;
    // 攻击目标还在远处时，由行为树先靠近；到达前不会启动命中判定
    if (State.Action == EDeliveryNpcAction::StrikeOnce && Combat->CanStrike(State.Target))
        bStarted = RequestStrike(State.Target, State.ActionId);
    else if (State.Action == EDeliveryNpcAction::Fight)
    {
        // 规则动作验证过目标和代次后才能使用该角色授权的接近距离
        const float ApproachRange =
            State.Origin == EDeliveryNpcActionOrigin::GameplayRule ? GetGameplayRuleFightApproachRange() : 0.f;
        bStarted = Combat->BeginFight(State.Target, State.ActionId, State.Duration,
                                      State.Origin == EDeliveryNpcActionOrigin::Model, ApproachRange);
    }
    else if (State.Action == EDeliveryNpcAction::HoldVehicle)
        bStarted = CastChecked<ADeliveryMotorbike>(State.Target)->ResidentHold->TryHold(this, State.ActionId);
    else if (State.Action == EDeliveryNpcAction::ReleaseVehicle)
    {
        if (GetHeldVehicle())
            GetHeldVehicle()->Release(TEXT("VoluntaryRelease"));
    }
    else if (State.Action == EDeliveryNpcAction::LieDown)
        bStarted = Combat->SetDeliberateProne(true);
    else if (State.Action == EDeliveryNpcAction::GetUp)
        bStarted = Combat->StartDeliberateRecovery();

    // 非流式回复在这里播放台词，流式回复由表现组件按句播放
    if (bStarted && !State.bStreamSpeech && !State.Speech.IsEmpty() &&
        !Presentation->PublishSpeech(State.Speech, State.Emotion, Agent->GetActionConversationDepth(), State.ReplyToEventId,
                                     State.GetReplyId()))
    {
        UE_LOG(LogTemp, Warning, TEXT("ResidentSpeech rejected: npc=%s action=%s"), *GetName(), *State.ActionId.ToString());
        return false;
    }
    return bStarted;
}

// 停止指定编号的动作：停止寻路、恢复速度、取消出拳和交战
void ADeliveryNpcCharacter::StopAction(FGuid Id)
{
    if (!Id.IsValid() || Id != ExecutingAction)
        return;
    if (auto* AI = Cast<AAIController>(GetController()))
        AI->StopMovement();
    if (PreviousActionWalkSpeed >= 0)
    {
        GetCharacterMovement()->MaxWalkSpeed = IsKnockedDown() ? 0.f : PreviousActionWalkSpeed;
        PreviousActionWalkSpeed = -1;
    }
    CancelStrikeAbility(Id);
    Combat->EndFight(Id, !Agent->IsReplacingFight());
    Combat->CancelStrike();
    if (GetHeldVehicle())
        GetHeldVehicle()->Release(TEXT("ActionEnded"));
    ExecutingAction.Invalidate();
}

// 拽住车时关闭行走，松手后恢复
void ADeliveryNpcCharacter::SetHeldVehicle(UDeliveryVehicleHoldComponent* Hold)
{
    HeldVehicle = Hold;
    GetCharacterMovement()->SetMovementMode(Hold ? MOVE_None : MOVE_Walking);
}

// 是否倒地，查询战斗组件
bool ADeliveryNpcCharacter::IsKnockedDown() const
{
    return Combat && Combat->IsDown();
}

// 无来源的撞击，转交 ReceiveImpactFrom
void ADeliveryNpcCharacter::ReceiveImpact(const FVector& Impulse, const FVector& Point)
{
    ReceiveImpactFrom(Impulse, Point, nullptr);
}

// 服务器：被撞时停止对白，交给战斗组件处理击退；刚倒地时取消当前安排
void ADeliveryNpcCharacter::ReceiveImpactFrom(const FVector& Impulse, const FVector& Point, AActor* Source)
{
    if (!HasAuthority())
        return;
    const bool bWasDown = Combat->IsDown();
    Presentation->CancelSpeech();
    Combat->ReceiveImpact(Impulse, Source);
    if (!bWasDown && Combat->IsDown())
        Presentation->PlayReaction(EDeliveryNpcVoiceReaction::Hit, FGuid::NewGuid());
}

// 无来源的鸣笛，转交 ReceiveHornFrom
void ADeliveryNpcCharacter::ReceiveHorn(float Multiplier)
{
    ReceiveHornFrom(Multiplier, nullptr, nullptr);
}

// 服务器：被鸣笛时受惊转头，每 1.5 秒最多记一次鸣笛事件，并记下驾驶的玩家
void ADeliveryNpcCharacter::ReceiveHornFrom(float Multiplier, AActor* Vehicle, AActor* Driver)
{
    if (!HasAuthority())
        return;
    if (Look)
        Look->TryStartle(Vehicle);
    const double Now = GetWorld()->GetTimeSeconds();
    if (Now - LastHorn > 2)
        HornPressure = 0;
    ++HornPressure;
    LastHorn = Now;
    if (Now - LastHornObservation < 1.5 || Agent->IsDecisionPending())
        return;
    LastHornObservation = Now;
    ObserveFact(TEXT("Horn"), Vehicle, FString::Printf(TEXT("附近车辆正在鸣笛，近期累计感知 %d 次鸣笛扫描"), HornPressure),
                EDeliveryNpcEvidenceSource::Heard);
    if (Driver)
        ObserveFact(TEXT("HornDriver"), Driver, TEXT("鸣笛时这名玩家正在驾驶该车辆"), EDeliveryNpcEvidenceSource::Seen);
}

// 服务器每帧：刷新黑板，动作结束后标记需要决策，条件满足时发出下一次模型请求
void ADeliveryNpcCharacter::Tick(float Dt)
{
    Super::Tick(Dt);
    ConversationInteractable->bShowPrompt = !IsKnockedDown() || Combat->IsDeliberatelyProne();
    if (!HasAuthority())
        return;
    if (auto* AI = Cast<ADeliveryNpcAIController>(GetController()))
        AI->RefreshBlackboard();

    // 动作结束后，除成功的等待和注视外都要重新决策
    const auto State = Agent->GetPublicState();
    if (DeliveryNpc::IsTerminal(State.Status) && State.ActionId != LastFinishedAction)
    {
        LastFinishedAction = State.ActionId;
        if (State.Status != EDeliveryNpcActionStatus::Succeeded ||
            (State.Action != EDeliveryNpcAction::Wait && State.Action != EDeliveryNpcAction::LookAt))
            MarkDecisionNeeded();
    }

    // 发请求的条件：有新情况、没在说话、没倒地、没在受击后仰、手上没有动作、上一个请求已结束
    const bool bCanDecide = !IsKnockedDown() || (Combat->IsDeliberatelyProne() && Agent->IsWaitingToReplyToPlayer());
    if (bDecisionNeeded &&
        (Agent->IsWaitingToReplyToPlayer() || Agent->HasPendingNpcMessage() || !Presentation->HasPendingSpeech()) && bCanDecide &&
        !Combat->IsReactingToHit() && (!Agent->HasActiveAction() || Agent->IsWaitingToReplyToPlayer()) &&
        !Agent->IsDecisionPending() && GetWorld()->GetTimeSeconds() >= NextDecision)
    {
        NextDecision = GetWorld()->GetTimeSeconds() + 4;
        Agent->RequestDecision();
    }
}

// 停止动作，移除出拳技能，松开车辆，解除所有绑定
void ADeliveryNpcCharacter::EndPlay(const EEndPlayReason::Type Reason)
{
    StopAction(ExecutingAction);
    if (AbilitySystem)
    {
        AbilitySystem->CancelAllAbilities();
        if (HasAuthority() && StrikeAbilityHandle.IsValid())
            AbilitySystem->ClearAbility(StrikeAbilityHandle);
        AbilitySystem->ClearActorInfo();
    }
    if (GetHeldVehicle())
        GetHeldVehicle()->Release(TEXT("ResidentEndPlay"));
    Agent->OnDecisionFinished.RemoveDynamic(this, &ADeliveryNpcCharacter::OnDecisionFinished);
    ConversationInteractable->CanUse.Unbind();
    ConversationInteractable->Use.Unbind();
    Super::EndPlay(Reason);
}

// 标记需要决策，序号用来识别请求发出后又到达的新情况
void ADeliveryNpcCharacter::MarkDecisionNeeded()
{
    ++DecisionWakeSerial;
    bDecisionNeeded = true;
}

// 请求入队后，若期间没有新情况就清除标记
void ADeliveryNpcCharacter::ConfirmDecisionRequestQueued(uint64 WakeSerial)
{
    if (WakeSerial == DecisionWakeSerial)
        bDecisionNeeded = false;
    bDecisionRetryScheduled = false;
}

// 清除决策标记和重试计数
void ADeliveryNpcCharacter::ResetDecisionScheduling()
{
    bDecisionNeeded = false;
    bDecisionRetryScheduled = false;
    DecisionRetryCount = 0;
}

// 让下一次请求在 0.25 秒内发出
void ADeliveryNpcCharacter::WakeDecision()
{
    MarkDecisionNeeded();
    if (Agent->IsWaitingToReplyToPlayer() && bDecisionRetryScheduled)
        return;
    // 旁人插话会补充下一次请求，同一句玩家消息仍沿用已有的重试次数
    if (!Agent->IsWaitingToReplyToPlayer())
    {
        DecisionRetryCount = 0;
        bDecisionRetryScheduled = false;
    }
    NextDecision = FMath::Min(NextDecision, GetWorld()->GetTimeSeconds() + .25);
}

// 决策结束：成功时清零重试计数；传输失败、超时、过期等情况最多重试两次
void ADeliveryNpcCharacter::OnDecisionFinished(FName Reason)
{
    if (!HasAuthority())
        return;
    bDecisionRetryScheduled = false;
    if (Reason.IsNone())
    {
        DecisionRetryCount = 0;
        return;
    }

    // 重试间隔依次为 2 秒、4 秒，同伴委托变化时为 0.25 秒
    // 原问题最多刷新两次，已知委托刚取得实物或结束时沿用现场事件的短暂合并间隔
    const bool bErrandChanged = Reason == TEXT("CompanionErrandEnded") || Reason == TEXT("CompanionErrandAdvanced");
    if ((Reason == TEXT("TransportFailure") || Reason == TEXT("RequestTimeout") || Reason == TEXT("StaleProposal") ||
         Reason == TEXT("MissingPlayerReply") || bErrandChanged) &&
        DecisionRetryCount < 2)
    {
        ++DecisionRetryCount;
        bDecisionNeeded = true;
        bDecisionRetryScheduled = true;
        NextDecision = GetWorld()->GetTimeSeconds() + (bErrandChanged ? .25 : 2. * DecisionRetryCount);
    }
}
