#include "AI/Presentation/DeliveryNpcConversationAnimInstance.h"
#include "AI/Presentation/DeliveryNpcExpressionSet.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Presentation/DeliveryNpcLookComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Pawn.h"
#include "Engine/SkeletalMesh.h"

void UDeliveryNpcConversationAnimInstance::NativeInitializeAnimation()
{
    Super::NativeInitializeAnimation();
    PreviousSpeaker.Reset();
    bHasLookTarget=false;
    LookWeight=0;
    PostureWeight=0;
    ChestRotation=FRotator::ZeroRotator;
    HeadRotation=FRotator::ZeroRotator;
    if (const auto* Mesh=GetSkelMeshComponent(); Mesh && Mesh->GetSkeletalMeshAsset())
    {
        const auto& Skeleton=Mesh->GetSkeletalMeshAsset()->GetRefSkeleton();
        const int32 Head=Skeleton.FindBoneIndex(TEXT("Head"));
        if (Head!=INDEX_NONE)
        {
            FTransform HeadInMesh=Skeleton.GetRefBonePose()[Head];
            for (int32 Parent=Skeleton.GetParentIndex(Head);Parent!=INDEX_NONE;Parent=Skeleton.GetParentIndex(Parent))
                HeadInMesh=HeadInMesh*Skeleton.GetRefBonePose()[Parent];
            HeadForwardAxis=HeadInMesh.InverseTransformVectorNoScale(Mesh->GetRelativeTransform().InverseTransformVectorNoScale(FVector::ForwardVector)).GetSafeNormal();
            HeadUpAxis=HeadInMesh.InverseTransformVectorNoScale(Mesh->GetRelativeTransform().InverseTransformVectorNoScale(FVector::UpVector)).GetSafeNormal();
        }
    }
}

FVector UDeliveryNpcConversationAnimInstance::GetHeadRelativeLookDirection(const FVector& WorldPoint) const
{
    const auto* Mesh=GetSkelMeshComponent();
    if (!Mesh) return FVector::ForwardVector;
    const FTransform Head=Mesh->GetSocketTransform(TEXT("Head"));
    const FVector Direction=(WorldPoint-Head.GetLocation()).GetSafeNormal();
    const FVector Forward=Head.TransformVectorNoScale(HeadForwardAxis).GetSafeNormal();
    const FVector Up=Head.TransformVectorNoScale(HeadUpAxis).GetSafeNormal();
    const FVector Right=FVector::CrossProduct(Up,Forward).GetSafeNormal();
    return FVector(FVector::DotProduct(Direction,Forward),FVector::DotProduct(Direction,Right),FVector::DotProduct(Direction,Up));
}

void UDeliveryNpcConversationAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
    Super::NativeUpdateAnimation(DeltaSeconds);
    // 这里读取角色状态，动画图随后只使用已经准备好的数值
    if (!IsInGameThread()) return;
    auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwningActor());
    auto* Mesh=GetSkelMeshComponent();
    if (!Npc || !Mesh || !Npc->Presentation || !Npc->Agent || !Npc->Combat)
    {
        LookWeight=0;
        PostureWeight=0;
        PreviousSpeaker.Reset();
        bHasLookTarget=false;
        return;
    }
    const auto* Settings=Npc->Presentation->Expressions.Get();
    const float BlendSpeed=Settings?Settings->PostureBlendSpeed:8.f;
    const auto State=Npc->Agent->GetPublicState();
    const auto Speech=Npc->Presentation->GetDisplayedSpeech();
    const bool bListening=Npc->Agent->IsListening();
    const bool bThinking=Npc->Agent->IsDecisionPending();
    const bool bBusyAction=State.Status==EDeliveryNpcActionStatus::Running &&
        (State.Action==EDeliveryNpcAction::StrikeOnce || State.Action==EDeliveryNpcAction::HoldVehicle);
    const bool bCanLookPose=Settings && Settings->bConversationPose && !Npc->IsKnockedDown() &&
        !Npc->Combat->IsRecovering() && !Npc->Combat->IsStriking() && !Npc->GetHeldVehicle() &&
        !bBusyAction;
    const bool bCanPose=bCanLookPose && Npc->GetVelocity().Size2D()<20;
    const bool bConversing=bListening || bThinking || Speech.bActive;
    if (AActor* Speaker=Npc->Agent->GetListeningSpeaker()) PreviousSpeaker=Speaker;
    AActor* Target=nullptr;
    if (bListening || (bThinking && Npc->Agent->GetListeningSpeaker())) Target=Npc->Agent->GetListeningSpeaker();
    else if (Speech.bActive)
    {
        Target=State.Target.Get();
        if (!IsValid(Target)) Target=State.ReplyToEventId.IsValid()?PreviousSpeaker.Get():(Npc->Look?Npc->Look->GetLookTarget():nullptr);
    }
    else if ((State.Status==EDeliveryNpcActionStatus::Running || State.Status==EDeliveryNpcActionStatus::Accepted) && State.Action==EDeliveryNpcAction::LookAt)
        Target=State.Target.Get();
    else if (Npc->Look) Target=Npc->Look->GetLookTarget();
    const bool bCanLook=bCanLookPose && IsValid(Target) && Target->GetWorld()==Npc->GetWorld() &&
        FVector::DistSquared(Target->GetActorLocation(),Npc->GetActorLocation())<=FMath::Square(Npc->Agent->MaxLookAtDistance);

    LookWeight=FMath::FInterpTo(LookWeight,bCanLook?1.f:0.f,DeltaSeconds,BlendSpeed);
    PostureWeight=FMath::FInterpTo(PostureWeight,bCanPose && bConversing?1.f:0.f,DeltaSeconds,BlendSpeed);
    if (Settings) HeadTurnLimit=Settings->HeadTurnLimit;
    if (bCanLook)
    {
        FVector TargetPoint=UDeliveryNpcLookComponent::GetLookPoint(Target);
        if (const auto* OtherNpc=Cast<ADeliveryNpcCharacter>(Target))
            TargetPoint=OtherNpc->GetMesh()->GetSocketLocation(TEXT("Head"));
        if (!bHasLookTarget)
        {
            LookTargetWorld=Mesh->GetSocketLocation(TEXT("Head"))+Npc->GetActorForwardVector()*200;
            bHasLookTarget=true;
        }
        LookTargetWorld=FMath::VInterpTo(LookTargetWorld,TargetPoint,DeltaSeconds,Settings->GazeFollowSpeed);
    }
    else if (LookWeight<.001f)
    {
        LookWeight=0;
        bHasLookTarget=false;
        if (!bConversing) PreviousSpeaker.Reset();
    }

    // 胸部沿角色前方轻微倾斜，头部围绕视线方向侧倾
    // 使用网格坐标转换，保留角色蓝图已有的模型朝向
    const FTransform MeshTransform=Mesh->GetComponentTransform();
    const FVector LeanAxis=MeshTransform.InverseTransformVectorNoScale(Npc->GetActorRightVector()).GetSafeNormal();
    const FVector Forward=bHasLookTarget?(LookTargetWorld-Mesh->GetSocketLocation(TEXT("Head"))).GetSafeNormal():Npc->GetActorForwardVector();
    const FVector TiltAxis=MeshTransform.InverseTransformVectorNoScale(Forward).GetSafeNormal();
    const float Lean=Settings && bListening?Settings->ListeningLean:0.f;
    const float Tilt=Settings?(bListening?Settings->ListeningHeadTilt:(bThinking?Settings->ThinkingHeadTilt:0.f)):0.f;
    const FRotator DesiredChest=FQuat(LeanAxis,FMath::DegreesToRadians(Lean)).Rotator();
    const FRotator DesiredHead=FQuat(TiltAxis,FMath::DegreesToRadians(Tilt)).Rotator();
    ChestRotation=FMath::RInterpTo(ChestRotation,DesiredChest,DeltaSeconds,BlendSpeed);
    HeadRotation=FMath::RInterpTo(HeadRotation,DesiredHead,DeltaSeconds,BlendSpeed);
}

void UDeliveryNpcConversationAnimInstance::NativeUninitializeAnimation()
{
    PreviousSpeaker.Reset();
    bHasLookTarget=false;
    LookWeight=0;
    PostureWeight=0;
    Super::NativeUninitializeAnimation();
}
