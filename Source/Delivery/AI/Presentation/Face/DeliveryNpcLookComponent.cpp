#include "AI/Presentation/Face/DeliveryNpcLookComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Combat/Types/DeliveryImpactReceiver.h"
#include "Character/DeliveryCharacter.h"
#include "Components/CapsuleComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "NPC/Pedestrian/DeliveryPedestrian.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Vehicle/Motorbike/DeliveryMotorbike.h"

// ── 关注 ──
// 服务器每 CheckInterval 秒（默认 0.2 秒）调用一次 TickComponent，查看附近的每位玩家
// 玩家刚走进 NoticeDistance 且在 NPC 前方或侧面，或者慢慢蹭到 NPC 身体，就调用 StartLook 看过去
// 看谁、看到什么时候写在 LookState 里，同步给所有客户端；动画蓝图和表情代码调用 GetLookTarget 读取
// NPC 在做别的事（CanLook 返回 false）时不看人，忙完后不补看忙碌期间路过的人
namespace
{
/// @brief 玩家身体的速度：步行主角取布娃娃骨盆的物理速度；取不到时用 Actor 速度，再不行用两次检查之间的位移算出的速度
FVector PlayerBodyVelocity(const AActor* Player,const FVector& SampledVelocity)
{
    if (const auto* Character=Cast<ADeliveryCharacter>(Player))
        if (const auto* Ragdoll=Character->GetActiveRagdoll()) return Ragdoll->GetPhysicalPelvisVelocity();
    const FVector Velocity=Player->GetVelocity();
    return Velocity.IsNearlyZero()?SampledVelocity:Velocity;
}
}

/// @brief 组件同步 LookState 给客户端，Tick 间隔默认 0.2 秒
UDeliveryNpcLookComponent::UDeliveryNpcLookComponent()
{
    PrimaryComponentTick.bCanEverTick=true;
    PrimaryComponentTick.TickInterval=.2f;
    SetIsReplicatedByDefault(true);
}

/// @brief 只有服务器打开 Tick，间隔取 CheckInterval（0.1 到 1 秒）
void UDeliveryNpcLookComponent::BeginPlay()
{
    Super::BeginPlay();
    SetComponentTickEnabled(GetOwner()->HasAuthority());
    SetComponentTickInterval(FMath::Clamp(CheckInterval,.1f,1.f));
}

/// @brief 服务器的世界时间；客户端也用它比较 LookState.EndTime，两边的时间对得上
double UDeliveryNpcLookComponent::ServerTime() const
{
    const UWorld* World=GetWorld();
    if (!World) return 0;
    if (const auto* State=World->GetGameState()) return State->GetServerWorldTimeSeconds();
    return World->GetTimeSeconds();
}

/// @brief 判断 NPC 现在有没有空看人：被打倒、起身、出拳、受击后仰、拽车、正在执行模型选的动作（Wait 除外）、倾听、说台词时都返回 false
/// 二狗在跑腿、摆家具、拿着东西或在走路时也返回 false；路人正在爬起来时返回 false
bool UDeliveryNpcLookComponent::CanLook() const
{
    if (!IsValid(GetOwner()) || GetOwner()->IsActorBeingDestroyed() || GetOwner()->IsHidden()) return false;
    if (const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        if (!Npc->Agent || !Npc->Combat || !Npc->Presentation || Npc->Combat->IsRecovering() ||
            Npc->Combat->IsStriking() || Npc->Combat->IsReactingToHit() || Npc->GetHeldVehicle()) return false;
        const auto State=Npc->Agent->GetPublicState();

        // 模型安排的动作、倾听和说台词优先；这些时候不看路人，结束后也不补看
        if ((Npc->Agent->HasActiveAction() && State.Action!=EDeliveryNpcAction::Wait) ||
            Npc->Agent->IsListening() ||
            Npc->Presentation->HasPendingSpeech() || Npc->Presentation->GetDisplayedSpeech().bActive) return false;
        if (const auto* Companion=Npc->FindComponentByClass<UDeliveryCompanionComponent>())
        {
            if (Companion->HasActiveErrand() || Companion->HasActiveHomeArrangement() || Companion->IsUsingErrandVehicle() ||
                Companion->GetCarriedItem() || Companion->GetCarriedTaskPackage() || Companion->GetHomeState().CarriedProp ||
                Companion->GetMode()==EDeliveryCompanionMode::GoHome) return false;

            // 二狗跟着主人走或者走回等候点时（速度超过 20 cm/s）看路；站定且空手时才看路人
            if (Companion->GetMode()!=EDeliveryCompanionMode::Unrecruited && Npc->GetVelocity().SizeSquared2D()>FMath::Square(20.f)) return false;
        }
    }
    if (const auto* Receiver=Cast<IDeliveryImpactReceiver>(GetOwner()); Receiver && Receiver->IsKnockedDown()) return false;
    if (const auto* Walker=Cast<ADeliveryPedestrian>(GetOwner()); Walker && Walker->IsStandingUp()) return false;
    return true;
}

