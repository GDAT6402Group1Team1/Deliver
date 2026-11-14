#include "AI/Presentation/DeliveryNpcLookComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Combat/DeliveryImpactReceiver.h"
#include "Character/DeliveryCharacter.h"
#include "Components/CapsuleComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "NPC/DeliveryPedestrian.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Vehicle/DeliveryMotorbike.h"

namespace
{
FVector PlayerBodyVelocity(const AActor* Player,const FVector& SampledVelocity)
{
    if (const auto* Character=Cast<ADeliveryCharacter>(Player))
        if (const auto* Ragdoll=Character->GetActiveRagdoll()) return Ragdoll->GetPhysicalPelvisVelocity();
    const FVector Velocity=Player->GetVelocity();
    return Velocity.IsNearlyZero()?SampledVelocity:Velocity;
}
}

UDeliveryNpcLookComponent::UDeliveryNpcLookComponent()
{
    PrimaryComponentTick.bCanEverTick=true;
    PrimaryComponentTick.TickInterval=.2f;
    SetIsReplicatedByDefault(true);
}

void UDeliveryNpcLookComponent::BeginPlay()
{
    Super::BeginPlay();
    SetComponentTickEnabled(GetOwner()->HasAuthority());
    SetComponentTickInterval(FMath::Clamp(CheckInterval,.1f,1.f));
}

double UDeliveryNpcLookComponent::ServerTime() const
{
    const UWorld* World=GetWorld();
    if (!World) return 0;
    if (const auto* State=World->GetGameState()) return State->GetServerWorldTimeSeconds();
    return World->GetTimeSeconds();
}

bool UDeliveryNpcLookComponent::CanLook() const
{
    if (!IsValid(GetOwner()) || GetOwner()->IsActorBeingDestroyed() || GetOwner()->IsHidden()) return false;
    if (const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        if (!Npc->Agent || !Npc->Combat || !Npc->Presentation || Npc->Combat->IsRecovering() ||
            Npc->Combat->IsStriking() || Npc->Combat->IsReactingToHit() || Npc->GetHeldVehicle()) return false;
        const auto State=Npc->Agent->GetPublicState();
        // 关注路人让位于模型安排、搬运和交谈，不等它们结束后补播旧反应
        if ((Npc->Agent->HasActiveAction() && State.Action!=EDeliveryNpcAction::Wait) ||
            Npc->Agent->IsListening() ||
            Npc->Presentation->HasPendingSpeech() || Npc->Presentation->GetDisplayedSpeech().bActive) return false;
        if (const auto* Companion=Npc->FindComponentByClass<UDeliveryCompanionComponent>())
        {
            if (Companion->HasActiveErrand() || Companion->HasActiveHomeArrangement() || Companion->IsUsingErrandVehicle() ||
                Companion->GetCarriedItem() || Companion->GetCarriedTaskPackage() || Companion->GetHomeState().CarriedProp ||
                Companion->GetMode()==EDeliveryCompanionMode::GoHome) return false;
            // 跟随途中和回到等候点时继续看路；已经站定且空手时可以看一眼路人
            if (Companion->GetMode()!=EDeliveryCompanionMode::Unrecruited && Npc->GetVelocity().SizeSquared2D()>FMath::Square(20.f)) return false;
        }
    }
    if (const auto* Receiver=Cast<IDeliveryImpactReceiver>(GetOwner()); Receiver && Receiver->IsKnockedDown()) return false;
    if (const auto* Walker=Cast<ADeliveryPedestrian>(GetOwner()); Walker && Walker->IsStandingUp()) return false;
    return true;
}

AActor* UDeliveryNpcLookComponent::GetLookTarget() const
{
    AActor* Target=LookState.Target.Get();
    const float Limit=LookState.bStartled?FMath::Clamp(StartleDistance,50.f,600.f):FMath::Max(NoticeDistance,ReleaseDistance);
    if (!CanLook() || !IsValid(Target) || Target->IsActorBeingDestroyed() || Target->IsHidden() ||
        Target->GetWorld()!=GetWorld() || ServerTime()>=LookState.EndTime ||
        !FMath::IsFinite(Limit) || FVector::DistSquared(GetOwner()->GetActorLocation(),Target->GetActorLocation())>FMath::Square(Limit)) return nullptr;
    return Target;
}

bool UDeliveryNpcLookComponent::IsAfterBrush() const { return GetLookTarget() && LookState.bAfterBrush; }
bool UDeliveryNpcLookComponent::IsStartled() const { return LookState.bStartled && GetLookTarget(); }

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

FVector UDeliveryNpcLookComponent::GetLookPoint(const AActor* Target)
{
    if (const auto* Player=Cast<ADeliveryCharacter>(Target))
        if (const auto* Ragdoll=Player->GetActiveRagdoll()) return Ragdoll->GetHeadLocation();
    if (const auto* Pawn=Cast<APawn>(Target)) return Pawn->GetPawnViewLocation();
    return Target?Target->GetActorLocation():FVector::ZeroVector;
}

void UDeliveryNpcLookComponent::StartLook(AActor* Player,bool bAfterBrush,double Now)
{
    if (!GetOwner()->HasAuthority() || !CanLook() || !IsValid(Player) || Player->GetWorld()!=GetWorld()) return;
    // 正在看向刚擦身的玩家时，不被另一个普通经过的人抢走视线
    if (IsStartled() || (!bAfterBrush && (IsAfterBrush() || Now<NextAttentionAt))) return;
    LookState.Target=Player;
    LookState.EndTime=Now+FMath::Max(.2f,bAfterBrush?BrushLookDuration:LookDuration);
    LookState.bAfterBrush=bAfterBrush;
    LookState.bStartled=false;
    NextAttentionAt=Now+FMath::Clamp(AttentionInterval,.2f,10.f);
    GetOwner()->ForceNetUpdate();
    // 轻擦只让人物回头看一眼，身体接近不能说明玩家有意撞人
    // 拳击和车撞由实际命中记录，交谈也不因这次注视重新请求回复
}

void UDeliveryNpcLookComponent::ClearLook()
{
    if (!LookState.Target && LookState.EndTime==0) return;
    LookState={};
    if (GetOwner()->HasAuthority()) GetOwner()->ForceNetUpdate();
}

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
    // 忙碌结束后先重建当前位置，期间已经发生的经过和轻擦不再补播
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

    // 只在附近确实有玩家时查询身体接触；每个 NPC 按固定间隔检查，不在每帧扫描
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
        // 普通过路只看前方和侧面；擦身可以引起回头，但动画仍限制转头角度
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
    for (auto It=PlayerVisits.CreateIterator();It;++It)
        if (!It.Key().IsValid() || !Players.Contains(It.Key().Get())) It.RemoveCurrent();
}

void UDeliveryNpcLookComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(UDeliveryNpcLookComponent,LookState);
}

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
