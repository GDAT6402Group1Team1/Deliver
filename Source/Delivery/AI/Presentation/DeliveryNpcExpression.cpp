#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Presentation/DeliveryNpcExpressionSet.h"
#include "AI/Presentation/DeliveryNpcConversationAnimInstance.h"
#include "AI/Presentation/DeliveryNpcLookComponent.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Curves/CurveFloat.h"
#include "GameFramework/Pawn.h"

bool UDeliveryNpcPresentationComponent::IsShowingListeningReaction() const
{
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc || !Npc->Agent || !Expressions || !Npc->Agent->IsListening() || Npc->IsKnockedDown() ||
        Npc->Combat->IsRecovering() || Npc->Combat->IsStriking() || Npc->Combat->IsReactingToHit() || Npc->GetHeldVehicle()) return false;
    const double Started=Npc->Agent->GetListeningStartTime();
    return FMath::IsFinite(Started) && Started>=0 &&
        ServerTime()-Started<FMath::Clamp(Expressions->ListeningReactionDuration,.1f,2.f);
}

bool UDeliveryNpcPresentationComponent::IsWaitingForMoreAudio() const
{
    if (!DisplayedSpeech.bActive || !DisplayedSpeech.bRecordedVoice || LocalVoices.IsEmpty()) return false;
    const auto& Local=LocalVoices[0];
    return Local.bRebuffering && Local.State.bActive && Local.State.bRecordedVoice &&
        Local.State.TurnId==DisplayedSpeech.TurnId && Local.State.ActionId==DisplayedSpeech.ActionId &&
        !CancelledVoiceActions.Contains(Local.State.ActionId) && !FinishedVoiceTurns.Contains(Local.State.TurnId);
}

FName UDeliveryNpcPresentationComponent::GetActionEmotion() const
{
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc || !Npc->Agent || Npc->IsKnockedDown()) return NAME_None;
    const auto State=Npc->Agent->GetPublicState();
    if (State.Status==EDeliveryNpcActionStatus::Accepted || State.Status==EDeliveryNpcActionStatus::Running) return State.Emotion;
    if (State.Status!=EDeliveryNpcActionStatus::Succeeded || State.Emotion.IsNone() ||
        State.Emotion==TEXT("Calm") || !IsValid(State.SpeechTarget)) return NAME_None;
    // 后台合成期间还没有本机声音，使用服务器同步的本轮编号；旧编号和停止通知都不能恢复表情
    const bool bThisReply=State.ActionId.IsValid() && ReplyAction==State.ActionId &&
        !CancelledVoiceActions.Contains(State.ActionId);
    return bThisReply || IsWaitingForSpeechPlayback()?State.Emotion:NAME_None;
}

