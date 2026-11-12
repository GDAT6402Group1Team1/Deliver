#include "AI/Residents/DeliveryResidentCombatComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "GameplayEffect.h"
#include "AI/Residents/DeliveryResidentCharacter.h"
#include "AI/DeliveryNpcAgentComponent.h"
#include "AI/Residents/DeliveryVehicleHoldComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

UDeliveryResidentCombatComponent::UDeliveryResidentCombatComponent()
{
    SetIsReplicatedByDefault(true); PrimaryComponentTick.bCanEverTick=true;
}
bool UDeliveryResidentCombatComponent::CanStrike(AActor* Target) const
{
    const auto* Player=Cast<ADeliveryCharacter>(Target);
    const auto* Receiver=Cast<IDeliveryImpactReceiver>(Target);
    const bool bValidReceiver=Receiver && !Receiver->IsKnockedDown();
    const bool bValidPlayer=Player && Player->GetActiveRagdoll() && Player->GetActiveRagdoll()->GetControlMode()==EDeliveryRagdollControlMode::Active;
    if (bDown || !IsValid(Target) || Target==GetOwner() || Target->GetWorld()!=GetWorld() ||
        (!bValidReceiver && !bValidPlayer) ||
        FVector::DistSquared(Target->GetActorLocation(),GetOwner()->GetActorLocation())>FMath::Square(StrikeRange)) return false;
    FHitResult Hit; FCollisionQueryParams Params(SCENE_QUERY_STAT(ResidentStrike),false,GetOwner());
    return !GetWorld()->LineTraceSingleByChannel(Hit,GetOwner()->GetActorLocation(),Target->GetActorLocation(),ECC_Visibility,Params) || Hit.GetActor()==Target;
}
bool UDeliveryResidentCombatComponent::StartStrike(AActor* Target,FGuid Id)
{
    if (!GetOwner()->HasAuthority() || !Id.IsValid() || AttackId.IsValid() || GetWorld()->GetTimeSeconds()<NextStrikeAt || !CanStrike(Target)) return false;
    AttackId=Id; AttackTarget=Target; HitAt=GetWorld()->GetTimeSeconds()+.35; NextStrikeAt=HitAt+1;
    return true;
}
void UDeliveryResidentCombatComponent::CancelStrike() { AttackId.Invalidate(); AttackTarget.Reset(); }
void UDeliveryResidentCombatComponent::ReceiveImpact(const FVector& Impulse,AActor* Source)
{
    auto* Resident=Cast<ADeliveryResidentCharacter>(GetOwner());
    if (!Resident || !Resident->HasAuthority() || bDown || Impulse.ContainsNaN()) return;
    CancelStrike(); Resident->Agent->CancelPlan(TEXT("Hit"));
    if (auto* Hold=Resident->GetHeldVehicle()) Hold->Release(TEXT("HolderHit"));
    bDown=true; bDeliberate=false; bRecovering=false; RecoverAt=GetWorld()->GetTimeSeconds()+RecoverySeconds;
    // CharacterMovement 处理扫掠和地面，冲量速度有界，不改玩家持续物理驱动
    Resident->LaunchCharacter(Impulse.GetClampedToMaxSize(450.f)+FVector(0,0,100),true,true);
    OnRep_Down(); Resident->ForceNetUpdate();
    Resident->ObserveFact(TEXT("Hit"),Source,TEXT("我被这个对象击倒了"),EDeliveryNpcEvidenceSource::Seen);
}
bool UDeliveryResidentCombatComponent::SetDeliberateProne(bool bProne)
{
    if (!GetOwner()->HasAuthority() || (bDown && !bDeliberate)) return false;
    bDown=bProne; bDeliberate=bProne; RecoverAt=GetWorld()->GetTimeSeconds()+4;
    OnRep_Down(); GetOwner()->ForceNetUpdate(); return true;
}
void UDeliveryResidentCombatComponent::OnRep_Down()
{
    if (auto* Resident=Cast<ADeliveryResidentCharacter>(GetOwner())) Resident->GetCharacterMovement()->MaxWalkSpeed=bDown?0.f:180.f;
}
void UDeliveryResidentCombatComponent::TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Fn)
{
    Super::TickComponent(Dt,Type,Fn); if (!GetOwner()->HasAuthority()) return;
    const double Now=GetWorld()->GetTimeSeconds();
    if (AttackId.IsValid() && Now>=HitAt)
    {
        auto* Target=AttackTarget.Get(); CancelStrike(); // 先清窗口，再调用外部目标，重入不重复命中
        if (CanStrike(Target))
        {
            const FVector Direction=(Target->GetActorLocation()-GetOwner()->GetActorLocation()).GetSafeNormal();
            if (auto* Receiver=Cast<IDeliveryImpactReceiver>(Target))
                Receiver->ReceiveImpactFrom(Direction*35000.f,Target->GetActorLocation(),GetOwner());
            else if (auto* Player=Cast<ADeliveryCharacter>(Target))
            {
                if (auto* ASC=Player->GetAbilitySystemComponent())
                {
                    auto Context=ASC->MakeEffectContext(); Context.AddSourceObject(GetOwner());
                    auto Spec=ASC->MakeOutgoingSpec(Player->GetDamageEffect(),1,Context);
                    if (Spec.IsValid()) { Spec.Data->SetSetByCallerMagnitude(TAG_Effect_Type_Damage,-10.f); ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data); }
                }
                Player->GetActiveRagdoll()->ApplyMeleeImpact(Direction*900.f,Target->GetActorLocation());
            }
        }
    }
    if (bDown && Now>=RecoverAt)
    {
        if (!bRecovering)
        {
            auto* Resident=CastChecked<ADeliveryResidentCharacter>(GetOwner());
            if (!Resident->GetCharacterMovement()->IsMovingOnGround()) return;
            bRecovering=true; RecoverAt=Now+1.6; GetOwner()->ForceNetUpdate(); return;
        }
        bDown=false; bDeliberate=false; bRecovering=false; OnRep_Down(); GetOwner()->ForceNetUpdate();
        if (auto* Resident=Cast<ADeliveryResidentCharacter>(GetOwner()))
            Resident->ObserveFact(TEXT("Recovered"),nullptr,TEXT("我已经恢复站立，可以继续行动"),EDeliveryNpcEvidenceSource::ActionResult);
    }
}
void UDeliveryResidentCombatComponent::EndPlay(const EEndPlayReason::Type Reason) { CancelStrike(); Super::EndPlay(Reason); }
void UDeliveryResidentCombatComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps); DOREPLIFETIME(UDeliveryResidentCombatComponent,bDown); DOREPLIFETIME(UDeliveryResidentCombatComponent,bDeliberate); DOREPLIFETIME(UDeliveryResidentCombatComponent,bRecovering);
}
