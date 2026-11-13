#include "AI/Presentation/DeliveryNpcLookComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
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
    if (!IsValid(GetOwner())) return false;
    if (const auto* Receiver=Cast<IDeliveryImpactReceiver>(GetOwner()); Receiver && Receiver->IsKnockedDown()) return false;
    if (const auto* Walker=Cast<ADeliveryPedestrian>(GetOwner()); Walker && Walker->IsStandingUp()) return false;
    return true;
}

AActor* UDeliveryNpcLookComponent::GetLookTarget() const
{
    AActor* Target=LookState.Target.Get();
    if (!CanLook() || !IsValid(Target) || Target->IsActorBeingDestroyed() || Target->GetWorld()!=GetWorld() || ServerTime()>=LookState.EndTime ||
        FVector::DistSquared(GetOwner()->GetActorLocation(),Target->GetActorLocation())>FMath::Square(FMath::Max(NoticeDistance,ReleaseDistance))) return nullptr;
    return Target;
}

bool UDeliveryNpcLookComponent::IsAfterBrush() const { return GetLookTarget() && LookState.bAfterBrush; }

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
    if (!bAfterBrush && IsAfterBrush()) return;
    LookState.Target=Player;
    LookState.EndTime=Now+FMath::Max(.2f,bAfterBrush?BrushLookDuration:LookDuration);
    LookState.bAfterBrush=bAfterBrush;
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
    if (!CanLook()) { ClearLook(); return; }
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
        const FVector SampledVelocity=Visit.bHasPosition && DeltaTime>SMALL_NUMBER?(Player->GetActorLocation()-Visit.LastPosition)/DeltaTime:FVector::ZeroVector;
        Visit.LastPosition=Player->GetActorLocation();
        Visit.bHasPosition=true;
        const FVector Direction=(Player->GetActorLocation()-Origin).GetSafeNormal2D();
        const bool bNearby=Distance<=FMath::Square(NoticeDistance) &&
            FVector::DotProduct(GetOwner()->GetActorForwardVector(),Direction)>=-.15f;
        const bool bTouching=Touching.Contains(Player);
        const bool bGentle=(PlayerBodyVelocity(Player,SampledVelocity)-OwnerVelocity).Size2D()<=BrushMaxSpeed;
        const bool bBrush=bTouching && !Visit.bTouching && bGentle && Now>=Visit.NextBrush;
        const bool bPassing=bNearby && !Visit.bNearby && Now>=Visit.NextLook;
        Visit.bNearby=bNearby;
        Visit.bTouching=bTouching;
        if (!bBrush && !bPassing) continue;
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
    Super::EndPlay(Reason);
}
