#include "AI/Actions/DeliveryVehicleHoldComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Vehicle/Motorbike/DeliveryMotorbike.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

// ── 拽车 ──
// 模型让 NPC 拽住玩家的车时，NPC 调用 CanHold 检查、TryHold 拽住；之后 TickComponent 每帧把 NPC 拉到车尾后面
// 玩家持续加油门 EscapeThrottleSeconds 秒（UpdateThrottle 计时），或者拽满 MaxHoldSeconds 秒、NPC 被打倒、站位没了，都调用 Release 松手

/// @brief 组件要同步 Holder 给客户端，并每帧更新拽车 NPC 的位置
UDeliveryVehicleHoldComponent::UDeliveryVehicleHoldComponent()
{
    SetIsReplicatedByDefault(true);
    PrimaryComponentTick.bCanEverTick = true;
}

/// @brief 在车尾后方 145 cm 处找 NPC 站的位置：往下打射线找地面，地面坡度太陡（法线 Z 小于 0.65）返回 false
bool UDeliveryVehicleHoldComponent::FindHoldLocation(const ADeliveryNpcCharacter* Resident,FVector& Location) const
{
    Location=GetOwner()->GetActorLocation()-GetOwner()->GetActorForwardVector()*145.f;
    FHitResult Ground;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(ResidentHoldGround),false,GetOwner());
    if (Resident) Params.AddIgnoredActor(Resident);
    FCollisionObjectQueryParams Objects; Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    if (!GetWorld()->LineTraceSingleByObjectType(Ground,Location+FVector(0,0,180),Location-FVector(0,0,300),Objects,Params) || Ground.ImpactNormal.Z<.65f) return false;
    const float HalfHeight=Resident?Resident->GetCapsuleComponent()->GetScaledCapsuleHalfHeight():70.f;

    // 水平位置跟着车尾，高度取地面加上 NPC 胶囊半高，不用车身的高度
    Location.Z=Ground.ImpactPoint.Z+HalfHeight+2.f;
    return true;
}

/// @brief 返回 NPC 拽车时站的位置；Resident 为空时按当前的 Holder 算
FVector UDeliveryVehicleHoldComponent::GetHoldLocation(const ADeliveryNpcCharacter* Resident) const
{
    FVector Location;
    FindHoldLocation(Resident?Resident:Holder.Get(),Location);
    return Location;
}

/// @brief 只判断不拽：车上有人开、车速不超过 100 cm/s、没有别人拽着、NPC 没被打倒且离站位不超过 180 cm
/// 还要找得到站位，而且从 NPC 当前位置到站位扫一遍胶囊不会撞到东西
bool UDeliveryVehicleHoldComponent::CanHold(const ADeliveryNpcCharacter* Resident) const
{
    const auto* Bike = Cast<ADeliveryMotorbike>(GetOwner());
    if (!Bike || !IsValid(Resident) || Resident->GetWorld()!=GetWorld() || Resident->IsKnockedDown() ||
        IsHeld() || Resident->GetHeldVehicle() || FMath::Abs(Bike->GetCurrentSpeed())>100.f || !Bike->GetDriver() ||
        FVector::DistSquared(Resident->GetActorLocation(), GetHoldLocation(Resident))>FMath::Square(180.f)) return false;
    FVector Location;
    if (!FindHoldLocation(Resident,Location)) return false;
    FHitResult Hit;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(ResidentHold), false, Resident);
    Params.AddIgnoredActor(Bike);
    return !GetWorld()->SweepSingleByChannel(Hit, Resident->GetActorLocation(), Location, FQuat::Identity,
        ECC_Pawn, Resident->GetCapsuleComponent()->GetCollisionShape(), Params);
}

/// @brief 服务器：CanHold 通过后拽住车，记下 Holder、HoldActionId，ExpiresAt 设为 MaxHoldSeconds 秒后
/// 再调用 OnRep_Holder 设置车和 NPC 互相忽略碰撞，ForceNetUpdate 让客户端马上收到
bool UDeliveryVehicleHoldComponent::TryHold(ADeliveryNpcCharacter* Resident, FGuid ActionId)
{
    if (!GetOwner()->HasAuthority() || !ActionId.IsValid() || !CanHold(Resident)) return false;
    Holder=Resident; HoldActionId=ActionId; ThrottleSeconds=0;
    ExpiresAt=GetWorld()->GetTimeSeconds()+FMath::Clamp(MaxHoldSeconds,.5f,5.f);
    Resident->SetHeldVehicle(this);
    OnRep_Holder(nullptr);
    GetOwner()->ForceNetUpdate();
    return true;
}

