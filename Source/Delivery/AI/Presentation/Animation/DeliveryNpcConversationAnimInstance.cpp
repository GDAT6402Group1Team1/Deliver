#include "AI/Presentation/Animation/DeliveryNpcConversationAnimInstance.h"
#include "AI/Presentation/Face/DeliveryNpcExpressionSet.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Presentation/Face/DeliveryNpcLookComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Pawn.h"
#include "Engine/SkeletalMesh.h"

// ── 视线 ──
// 交谈叠加动画 ABP_NpcConversation 每帧调用 NativeUpdateAnimation：先决定 NPC 要看谁，再算出 LookWeight、PostureWeight、ChestRotation、HeadRotation
// 动画蓝图读这些值转头、挺胸；表情代码调用 GetHeadRelativeLookDirection 算眼睛还要再偏多少

/// @brief 动画开始时清空状态，并从骨架的参考姿势量出 Head 骨的前方轴 HeadForwardAxis 和上方轴 HeadUpAxis
/// 不同模型的头骨朝向不同，量出来之后 GetHeadRelativeLookDirection 才能算对方向
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

/// @brief 把从头部到 WorldPoint 的方向换算到头骨自己的坐标里：X 向前、Y 向右、Z 向上
/// 头已经转过去的部分包含在头骨朝向里，所以结果就是眼睛还要再偏的方向
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

/// @brief 每帧在游戏线程决定看谁、转多少；动画图只读算好的值
/// 看谁按顺序选：正在倾听或等回复时看说话的人；说台词或等声音时看 SpeechTarget（台词说给谁）
/// 正在执行 LookAt 动作时看动作目标；都不是就看 Look 组件选的目标；离得比 MaxLookAtDistance 远就不看
void UDeliveryNpcConversationAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
    Super::NativeUpdateAnimation(DeltaSeconds);

    // 只在游戏线程读角色和组件的状态；动画在工作线程更新时直接返回，用上一帧算好的值
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
    const bool bListeningReaction=Npc->Presentation->IsShowingListeningReaction();

    // bThinking：在倾听但刚开始的反应已经过去、在等模型回复、或者回复到了但声音还没开始播，都保持思考的姿态
    const bool bThinking=(bListening && !bListeningReaction) || Npc->Agent->IsDecisionPending() || Npc->Presentation->IsWaitingForSpeechPlayback();

    // 正在出拳、交战、逃跑、拽车时不做交谈姿态；被打倒、起身、受击后仰时也不做
    const bool bBusyAction=State.Status==EDeliveryNpcActionStatus::Running &&
        (State.Action==EDeliveryNpcAction::StrikeOnce || State.Action==EDeliveryNpcAction::Fight ||
         State.Action==EDeliveryNpcAction::Flee || State.Action==EDeliveryNpcAction::HoldVehicle);
    const bool bCanLookPose=Settings && Settings->bConversationPose && !Npc->IsKnockedDown() &&
        !Npc->Combat->IsRecovering() && !Npc->Combat->IsStriking() && !Npc->Combat->IsReactingToHit() && !Npc->GetHeldVehicle() &&
        !bBusyAction;
    const bool bCanPose=bCanLookPose && Npc->GetVelocity().Size2D()<20;
    const bool bConversing=bListening || bThinking || Speech.bActive;
    if (AActor* Speaker=Npc->Agent->GetListeningSpeaker()) PreviousSpeaker=Speaker;
    AActor* Target=nullptr;
    if (bListening || (bThinking && Npc->Agent->GetListeningSpeaker())) Target=Npc->Agent->GetListeningSpeaker();
    else if (Speech.bActive || (bThinking && IsValid(State.SpeechTarget)))
    {
        // 说台词或等声音时看 SpeechTarget；它没了的话，回答别人时看刚才说话的人，否则看 Look 组件选的目标
        Target=State.SpeechTarget.Get();
        if (!IsValid(Target)) Target=State.ReplyToEventId.IsValid()?PreviousSpeaker.Get():(Npc->Look?Npc->Look->GetLookTarget():nullptr);
    }
    else if ((State.Status==EDeliveryNpcActionStatus::Running || State.Status==EDeliveryNpcActionStatus::Accepted) && State.Action==EDeliveryNpcAction::LookAt)
        Target=State.Target.Get();
    else if (Npc->Look) Target=Npc->Look->GetLookTarget();
    const bool bCanLook=bCanLookPose && IsValid(Target) && Target->GetWorld()==Npc->GetWorld() &&
        FVector::DistSquared(Target->GetActorLocation(),Npc->GetActorLocation())<=FMath::Square(Npc->Agent->MaxLookAtDistance);

    // LookWeight 和 PostureWeight 按 BlendSpeed 平滑变化；身体还在走动（速度超过 20 cm/s）时 PostureWeight 回到 0
    LookWeight=FMath::FInterpTo(LookWeight,bCanLook?1.f:0.f,DeltaSeconds,BlendSpeed);
    PostureWeight=FMath::FInterpTo(PostureWeight,bCanPose && bConversing?1.f:0.f,DeltaSeconds,BlendSpeed);
    if (Settings) HeadTurnLimit=Settings->HeadTurnLimit;

    // 看另一位 NPC 时看它的 Head 骨；第一次有目标时从正前方 200 cm 开始，按 GazeFollowSpeed 平滑移向目标
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

    // 胸口绕角色右方的轴前倾 ListeningLean 度（刚开始倾听时）；头绕视线方向的轴歪 ListeningHeadTilt 或 ThinkingHeadTilt 度
    // 两根轴都换算到网格坐标里，角色蓝图给网格设的朝向不受影响
    // 思考时歪头只在还没说台词、动作也没有带表情时出现
    const FTransform MeshTransform=Mesh->GetComponentTransform();
    const FVector LeanAxis=MeshTransform.InverseTransformVectorNoScale(Npc->GetActorRightVector()).GetSafeNormal();
    const FVector Forward=bHasLookTarget?(LookTargetWorld-Mesh->GetSocketLocation(TEXT("Head"))).GetSafeNormal():Npc->GetActorForwardVector();
    const FVector TiltAxis=MeshTransform.InverseTransformVectorNoScale(Forward).GetSafeNormal();
    const float Lean=Settings && bListeningReaction?Settings->ListeningLean:0.f;
    const FName ActionEmotion=Npc->Presentation->GetActionEmotion();
    const bool bEmotionChosen=!ActionEmotion.IsNone() && ActionEmotion!=TEXT("Calm");
    const float Tilt=Settings?(bListeningReaction?Settings->ListeningHeadTilt:(bThinking && !Speech.bActive && !bEmotionChosen?Settings->ThinkingHeadTilt:0.f)):0.f;
    const FRotator DesiredChest=FQuat(LeanAxis,FMath::DegreesToRadians(Lean)).Rotator();
    const FRotator DesiredHead=FQuat(TiltAxis,FMath::DegreesToRadians(Tilt)).Rotator();
    ChestRotation=FMath::RInterpTo(ChestRotation,DesiredChest,DeltaSeconds,BlendSpeed);
    HeadRotation=FMath::RInterpTo(HeadRotation,DesiredHead,DeltaSeconds,BlendSpeed);
}

/// @brief 动画结束时清空看向目标和权重
void UDeliveryNpcConversationAnimInstance::NativeUninitializeAnimation()
{
    PreviousSpeaker.Reset();
    bHasLookTarget=false;
    LookWeight=0;
    PostureWeight=0;
    Super::NativeUninitializeAnimation();
}