/// @brief 返回 LookState 里正在看的人；已经过了 EndTime、目标没了、走出距离（受惊时 StartleDistance，否则 ReleaseDistance）或 CanLook 为 false 时返回 nullptr
AActor* UDeliveryNpcLookComponent::GetLookTarget() const
{
    AActor* Target=LookState.Target.Get();
    const float Limit=LookState.bStartled?FMath::Clamp(StartleDistance,50.f,600.f):FMath::Max(NoticeDistance,ReleaseDistance);
    if (!CanLook() || !IsValid(Target) || Target->IsActorBeingDestroyed() || Target->IsHidden() ||
        Target->GetWorld()!=GetWorld() || ServerTime()>=LookState.EndTime ||
        !FMath::IsFinite(Limit) || FVector::DistSquared(GetOwner()->GetActorLocation(),Target->GetActorLocation())>FMath::Square(Limit)) return nullptr;
    return Target;
}

/// @brief 正在看的人是因为蹭到才看的
bool UDeliveryNpcLookComponent::IsAfterBrush() const { return GetLookTarget() && LookState.bAfterBrush; }

/// @brief 正在被喇叭吓着
bool UDeliveryNpcLookComponent::IsStartled() const { return LookState.bStartled && GetLookTarget(); }

// ── 受惊 ──
// 玩家在 StartleDistance 内按喇叭时，角色调用 TryStartle，NPC 看向喇叭声的来源 StartleDuration 秒，表情代码换上 StartleExpression

/// @brief 服务器：被喇叭吓到时看向 Source；距离太远、两次受惊间隔不到 StartleCooldown、视线被挡住时返回 false
/// 受惊结束前不会被普通路人抢走视线：NextAttentionAt 至少推到受惊结束
bool UDeliveryNpcLookComponent::TryStartle(AActor* Source)
{
    if (!GetWorld() || !GetOwner() || !GetOwner()->HasAuthority() || !bStartleAtHorn || !CanLook() ||
        !IsValid(Source) || Source==GetOwner() || Source->IsActorBeingDestroyed() || Source->IsHidden() ||
        Source->GetWorld()!=GetWorld() || !FMath::IsFinite(StartleDistance) || !FMath::IsFinite(StartleDuration) ||
        !FMath::IsFinite(StartleCooldown)) return false;
    const double Now=ServerTime();
    if (Now<NextStartleAt || FVector::DistSquared(GetOwner()->GetActorLocation(),Source->GetActorLocation())>
        FMath::Square(FMath::Clamp(StartleDistance,50.f,600.f))) return false;
    const auto* Capsule=GetOwner()->FindComponentByClass<UCapsuleComponent>();
    const FVector Eyes=Capsule?Capsule->GetComponentLocation()+FVector(0,0,Capsule->GetScaledCapsuleHalfHeight()*.7f):GetOwner()->GetActorLocation();
    FHitResult Hit;
    FCollisionQueryParams Sight(SCENE_QUERY_STAT(NpcStartleSight),false,GetOwner());
    if (GetWorld()->LineTraceSingleByChannel(Hit,Eyes,GetLookPoint(Source),ECC_Visibility,Sight) && Hit.GetActor()!=Source) return false;
    LookState.Target=Source;
    LookState.EndTime=Now+FMath::Clamp(StartleDuration,.2f,2.f);
    LookState.bAfterBrush=false;
    LookState.bStartled=true;
    NextStartleAt=Now+FMath::Clamp(StartleCooldown,1.f,60.f);
    NextAttentionAt=FMath::Max(NextAttentionAt,LookState.EndTime);
    GetOwner()->ForceNetUpdate();
    return true;
}