/// @brief 服务器：松开车，清掉 Holder 和 HoldActionId，恢复碰撞
/// 再调用 Agent->TryFinishAction，把拽车动作按 Succeeded 结束，Reason 写明为什么松手；没人拽时调用也不会出错
void UDeliveryVehicleHoldComponent::Release(FName Reason)
{
    if (!GetOwner()->HasAuthority()) return;
    auto* Previous=Holder.Get(); const FGuid PreviousId=HoldActionId;
    Holder=nullptr; HoldActionId.Invalidate(); ThrottleSeconds=0;
    OnRep_Holder(Previous);
    if (IsValid(Previous))
    {
        Previous->SetHeldVehicle(nullptr);
        Previous->Agent->TryFinishAction(PreviousId, EDeliveryNpcActionStatus::Succeeded,Reason);
    }
    GetOwner()->ForceNetUpdate();
}

/// @brief 摩托车每帧调用：油门绝对值超过 0.8 时 ThrottleSeconds 累加，松开就清零；累计到 EscapeThrottleSeconds 时调用 Release
void UDeliveryVehicleHoldComponent::UpdateThrottle(float Throttle,float Dt)
{
    if (!GetOwner()->HasAuthority() || !IsHeld()) return;

    // 车被拽住时速度已经乘了 SpeedScale，所以按持续加油门的时间判断挣脱，不看车速
    ThrottleSeconds=FMath::Abs(Throttle)>.8f ? ThrottleSeconds+Dt : 0;
    if (ThrottleSeconds>=EscapeThrottleSeconds) Release(TEXT("DriverEscaped"));
}

/// @brief 服务器每帧：检查还能不能继续拽，能就把 NPC 平滑拉到车尾站位并朝向车头方向
/// 车上没人、NPC 被打倒、超过 ExpiresAt、NPC 的当前动作已经不是 HoldActionId、找不到站位、离站位超过 250 cm、移动时撞到东西，都调用 Release
void UDeliveryVehicleHoldComponent::TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Fn)
{
    Super::TickComponent(Dt,Type,Fn);
    if (!GetOwner()->HasAuthority() || !IsHeld()) return;
    const auto* Bike=Cast<ADeliveryMotorbike>(GetOwner());
    if (!Bike || !Bike->GetDriver() || Holder->IsKnockedDown() || GetWorld()->GetTimeSeconds()>=ExpiresAt ||
        Holder->Agent->GetPublicState().ActionId!=HoldActionId || !Holder->Agent->HasActiveAction())
    { Release(TEXT("HoldEnded")); return; }
    FVector Target;
    if (!FindHoldLocation(Holder,Target)) { Release(TEXT("HoldGroundLost")); return; }
    if (FVector::DistSquared(Target,Holder->GetActorLocation())>FMath::Square(250.f))
    { Release(TEXT("HoldTooFar")); return; }
    FHitResult Hit;
    Holder->SetActorLocation(FMath::VInterpTo(Holder->GetActorLocation(),Target,Dt,12.f),true,&Hit);
    Holder->SetActorRotation(GetOwner()->GetActorRotation());
    if (Hit.bBlockingHit) Release(TEXT("HoldObstructed"));
}

/// @brief 车被销毁时：服务器调用 Release；客户端只恢复碰撞
void UDeliveryVehicleHoldComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    if (GetOwner()->HasAuthority()) Release(TEXT("VehicleEndPlay"));
    else { auto* Previous=Holder.Get(); Holder=nullptr; OnRep_Holder(Previous); }
    Super::EndPlay(Reason);
}

/// @brief Holder 变化时调用：之前拽车的 NPC 恢复和车的碰撞，现在拽车的 NPC 和车互相忽略碰撞
void UDeliveryVehicleHoldComponent::OnRep_Holder(ADeliveryNpcCharacter* Previous)
{
    // IgnoreActorWhenMoving 的设置不会通过网络同步，所以服务器在 TryHold、Release 里直接调用这里，客户端收到 Holder 时也调用
    auto SetIgnored=[&](ADeliveryNpcCharacter* Resident,bool bIgnore)
    {
        if (!IsValid(Resident)) return;
        Resident->GetCapsuleComponent()->IgnoreActorWhenMoving(GetOwner(),bIgnore);
        if (auto* Body=Cast<UPrimitiveComponent>(GetOwner()->GetRootComponent())) Body->IgnoreActorWhenMoving(Resident,bIgnore);
    };
    SetIgnored(Previous,false); SetIgnored(Holder,true);
}

/// @brief 登记要同步的属性：Holder
void UDeliveryVehicleHoldComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps); DOREPLIFETIME(UDeliveryVehicleHoldComponent,Holder);
}

/// @brief 有 NPC 拽着车时返回 true
bool UDeliveryVehicleHoldComponent::IsHeld() const { return IsValid(Holder.Get()); }
