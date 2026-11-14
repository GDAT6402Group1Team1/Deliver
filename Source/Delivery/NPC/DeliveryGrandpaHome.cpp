#include "NPC/DeliveryGrandpa.h"
#include "World/DeliveryHome.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/World/DeliveryTargetComponent.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/PlayerState.h"
#include "Engine/World.h"
#include "Delivery.h"

void ADeliveryGrandpa::HandleDeliveryConfirmed(UDeliveryTaskDefinition* Task,AActor* Package,APlayerState* Player)
{
    const auto* Item=IsValid(Package)?Package->FindComponentByClass<UDeliveryItemComponent>():nullptr;
    const auto* Manager=UDeliveryTaskManagerComponent::Get(this);
    if (!HasAuthority() || bHomeDeliveryConfirmed || !IsValid(Package) || !IsValid(Task) || Task!=DeliveryTarget->ExpectedTask ||
        Task->TaskId!=TEXT("Task_001") || !Package->IsA<ADeliveryPackageItem>() || Package->GetWorld()!=GetWorld() ||
        !Item || Item->OwningTask!=Task || !IsValid(Player) || Player->GetWorld()!=GetWorld() ||
        !Manager || !Manager->IsTaskRegistered(Task) || Manager->GetTaskStatus(Task)!=EDeliveryTaskStatus::Completed) return;
    // 只有收件组件成功交付本件的通知能写入，单独收到任务 Completed 不解锁住房
    bHomeDeliveryConfirmed=true;
    ConfirmedDeliveryId=FGuid::NewGuid();
    bDelivered=true;
    UpdateBlackboard();
    RefreshInteractions();
    ForceNetUpdate();
    UE_LOG(LogDelivery,Log,TEXT("Grandpa received home delivery event=%s task=%s player=%s"),
        *ConfirmedDeliveryId.ToString(),*Task->TaskId.ToString(),*GetNameSafe(Player));
}

bool ADeliveryGrandpa::BeginMoveOut()
{
    if (!HasAuthority() || GetNetMode()==NM_Client) return false;
    if (HasMovedOut()) return true;
    bMoveOutActive=true;
    bMotionActive=false;
    bWalking=false;
    CurrentAction=EDeliveryGrandpaAction::MoveOut;
    NextMoveOutRetry=0;
    SetMoveOutWaiting(bHomeDeliveryConfirmed?TEXT("PreparingRoute"):TEXT("DeliveryNotConfirmed"));
    return true;
}

void ADeliveryGrandpa::SetMoveOutWaiting(FName Reason)
{
    if (HasMovedOut()) return;
    const bool bChanged=MoveOutState!=EDeliveryGrandpaMoveOutState::Waiting || MoveOutWaitReason!=Reason || bWalking;
    MoveOutState=EDeliveryGrandpaMoveOutState::Waiting;
    MoveOutWaitReason=Reason;
    bWalking=false;
    NextMoveOutRetry=GetWorld()->GetTimeSeconds()+FMath::Clamp(MoveOutRetrySeconds,.2f,5.f);
    if (bChanged)
    {
        OnRep_Action();
        ForceNetUpdate();
        UE_LOG(LogDelivery,Log,TEXT("Grandpa move out waiting point=%d reason=%s"),DeparturePointIndex,*Reason.ToString());
    }
}

void ADeliveryGrandpa::CancelMoveOut()
{
    if (!HasAuthority() || !bMoveOutActive) return;
    bMoveOutActive=false;
    bMotionActive=false;
    bWalking=false;
    if (!HasMovedOut()) SetMoveOutWaiting(TEXT("BehaviorCancelled"));
}