/// @brief 看这个人时对准的点：步行主角取布娃娃的头部位置，其他角色取 GetPawnViewLocation，物体取 Actor 位置
FVector UDeliveryNpcLookComponent::GetLookPoint(const AActor* Target)
{
    if (const auto* Player=Cast<ADeliveryCharacter>(Target))
        if (const auto* Ragdoll=Player->GetActiveRagdoll()) return Ragdoll->GetHeadLocation();
    if (const auto* Pawn=Cast<APawn>(Target)) return Pawn->GetPawnViewLocation();
    return Target?Target->GetActorLocation():FVector::ZeroVector;
}

/// @brief 服务器：开始看 Player，看 LookDuration 秒；bAfterBrush 为 true 时看 BrushLookDuration 秒
/// 正在受惊时不换；普通路过时，正在看蹭到的人或者没到 NextAttentionAt 也不换
void UDeliveryNpcLookComponent::StartLook(AActor* Player,bool bAfterBrush,double Now)
{
    if (!GetOwner()->HasAuthority() || !CanLook() || !IsValid(Player) || Player->GetWorld()!=GetWorld()) return;

    // 正在看蹭到的人时，普通路过的人抢不走视线；蹭到可以打断普通的看
    if (IsStartled() || (!bAfterBrush && (IsAfterBrush() || Now<NextAttentionAt))) return;
    LookState.Target=Player;
    LookState.EndTime=Now+FMath::Max(.2f,bAfterBrush?BrushLookDuration:LookDuration);
    LookState.bAfterBrush=bAfterBrush;
    LookState.bStartled=false;
    NextAttentionAt=Now+FMath::Clamp(AttentionInterval,.2f,10.f);
    GetOwner()->ForceNetUpdate();

// 这里只改视线：蹭到不记成被撞，也不请求模型回复；拳打和车撞由命中代码另外记录
}

/// @brief 清空 LookState；服务器上调用 ForceNetUpdate 让客户端马上收到
void UDeliveryNpcLookComponent::ClearLook()
{
    if (!LookState.Target && LookState.EndTime==0) return;
    LookState={};
    if (GetOwner()->HasAuthority()) GetOwner()->ForceNetUpdate();
}