void UDeliveryNpcPresentationComponent::TickExpression(float DeltaTime)
{
    auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc || !Expressions) return;
    const auto Display=GetDisplayedSpeech();
    const FName ActionEmotion=GetActionEmotion();
    FName Pose=TEXT("Calm");
    if (Npc->IsKnockedDown()) Pose=TEXT("Down");
    else if (Npc->Combat->IsReactingToHit()) Pose=TEXT("Surprised");
    else if (Display.bActive) Pose=Display.Emotion;
    else if (IsShowingListeningReaction()) Pose=TEXT("Listening");
    // 快步上前或踱步时也保留这次决定的表情，不能等声音开始才看出生气
    else if (!ActionEmotion.IsNone() && ActionEmotion!=TEXT("Calm")) Pose=ActionEmotion;
    else if (Npc->Agent->IsListening() || Npc->Agent->IsDecisionPending() || IsWaitingForSpeechPlayback()) Pose=TEXT("Thinking");
    else if (Npc->Look && Npc->Look->IsAfterBrush()) Pose=TEXT("Angry");
    TMap<FName,float> Targets;
    if (const auto* Settings=Expressions->Poses.Find(Pose)) Targets=Settings->MorphWeights;
    // 眼睛沿用动画蓝图正在跟随的目标，头转过去后只补剩余角度
    const auto* LookAnimation=Cast<UDeliveryNpcConversationAnimInstance>(Npc->GetMesh()->GetPostProcessInstance());
    const bool bCanMoveEyes=!Npc->IsKnockedDown() && !Npc->Combat->IsRecovering() && !Npc->Combat->IsStriking() && !Npc->GetHeldVehicle();
    if (bCanMoveEyes && LookAnimation && LookAnimation->LookWeight>.01f)
    {
        const FRotator Direction=LookAnimation->GetHeadRelativeLookDirection(LookAnimation->LookTargetWorld).Rotation();
        const float Yaw=FMath::Clamp(Direction.Yaw/FMath::Max(1.f,Expressions->MaxEyeYaw),-1.f,1.f);
        const float Pitch=FMath::Clamp(Direction.Pitch/FMath::Max(1.f,Expressions->MaxEyePitch),-1.f,1.f);
        Targets.Add(Expressions->LookLeftMorph,FMath::Max(0.f,-Yaw)*LookAnimation->LookWeight);
        Targets.Add(Expressions->LookRightMorph,FMath::Max(0.f,Yaw)*LookAnimation->LookWeight);
        Targets.Add(Expressions->LookUpMorph,FMath::Max(0.f,Pitch)*LookAnimation->LookWeight);
        Targets.Add(Expressions->LookDownMorph,FMath::Max(0.f,-Pitch)*LookAnimation->LookWeight);
    }
    const double Now=ServerTime();
    if (NextBlink==0) NextBlink=Now+FMath::FRandRange(1.f,3.f);
    if (Now>=NextBlink)
    {
        BlinkStarted=Now;
        NextBlink=Now+FMath::FRandRange(FMath::Max(.5,Expressions->BlinkInterval.X),FMath::Max(Expressions->BlinkInterval.X,Expressions->BlinkInterval.Y));
    }
    const float Progress=float((Now-BlinkStarted)/FMath::Max(.1f,Expressions->BlinkDuration));
    float Blink=0;
    if (BlinkStarted>=0 && Progress>=0 && Progress<=1)
    {
        if (Expressions->BlinkCurve) Blink=Expressions->BlinkCurve->GetFloatValue(Progress);
        else if (Progress<.28f) Blink=FMath::SmoothStep(0.f,.28f,Progress);
        else Blink=1.f-FMath::SmoothStep(.28f,1.f,Progress);
    }
    Targets.FindOrAdd(Expressions->BlinkMorph)=FMath::Max(Targets.FindRef(Expressions->BlinkMorph),FMath::Clamp(Blink,0.f,1.f));
    const float EyeOpenWeight=1.f-FMath::Clamp(Targets.FindRef(Expressions->BlinkMorph),0.f,1.f);
    for (const auto& Pair:Targets) ExpressionWeights.FindOrAdd(Pair.Key);
    for (auto& Pair:ExpressionWeights)
    {
        // 直接使用曲线控制眼睛开合，保证闭眼时能完全合拢
        const float Target=FMath::Clamp(Targets.FindRef(Pair.Key),0.f,1.f);
        const bool bBlink=Pair.Key==Expressions->BlinkMorph;
        Pair.Value=bBlink?Target:FMath::FInterpTo(Pair.Value,Target,DeltaTime,Expressions->BlendSpeed);
        // 闭眼时减小其他眼部变化，防止眨眼和情绪同时压缩眼睛造成翻转
        // 保留原来的表情过渡进度，睁眼后继续显示当前情绪
        // 眉毛保持当前表情，眨眼只调整眼睛
        const bool bKeepWeight=bBlink || Pair.Key==TEXT("BrowsAngry");
        const float DisplayWeight=bKeepWeight?Pair.Value:Pair.Value*EyeOpenWeight;
        if (!Pair.Key.IsNone()) Npc->GetMesh()->SetMorphTarget(Pair.Key,DisplayWeight);
    }
}