bool ADeliveryGrandpa::FindMoveOutSupport(const FVector& Position,FVector& OutSupportedPosition) const
{
    if (!IsValid(Capsule) || Position.ContainsNaN()) return false;
    const float StepUp=FMath::Clamp(MoveOutMaxStepUp,0.f,40.f);
    const float StepDown=FMath::Clamp(MoveOutMaxStepDown,0.f,80.f);
    FCollisionQueryParams Query(SCENE_QUERY_STAT(GrandpaDepartureSupport),false,this);
    const FCollisionResponseParams Responses(Capsule->GetCollisionResponseToChannels());
    FHitResult Floor;
    const float HalfHeight=Capsule->GetScaledCapsuleHalfHeight();
    const FVector Feet=Position-FVector(0,0,HalfHeight);
    // 射线取得落脚地面，实际抬升、前进和下降都用完整胶囊扫掠
    if (!GetWorld()->LineTraceSingleByChannel(Floor,Feet+FVector(0,0,StepUp+2.f),
        Feet-FVector(0,0,StepDown+2.f),Capsule->GetCollisionObjectType(),Query,Responses) ||
        Floor.bStartPenetrating || Floor.ImpactNormal.Z<.7f) return false;
    const auto* Surface=Floor.GetComponent();
    if (!IsValid(Surface) || Surface->IsSimulatingPhysics(Floor.BoneName) ||
        (Surface->GetCollisionObjectType()!=ECC_WorldStatic && Surface->GetCollisionObjectType()!=ECC_WorldDynamic)) return false;
    OutSupportedPosition=FVector(Position.X,Position.Y,Floor.ImpactPoint.Z+HalfHeight+2.f);
    const float HeightChange=OutSupportedPosition.Z-Position.Z;
    return !OutSupportedPosition.ContainsNaN() && HeightChange<=StepUp+2.f && HeightChange>=-StepDown;
}

void ADeliveryGrandpa::SetMoveOutWalking()
{
    if (!bWalking || MoveOutState!=EDeliveryGrandpaMoveOutState::Walking)
    {
        bWalking=true;
        MoveOutState=EDeliveryGrandpaMoveOutState::Walking;
        MoveOutWaitReason=NAME_None;
        OnRep_Action();
        ForceNetUpdate();
    }
}