/// @brief 服务器定时调用：CanLook 为 false 时清空注视，并设置 bRefreshVisits，忙完后的第一次检查只记录位置不看人
/// 否则逐位玩家判断是刚走近还是刚蹭到，挑出一位调用 StartLook：蹭到的优先，同类里选最近的
void UDeliveryNpcLookComponent::TickComponent(float DeltaTime,ELevelTick TickType,FActorComponentTickFunction* Function)
{
    Super::TickComponent(DeltaTime,TickType,Function);
    if (!GetOwner()->HasAuthority()) return;
    if (!CanLook())
    {
        ClearLook();
        bRefreshVisits=true;
        bHasOwnerPosition=false;
        return;
    }

    // bSkipOldVisits 为 true 的这一次只更新每位玩家的位置和远近状态，不调用 StartLook
    const bool bSkipOldVisits=bRefreshVisits;
    bRefreshVisits=false;
    if (!GetLookTarget()) ClearLook();
    const double Now=ServerTime();
    const FVector Origin=GetOwner()->GetActorLocation();
    const FVector OwnerVelocity=bHasOwnerPosition && DeltaTime>SMALL_NUMBER?(Origin-LastOwnerPosition)/DeltaTime:GetOwner()->GetVelocity();
    LastOwnerPosition=Origin;
    bHasOwnerPosition=true;
    AActor* BestPlayer=nullptr;
    bool bBestBrush=false;
    float BestDistance=MAX_flt;

    // 有玩家在 180 cm 内才做一次碰撞重叠查询，找出身体（PhysicsBody 或 Pawn）贴到 NPC 胶囊加 BrushMargin 范围内的玩家；骑车的不算
    TArray<AActor*> Players;
    bool bCheckContact=false;
    for (auto It=GetWorld()->GetPlayerControllerIterator();It;++It)
        if (const auto* Controller=It->Get())
            if (APawn* Pawn=Controller->GetPawn(); IsValid(Pawn) && Pawn!=GetOwner())
            {
                Players.Add(Pawn);
                bCheckContact|=FVector::DistSquared(Origin,Pawn->GetActorLocation())<FMath::Square(180.f);
            }
    TSet<AActor*> Touching;
    if (bCheckContact)
        if (const auto* Capsule=GetOwner()->FindComponentByClass<UCapsuleComponent>())
        {
            FCollisionObjectQueryParams Objects;
            Objects.AddObjectTypesToQuery(ECC_PhysicsBody);
            Objects.AddObjectTypesToQuery(ECC_Pawn);
            FCollisionQueryParams Params(SCENE_QUERY_STAT(NpcBrush),false,GetOwner());
            TArray<FOverlapResult> Hits;
            GetWorld()->OverlapMultiByObjectType(Hits,Capsule->GetComponentLocation(),Capsule->GetComponentQuat(),Objects,
                FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius()+BrushMargin,Capsule->GetScaledCapsuleHalfHeight()+BrushMargin),Params);
            for (const auto& Hit:Hits)
                if (AActor* Actor=Hit.GetActor(); Players.Contains(Actor) && !Actor->IsA<ADeliveryMotorbike>()) Touching.Add(Actor);
        }

    // bNearby：在 NoticeDistance 内而且不在 NPC 正后方；bBrush：这次刚贴上、相对速度不超过 BrushMaxSpeed、过了 BrushCooldown
    // bPassing：刚走进范围、过了 PlayerCooldown；两样都不是就跳过
    for (AActor* Player:Players)
    {
        const float Distance=FVector::DistSquared(Origin,Player->GetActorLocation());
        auto& Visit=PlayerVisits.FindOrAdd(Player);
        const FVector SampledVelocity=!bSkipOldVisits && Visit.bHasPosition && DeltaTime>SMALL_NUMBER?(Player->GetActorLocation()-Visit.LastPosition)/DeltaTime:FVector::ZeroVector;
        Visit.LastPosition=Player->GetActorLocation();
        Visit.bHasPosition=true;
        const FVector Direction=(Player->GetActorLocation()-Origin).GetSafeNormal2D();
        const bool bNearby=Distance<=FMath::Square(NoticeDistance) &&
            FVector::DotProduct(GetOwner()->GetActorForwardVector(),Direction)>=-.15f;
        const bool bTouching=Touching.Contains(Player);
        const bool bGentle=(PlayerBodyVelocity(Player,SampledVelocity)-OwnerVelocity).Size2D()<=BrushMaxSpeed;
        const bool bBrush=bTouching && !Visit.bTouching && bGentle && Now>=Visit.NextBrush;
        const bool bPassing=bNoticePassersby && bNearby && !Visit.bNearby && Now>=Visit.NextLook;
        Visit.bNearby=bNearby;
        Visit.bTouching=bTouching;
        if (bSkipOldVisits || (!bBrush && !bPassing)) continue;

        // 从 NPC 眼睛到玩家头部打一条射线，被挡住就不看，并把 bNearby 设回 false，下次走近还能再触发
        FHitResult Hit;
        FCollisionQueryParams Sight(SCENE_QUERY_STAT(NpcLookSight),false,GetOwner());
        const auto* Capsule=GetOwner()->FindComponentByClass<UCapsuleComponent>();
        const FVector Eyes=Capsule?Capsule->GetComponentLocation()+FVector(0,0,Capsule->GetScaledCapsuleHalfHeight()*.7f):Origin;
        if (GetWorld()->LineTraceSingleByChannel(Hit,Eyes,GetLookPoint(Player),ECC_Visibility,Sight) && Hit.GetActor()!=Player)
        { Visit.bNearby=false; continue; }
        if (bBrush) Visit.NextBrush=Now+FMath::Max(1.f,BrushCooldown);
        Visit.NextLook=Now+FMath::Max(1.f,PlayerCooldown);
        if ((bBrush && !bBestBrush) || (bBrush==bBestBrush && Distance<BestDistance))
        { BestPlayer=Player; bBestBrush=bBrush; BestDistance=Distance; }
    }
    if (BestPlayer) StartLook(BestPlayer,bBestBrush,Now);

    // 删掉已经离开世界的玩家的记录
    for (auto It=PlayerVisits.CreateIterator();It;++It)
        if (!It.Key().IsValid() || !Players.Contains(It.Key().Get())) It.RemoveCurrent();
}

/// @brief 登记要同步的属性：LookState
void UDeliveryNpcLookComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(UDeliveryNpcLookComponent,LookState);
}

/// @brief 组件结束时关掉 Tick，清空注视和每位玩家的记录
void UDeliveryNpcLookComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    SetComponentTickEnabled(false);
    LookState={};
    PlayerVisits.Reset();
    bHasOwnerPosition=false;
    bRefreshVisits=false;
    NextAttentionAt=0;
    NextStartleAt=0;
    Super::EndPlay(Reason);
}