void ADeliveryGrandpa::UpdateMoveOut(float DeltaSeconds)
{
    if (!HasAuthority() || !bMoveOutActive || HasMovedOut() || !FMath::IsFinite(DeltaSeconds) || DeltaSeconds<=0) return;
    if (GetWorld()->GetTimeSeconds()<NextMoveOutRetry) return;
    const auto* Manager=UDeliveryTaskManagerComponent::Get(this);
    if (!bHomeDeliveryConfirmed || !Manager || Manager->GetTaskStatus(DeliveryTarget->ExpectedTask)!=EDeliveryTaskStatus::Completed)
    { SetMoveOutWaiting(TEXT("DeliveryNotConfirmed")); return; }
    if (!IsValid(SharedHome) || SharedHome->GetWorld()!=GetWorld() || SharedHome->FormerResident!=this ||
        ADeliveryHome::FindSharedHome(this)!=SharedHome || !IsValid(SharedHome->ExistingHouse))
    { SetMoveOutWaiting(TEXT("SharedHomeUnavailable")); return; }
    if (SharedHome->DepartureRoute.Num()<2 || SharedHome->DepartureRoute.Num()>32)
    { SetMoveOutWaiting(TEXT("DepartureRouteMissing")); return; }
    FVector Point;
    if (!SharedHome->GetDeparturePoint(DeparturePointIndex,Point))
    { SetMoveOutWaiting(TEXT("DeparturePointInvalid")); return; }
    FVector Position=GetActorLocation();
    FVector Support;
    if (!FindMoveOutSupport(Position,Support))
    { SetMoveOutWaiting(TEXT("DepartureFloorUnavailable")); return; }
    const float Step=FMath::Clamp(MoveOutSpeed,10.f,200.f)*FMath::Min(DeltaSeconds,.1f);
    const TWeakObjectPtr<ADeliveryGrandpa> WeakThis(this);
    FHitResult Block;
    const auto IsStepEdge=[this](const FHitResult& Hit)
    {
        const auto* Edge=Hit.GetComponent();
        const float BottomSphereZ=GetActorLocation().Z-Capsule->GetScaledCapsuleHalfHeight()+Capsule->GetScaledCapsuleRadius();
        return !Hit.bStartPenetrating && Hit.Normal.Z>UE_KINDA_SMALL_NUMBER && Hit.ImpactPoint.Z<=BottomSphereZ+2.f &&
            IsValid(Edge) && !Edge->IsSimulatingPhysics(Hit.BoneName) &&
            (Edge->GetCollisionObjectType()==ECC_WorldStatic || Edge->GetCollisionObjectType()==ECC_WorldDynamic);
    };
    // 下台阶分帧扫掠落到支撑面，落稳后才能认领房屋
    if (FMath::Abs(Support.Z-Position.Z)>3.f)
    {
        AddActorWorldOffset(FVector(0,0,FMath::Clamp(Support.Z-Position.Z,-Step*2.f,Step*2.f)),true,&Block);
        if (!WeakThis.IsValid() || !bMoveOutActive) return;
        if (!Block.bBlockingHit)
        {
            if (FVector::DistSquared(GetActorLocation(),Position)<.01f)
            { SetMoveOutWaiting(TEXT("DepartureBlocked")); return; }
            SetMoveOutWalking();
            return;
        }
        // 下降时胶囊后沿仍可搭在门槛上，继续前进才能离开边沿；竖直阻挡仍等候
        if (Support.Z>=Position.Z || !IsStepEdge(Block))
        { SetMoveOutWaiting(TEXT("DepartureBlocked")); return; }
        Position=GetActorLocation();
    }
    const float Arrival=FMath::Clamp(MoveOutArrivalRadius,1.f,30.f);
    // 路点只规定平面路线，到达高度以当前位置真实支撑为准
    if (FVector::DistSquared2D(Position,Point)<=FMath::Square(Arrival) && FMath::Abs(Support.Z-Position.Z)<=3.f)
    {
        if (DeparturePointIndex+1<SharedHome->DepartureRoute.Num()) { ++DeparturePointIndex; return; }
        // 真正走到最后一个屋外路点才认领，不用销毁或传送代替离开
        MoveOutState=EDeliveryGrandpaMoveOutState::Departed;
        if (!SharedHome->TryClaimAfterDeparture(this))
        { MoveOutState=EDeliveryGrandpaMoveOutState::Walking; SetMoveOutWaiting(TEXT("HomeClaimUnavailable")); return; }
        bMoveOutActive=false;
        bWalking=false;
        MoveOutWaitReason=NAME_None;
        OnRep_Action();
        ForceNetUpdate();
        return;
    }
    const FVector Delta=FVector(Point.X-Position.X,Point.Y-Position.Y,0).GetClampedToMaxSize(Step);
    const FVector Next=Position+Delta;
    FVector NextSupport;
    if (!FindMoveOutSupport(Next,NextSupport))
    { SetMoveOutWaiting(TEXT("DepartureFloorUnavailable")); return; }

    const float Rise=NextSupport.Z-Position.Z;
    if (Rise>.1f)
    {
        FCollisionQueryParams Query(SCENE_QUERY_STAT(GrandpaDepartureStep),false,this);
        const FCollisionResponseParams Responses(Capsule->GetCollisionResponseToChannels());
        const FVector Raised=Position+FVector(0,0,Rise);
        // 上台阶先核对抬升和前进两段的完整胶囊空间，避免抬起后卡在家具前
        if (GetWorld()->SweepSingleByChannel(Block,Position,Raised,GetActorQuat(),Capsule->GetCollisionObjectType(),
            Capsule->GetCollisionShape(),Query,Responses) ||
            GetWorld()->SweepSingleByChannel(Block,Raised,Raised+Delta,GetActorQuat(),Capsule->GetCollisionObjectType(),
            Capsule->GetCollisionShape(),Query,Responses))
        { SetMoveOutWaiting(TEXT("DepartureBlocked")); return; }
        AddActorWorldOffset(FVector(0,0,Rise),true,&Block);
        if (!WeakThis.IsValid() || !bMoveOutActive) return;
        if (Block.bBlockingHit)
        { SetMoveOutWaiting(TEXT("DepartureBlocked")); return; }
    }

    // 先横移再下降，胶囊仍碰到门槛边沿时由真实碰撞支撑，不斜穿门槛
    AddActorWorldOffset(Delta,true,&Block);
    if (!WeakThis.IsValid() || !bMoveOutActive) return;
    const bool bBlocked=Block.bBlockingHit;
    if (!FindMoveOutSupport(GetActorLocation(),Support))
    { SetMoveOutWaiting(TEXT("DepartureFloorUnavailable")); return; }
    if (Support.Z<GetActorLocation().Z-.1f)
    {
        AddActorWorldOffset(FVector(0,0,FMath::Max(Support.Z-GetActorLocation().Z,-Step*2.f)),true,&Block);
        if (!WeakThis.IsValid() || !bMoveOutActive) return;
        if (Block.bBlockingHit && !IsStepEdge(Block))
        { SetMoveOutWaiting(TEXT("DepartureBlocked")); return; }
    }
    if (bBlocked || FVector::DistSquared(GetActorLocation(),Position)<.01f)
    { SetMoveOutWaiting(TEXT("DepartureBlocked")); return; }
    SetActorRotation(FMath::RInterpConstantTo(GetActorRotation(),Delta.GetSafeNormal2D().Rotation(),DeltaSeconds,180.f));
    SetMoveOutWalking();
}
